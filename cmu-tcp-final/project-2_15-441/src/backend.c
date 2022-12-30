/**
 * This file implements the CMU-TCP backend. The backend runs in a different
 * thread and handles all the socket operations separately from the application.
 *
 * This is where most of your code should go. Feel free to modify any function
 * in this file.
 */

#include "backend.h"

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

/**
 * Tells if a given sequence number has been acknowledged by the socket.
 *
 * @param sock The socket to check for acknowledgements.
 * @param seq Sequence number to check.
 *
 * @return 1 if the sequence number has been acknowledged, 0 otherwise.
 */
int has_been_acked(cmu_socket_t *sock, uint32_t seq) {
  int result;
  while (pthread_mutex_lock(&(sock->window.ack_lock)) != 0) {
  }
  result = after(sock->window.last_ack_received, seq);
  pthread_mutex_unlock(&(sock->window.ack_lock));
  return result;
}

void handle_message(cmu_socket_t *sock, uint8_t *pkt) {
  cmu_tcp_header_t *hdr = (cmu_tcp_header_t *)pkt;
  uint8_t flags = get_flags(hdr);

  switch (flags) {
    // 如果 ack 已经存在了
    case ACK_FLAG_MASK: {
      uint32_t ack = get_ack(hdr);
      if (after(ack, sock->window.last_ack_received)) {
        sock->window.last_ack_received = ack;
      }
      break;
    }
    default: {
      // 发送仅有 tcp 头部的包
      socklen_t conn_len = sizeof(sock->conn);
      uint32_t seq = sock->window.last_ack_received;

      // No payload.
      uint8_t *payload = NULL;
      uint16_t payload_len = 0;

      // No extension.
      uint16_t ext_len = 0;
      uint8_t *ext_data = NULL;

      uint16_t src = sock->my_port;
      uint16_t dst = ntohs(sock->conn.sin_port);
      uint32_t ack = get_seq(hdr) + get_payload_len(pkt);
      uint16_t hlen = sizeof(cmu_tcp_header_t);
      uint16_t plen = hlen + payload_len;
      uint8_t flags = ACK_FLAG_MASK;
      uint16_t adv_window = 1;
      uint8_t *response_packet =
          create_packet(src, dst, seq, ack, hlen, plen, flags, adv_window,
                        ext_len, ext_data, payload, payload_len);
      // udp 发送
      sendto(sock->socket, response_packet, plen, 0,
             (struct sockaddr *)&(sock->conn), conn_len);
      free(response_packet);

      seq = get_seq(hdr);

      if (seq == sock->window.next_seq_expected) {
        sock->window.next_seq_expected = seq + get_payload_len(pkt);
        payload_len = get_payload_len(pkt);
        payload = get_payload(pkt);

        // Make sure there is enough space in the buffer to store the payload.
        sock->received_buf =
            realloc(sock->received_buf, sock->received_len + payload_len);
        memcpy(sock->received_buf + sock->received_len, payload, payload_len);
        sock->received_len += payload_len;
      }
    }
  }
}

cmu_tcp_header_t *check_for_data(cmu_socket_t *sock, cmu_read_mode_t flags) {
  cmu_tcp_header_t hdr; // 接收到的 tcp 头部
  uint8_t *pkt; 
  socklen_t conn_len = sizeof(sock->conn); // 源缓冲区长度
  ssize_t len = 0;  // 接收到的字节数（接收tcp头部时）
  uint32_t plen = 0, buf_size = 0, n = 0;

  while (pthread_mutex_lock(&(sock->recv_lock)) != 0) {
  }
  // 等待接收 tcp 头部大小的信息
  switch (flags) {
    // 等待直到数据传输到
    case NO_FLAG:
      // recvfrom MSG_PEEK 接收后保留数据
      // 参数分别为: socket描述字 接收缓冲区 缓冲区长度 flag标志位 源地址指针 源缓冲区长度
      // 返回接收到的字节数 否则失败-1
      len = recvfrom(sock->socket, &hdr, sizeof(cmu_tcp_header_t), MSG_PEEK,
                     (struct sockaddr *)&(sock->conn), &conn_len);
      break;
    // 带有超时限制的等待
    case TIMEOUT: {
      // Using `poll` here so that we can specify a timeout.
      // poll 非阻塞等待 3s
      struct pollfd ack_fd;
      ack_fd.fd = sock->socket;
      ack_fd.events = POLLIN;
      // Timeout after 3 seconds.
      if (poll(&ack_fd, 1, 3000) <= 0) {
        break;
      }
    }
    // 不进行等待
    // Fallthrough.
    case NO_WAIT:
      // recvfrom MSG_DONTWAIT recv不会进行阻塞 即不会进行等待
      len = recvfrom(sock->socket, &hdr, sizeof(cmu_tcp_header_t),
                     MSG_DONTWAIT | MSG_PEEK, (struct sockaddr *)&(sock->conn),
                     &conn_len);
      break;
    default:
      perror("ERROR unknown flag");
  }
  cmu_tcp_header_t *header = malloc(sizeof(cmu_tcp_header_t));
  memcpy(header, &hdr, sizeof(cmu_tcp_header_t));
  
  // 如果取得了一个 tcp 头部大小的文件 就进行解读
  if (len >= (ssize_t)sizeof(cmu_tcp_header_t)) {
    // 从头部解读包的大小
    plen = get_plen(&hdr);
    pkt = malloc(plen);
    // 读取包
    while (buf_size < plen) {
      n = recvfrom(sock->socket, pkt + buf_size, plen - buf_size, 0,
                   (struct sockaddr *)&(sock->conn), &conn_len);
      buf_size = buf_size + n;
    }
    // 处理收到的包
    // handle_message(sock, pkt);
    free(pkt);
  }
  pthread_mutex_unlock(&(sock->recv_lock));
  return header;
}

// FIXME: 使用滑动窗口策略替换 single_send
void single_send(cmu_socket_t *sock, uint8_t *data, int buf_len) {
  uint8_t *msg;
  uint8_t *data_offset = data;
  size_t conn_len = sizeof(sock->conn);

  int sockfd = sock->socket;
  if (buf_len > 0) {
    while (buf_len != 0) {
      // 取出 data 中的数据 大小取决于 buf_len 和 MSS
      uint16_t payload_len = MIN((uint16_t)buf_len, (uint16_t)MSS);

      uint16_t src = sock->my_port;
      uint16_t dst = ntohs(sock->conn.sin_port);
      uint32_t seq = sock->window.last_ack_received;
      uint32_t ack = sock->window.next_seq_expected;
      uint16_t hlen = sizeof(cmu_tcp_header_t);
      uint16_t plen = hlen + payload_len;
      uint8_t flags = 0;
      uint16_t adv_window = 1;
      uint16_t ext_len = 0;
      uint8_t *ext_data = NULL;
      uint8_t *payload = data_offset;

      msg = create_packet(src, dst, seq, ack, hlen, plen, flags, adv_window,
                          ext_len, ext_data, payload, payload_len);
      buf_len -= payload_len;

      while (1) {
        // stop and wait 实现
        // 使用 udp 发送包
        sendto(sockfd, msg, plen, 0, (struct sockaddr *)&(sock->conn), conn_len);
        // 超时重发 收到 3次 ack 立刻重发
        check_for_data(sock, TIMEOUT);
        // 查看 ack 判断是否收到包
        if (has_been_acked(sock, seq)) {
          break;
        }
      }

      data_offset += payload_len;
    }
  }
}

void *begin_backend(void *in) {
  cmu_socket_t *sock = (cmu_socket_t *)in;
  int death, buf_len, send_signal;
  uint8_t *data;

  // TODO: 滑动窗口

  window_init(sock);

  while (1) {
    // printf("while ...\n");
    while(pthread_mutex_lock(&(sock->death_lock)) !=  0);
		death = sock->dying;
		pthread_mutex_unlock(&(sock->death_lock));
		while(pthread_mutex_lock(&(sock->send_lock)) != 0);
		if(death && sock->state == CLOSED){
			printf("exited....\n");
			break;
		}
		if((sock->state == CLOSED)){
			// 通知上层可以读取数据,打破上层读取的循环 
    		pthread_cond_signal(&(sock->wait_cond));  
		}
		else{
			window_routine(sock);
		}
		pthread_mutex_unlock(&(sock->send_lock));
		window_check_for_data(sock, NO_WAIT);
  }
  slide_window_close(&sock->window);
  pthread_exit(NULL);
  return NULL;
}

void window_init(cmu_socket_t *sock){
    window_t *win = &sock->window;
    win->seq_expect = win->last_seq_received;
    win->dup_ack_num = 0;
    win->LAR = 0;
    win->LFS = 0;
    win->DAT = 0;
    win->state = WINDOW_DEAULT;
    win->advice_window = get_window_size(WINDOW_INITIAL_ADVERTISED);
    win->my_advice_window = get_window_size(WINDOW_INITIAL_ADVERTISED);
    win->TimeoutInterval = 1000000;
    win->EstimatedRTT = 1000000;
    win->DevRTT = 0;
    win->send_seq = -1;
    win->recv_buffer_header.next = NULL;

    time_out(0,win);
    last_time_wait(0,sock);
}

void copy_string_to_buffer(cmu_socket_t* sock){
    char *data = sock->sending_buf;
    int len = sock->sending_len;
    int start = sock->window.DAT % WINDOW_INITIAL_WINDOW_SIZE;
    int buf_len = len;
    if(start + len > WINDOW_INITIAL_WINDOW_SIZE){
        buf_len = WINDOW_INITIAL_WINDOW_SIZE - start;
        memcpy(sock->window.send_buffer + start, data, buf_len);
    }
    else{
        memcpy(sock->window.send_buffer + start, data, buf_len);
    }
    sock->sending_len -= buf_len;
    if(sock->sending_len != 0){
        uint8_t *buf = malloc(sock->sending_len);
        memcpy(buf,data+buf_len,sock->sending_len);
        free(sock->sending_buf);
        sock->sending_buf = buf;
    }
    else{
        free(sock->sending_buf);
        sock->sending_buf = NULL;
    }
    sock->window.DAT += buf_len;
}

int copy_string_from_buffer(window_t *win, int idx, char *data, int max_len){
    idx = idx % MAX_BUFFER_SIZE;
    int len = min(win->DAT-idx,max_len);
    int start = idx % MAX_BUFFER_SIZE;
    if(start + len > MAX_BUFFER_SIZE){
        int temp = MAX_BUFFER_SIZE-start;
        memcpy(data,win->send_buffer+start, temp);
        memcpy(data+temp,win->send_buffer,len-temp);
    }
    else{
        memcpy(data,win->send_buffer+start, len);
    }
    return len;
}

void window_routine(cmu_socket_t *sock){
    // 检查缓冲区是否有数据，如果有数据转移至发送窗口内
    int buf_len = sock->sending_len;
    // printf("buF_len = %d\n", buf_len);
    if(buf_len > 0 && (sock->window.DAT == sock->window.LAR)){
        copy_string_to_buffer(sock);
        printf("copy to window buffer success!\n");
    }

    if(sock->window.DAT > sock->window.LAR){
        // printf("有数据需要发送 .. DAT %d, LAR %d\n", sock->window.DAT, sock->window.LAR);
        window_send(sock);
    }
    if(sock->window.DAT == sock->window.LAR && sock->state == CLOSE_WAIT){
        uint16_t src = sock->my_port;
        uint16_t dst = sock->their_port; //uint16_t dst = ntohs(sock->conn.sin_port);
        uint8_t *rsp = create_packet(src, dst, 
                sock->window.last_ack_received,
                sock->window.last_seq_received, 
                sizeof(cmu_tcp_header_t), sizeof(cmu_tcp_header_t), ACK_FLAG_MASK|FIN_FLAG_MASK,
                        0, 0, NULL, NULL, 0);
        sendto(sock->socket, rsp, sizeof(cmu_tcp_header_t), 0, 
                    (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
        free(rsp);
        sock->state = LAST_ACK;
        printf("确认挥手 FIN ack %d\n", sock->window.last_seq_received);
    }
}

void window_send(cmu_socket_t *sock){
    // printf("sending ...\n");
    window_t *win = &sock->window;
	uint8_t *msg;
    uint8_t *data;
    sigset_t mask;

    sigemptyset(&mask);       // 将信号集合设置为空
    sigaddset(&mask,SIGALRM); // 加入中断SIGALRM信号
	int plen;
	size_t conn_len = sizeof(sock->conn);

	uint32_t ack; // 当前发送的序列号
     uint32_t mkpkt_seq = win->last_ack_received; // 当前打包的seq
    int buf_len,adv_len = MSS;

    if(win->DAT == win->LAR){
        printf("window send finished!\n");
        return;
    }
    ack = win->last_seq_received;

    sigprocmask(0 /* SIG_BLOCK */ ,&mask,NULL); // 堵塞超时信号，防止超时信号干扰当前的发送

    // 如果数据已经全部发送了，等待ACK或者超时
    if((win->DAT == win->LFS)&&(win->state == WINDOW_DEAULT)){
        printf("数据已经全部发送了! 正在等待 ACK 或 超时 ...\n");
        win->state = WINDOW_SEND_END;
        // sleep(1);
    }

    // 检查接收窗口是否满
    if((win->LFS + MSS - win->LAR > (int)win->advice_window)&&win->state == WINDOW_DEAULT){
        if(win->advice_window == 0){
            adv_len = 1;
        }
        else if(win->advice_window < MSS){
            adv_len =  win->advice_window;
        }
        else{
            win->state = WINDOW_WAIT_ACK;
        }
    }
    
    switch(win->state){
        case WINDOW_DEAULT: 
            buf_len = win->DAT - win->LFS;
            buf_len = (buf_len <= adv_len)?buf_len:adv_len;

            plen = sizeof(cmu_tcp_header_t) + buf_len;
            data = (char *)malloc(buf_len);
            copy_string_from_buffer(win,win->LFS,data,buf_len);
            // printf("content: %s\n", win->send_buffer);
  
            mkpkt_seq = (win->last_ack_received + (win->LFS - win->LAR))%MAX_SEQ_NUM;
            uint16_t src = sock->my_port;
            uint16_t dst = sock->their_port; //uint16_t dst = ntohs(sock->conn.sin_port);
            printf("正常发包 ...... size: %d seq: %d\n", plen, mkpkt_seq);
            msg = create_packet(src, dst, 
                mkpkt_seq, ack, 
                sizeof(cmu_tcp_header_t), plen, NO_FLAG, win->my_advice_window, 0, NULL,
                data, buf_len);
            
            sendto(sock->socket, msg, plen, 0, (struct sockaddr*) &(sock->conn), conn_len);

            free(msg);
            free(data);
            msg = NULL;
            data = NULL;

            if(!win->timer){
                set_timer(win->TimeoutInterval/1000000,win->TimeoutInterval%1000000,(void (*)(int))time_out);
                win->timer = 1;
            }
            if(win->send_seq == -1){
                win->send_seq = mkpkt_seq + buf_len;
                gettimeofday(&win->time_send,NULL);
            }

            win->LFS += buf_len;
            break;

        case WINDOW_RESEND:  

            if(win->LFS > win->LAR){
                buf_len = win->LFS - win->LAR;

                buf_len = (buf_len <= adv_len)?buf_len:adv_len;
                plen = sizeof(cmu_tcp_header_t) + buf_len;
                data = (char *)malloc(buf_len);
                copy_string_from_buffer(win,win->LAR,data,buf_len);
                mkpkt_seq = win->last_ack_received;
                uint16_t src = sock->my_port;
                uint16_t dst = sock->their_port; //uint16_t dst = ntohs(sock->conn.sin_port);
                msg = create_packet(src, dst, 
                    mkpkt_seq, ack, 
                    sizeof(cmu_tcp_header_t), plen, NO_FLAG, win->my_advice_window, 0, NULL,
                    data, buf_len);

                sendto(sock->socket, msg, plen, 0, (struct sockaddr*) &(sock->conn), conn_len);
                free(msg);
                free(data);
                msg = NULL;
                data = NULL;
                unset_timer();
                set_timer(win->TimeoutInterval/1000000,win->TimeoutInterval%1000000,(void (*)(int))time_out);
                win->timer = 1;
  
                if(win->send_seq == -1){
                    win->send_seq = mkpkt_seq + buf_len;
                    gettimeofday(&win->time_send,NULL);
                }
            }
            win->state = WINDOW_DEAULT;
            break;
            
        case WINDOW_TIMEOUT:  
            if(win->LFS > win->LAR){
                buf_len = win->LFS - win->LAR;

                buf_len = (buf_len <= adv_len)?buf_len:adv_len;
     
                plen = sizeof(cmu_tcp_header_t) + buf_len;
                data = (char *)malloc(buf_len);
                copy_string_from_buffer(win,win->LAR,data,buf_len);
                mkpkt_seq = win->last_ack_received;
                uint16_t src = sock->my_port;
                uint16_t dst = sock->their_port; //uint16_t dst = ntohs(sock->conn.sin_port);
                msg = create_packet(src, dst, 
                    mkpkt_seq, ack, 
                    sizeof(cmu_tcp_header_t), plen, NO_FLAG, win->my_advice_window, 0, NULL,
                    data, buf_len);
   
                sendto(sock->socket, msg, plen, 0, (struct sockaddr*) &(sock->conn), conn_len);
                free(msg);
                free(data);
                msg = NULL;
                data = NULL;
                unset_timer();
                set_timer(win->TimeoutInterval/1000000,win->TimeoutInterval%1000000,(void (*)(int))time_out);
                win->timer = 1;
        
                if(win->send_seq == -1){
                    win->send_seq = mkpkt_seq + buf_len;
                    gettimeofday(&win->time_send,NULL);
                }
            }
            win->state = WINDOW_DEAULT;
            break;
        case WINDOW_WAIT_ACK:
            win->state = WINDOW_DEAULT;
            break;
        case WINDOW_SEND_END:
            win->state = WINDOW_DEAULT;
            break;
        default:
            break;
    }
    sigprocmask(1 /* SIG_UNBLOCK */ ,&mask,NULL);
}

void window_check_for_data(cmu_socket_t *sock, cmu_read_mode_t flags){

    uint8_t hdr[sizeof(cmu_tcp_header_t)];
	socklen_t conn_len = sizeof(sock->conn);
	ssize_t len = 0;
	uint32_t plen = 0, buf_size = 0, n = 0;
    char *pkt;
	fd_set ackFD;

	struct timeval time_out;
	time_out.tv_sec = 1;
	time_out.tv_usec = 0;
	while(pthread_mutex_lock(&(sock->recv_lock)) != 0);

    sock->window.my_advice_window = WINDOW_INITIAL_WINDOW_SIZE - sock->received_len;
	switch(flags){
		case NO_FLAG: 
			len = recvfrom(sock->socket, hdr, sizeof(cmu_tcp_header_t), MSG_PEEK,
								(struct sockaddr *) &(sock->conn), &conn_len);
			break;
		case TIMEOUT: 
			FD_ZERO(&ackFD);
			FD_SET(sock->socket, &ackFD);

			if(select(sock->socket+1, &ackFD, NULL, NULL, &time_out) <= 0){
				break;
			}
		case NO_WAIT:
			len = recvfrom(sock->socket, hdr, sizeof(cmu_tcp_header_t), MSG_DONTWAIT | MSG_PEEK,
							 (struct sockaddr *) &(sock->conn), &conn_len);
			break;
		default:
			perror("ERROR unknown flag");
	}
    if(len < (ssize_t)sizeof(cmu_tcp_header_t)){
        /* 暂时没有收到有效数据 */
        // printf("暂时没有有效数据\n");
        // sleep(1);
        // printf("###recv data error %d...\n",(int)len);
    }
    if(len >= (ssize_t)sizeof(cmu_tcp_header_t)){
        // printf("###recv data  %d...\n",(int)len);
        plen = get_plen(hdr);
        pkt = malloc(plen);
        /* 直到包的信息全部收到 */
        while(buf_size < plen ){
            n = recvfrom(sock->socket, pkt + buf_size, plen - buf_size, 
                    NO_FLAG, (struct sockaddr *) &(sock->conn), &conn_len);
            buf_size = buf_size + n;
        }
        window_handle_message(sock, pkt);
    }
    pthread_mutex_unlock(&(sock->recv_lock));
}

void window_handle_message(cmu_socket_t *sock, uint8_t *pkt){

    uint8_t* rsp;
    window_t *win = &sock->window;

    uint8_t flags = get_flags(pkt);
    uint32_t data_len, seq, ack, adv_win;
    socklen_t conn_len = sizeof(sock->conn);
    ack = get_ack(pkt);
    seq = get_seq(pkt);
    adv_win = MAX_NETWORK_BUFFER;

    recv_list *slot;
    recv_list *prev;

    int buf_len;
	switch(flags){
        case ACK_FLAG_MASK: 
            printf("收到了 ACK %d\n", ack);

            if(sock->state == FIN_WAIT_1){
                if(win->last_ack_received < ack){
                    win->last_ack_received = ack;
                }
                if(win->last_seq_received < seq){
                    win->last_seq_received = seq;
                }
                sock->state = FIN_WAIT_2;
            }
            // 处理四次挥手事件 
            if(sock->state == LAST_ACK){
                if(win->last_ack_received < ack){
                    win->last_ack_received = ack; 
                }
                if(win->last_seq_received < seq){
                    win->last_seq_received = seq;
                }
                sock->state = CLOSED;
            }
 
			if(ack > win->last_ack_received){ 
                buf_len = (ack + MAX_SEQ_NUM - win->last_ack_received)%MAX_SEQ_NUM;
				win->last_ack_received = ack; 
                if(win->last_seq_received < seq){
                    win->last_seq_received = seq;
                    win->seq_expect = seq;
                }

                win->advice_window = get_advertised_window(pkt);

                win->LAR += buf_len;

                win->state = WINDOW_DEAULT;

                unset_timer();
    
                if(win->LAR < win->LFS){
                    set_timer(win->TimeoutInterval/1000000,win->TimeoutInterval%1000000,(void (*)(int))time_out);
                    win->timer = 1;
                }
            
                if(ack == win->send_seq){
                    adjust_rtt_value(win);
                }

                win->dup_ack_num = 0;

            }else{ 
                win->dup_ack_num++;
                if(win->dup_ack_num == 3){
                    resend(win);
                    win->dup_ack_num = 0;
                }
            }
            break;

        case FIN_FLAG_MASK:{ // 包含FIN 
            if(win->last_ack_received < ack){
                win->last_ack_received = ack; 
            }
            if(win->last_seq_received < seq){
                win->last_seq_received = seq;
            }
            sock->window.last_seq_received++;
            uint16_t src = sock->my_port;
            uint16_t dst = sock->their_port; 
            rsp = create_packet(src, dst, 
                sock->window.last_ack_received,
                sock->window.last_seq_received, 
                sizeof(cmu_tcp_header_t), sizeof(cmu_tcp_header_t), ACK_FLAG_MASK,
                        win->my_advice_window, 0, NULL, NULL, 0);
            sendto(sock->socket, rsp, sizeof(cmu_tcp_header_t), 0, 
                    (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
            free(rsp);
            sock->state = CLOSE_WAIT;
            printf("FIN 已获得!\n");
            break;
        } 

        case FIN_FLAG_MASK|ACK_FLAG_MASK:{ // 包含 FIN 和 ACK
            if(win->last_ack_received < ack){
                win->last_ack_received = ack;
            }
            if(win->last_seq_received < seq){
                win->last_seq_received = seq;
            }
            sock->window.last_seq_received++;
            uint16_t src = sock->my_port;
            uint16_t dst = sock->their_port; 
            rsp = create_packet(src, dst, 
                sock->window.last_ack_received,
                        sock->window.last_seq_received, 
                sizeof(cmu_tcp_header_t), sizeof(cmu_tcp_header_t), ACK_FLAG_MASK,
                        win->my_advice_window, 0, NULL, NULL, 0);
            sendto(sock->socket, rsp, sizeof(cmu_tcp_header_t), 0, 
                    (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
            free(rsp);
         
            pthread_cond_signal(&(sock->wait_cond));  
            sock->state = TIME_WAIT;
         
            set_timer(win->TimeoutInterval/1000000,win->TimeoutInterval%1000000,(void (*)(int))last_time_wait);
            printf("FIN and ACK 都获得!\n");
            break;
        } 

		default:  
            printf("收到数据！seq = %d, seq_expect = %d\n", seq, win->seq_expect);
      
			if(seq == win->seq_expect){
            printf("收到期待的数据！seq = %d, seq_expect = %d\n", seq, win->seq_expect);
           
                if(win->last_ack_received < ack){
                    win->last_ack_received = ack; 
                }
                if(sock->received_len == WINDOW_INITIAL_WINDOW_SIZE){
                    break;
                }

                data_len = get_plen(pkt) - sizeof(cmu_tcp_header_t);
                adv_win = deliver_data(sock,pkt,data_len);
                win->last_seq_received = seq;
                win->seq_expect = (win->seq_expect + data_len)%MAX_SEQ_NUM;
                slot = win->recv_buffer_header.next;
                prev = &win->recv_buffer_header;

                while((slot != NULL) && (win->seq_expect == get_seq(slot->msg))){
                    data_len = get_plen(slot->msg) - sizeof(cmu_tcp_header_t);
                    adv_win = deliver_data(sock,slot->msg,data_len);
                    win->last_seq_received = get_seq(slot->msg);
                    win->seq_expect = (win->seq_expect + data_len)%MAX_SEQ_NUM;
                    prev->next = slot->next;
                    free(slot->msg);
                    free(slot);
                }
                win->last_seq_received = win->seq_expect;
                win->my_advice_window = WINDOW_INITIAL_WINDOW_SIZE - sock->received_len;
                rsp = create_packet(sock->my_port, ntohs(sock->conn.sin_port),
                    win->last_ack_received, win->seq_expect, 
                    sizeof(cmu_tcp_header_t), sizeof(cmu_tcp_header_t), ACK_FLAG_MASK, win->my_advice_window/*adv_win*/, 0, NULL, NULL, 0);

                sendto(sock->socket, rsp, sizeof(cmu_tcp_header_t), 0, (struct sockaddr*) 
                    &(sock->conn), conn_len);
                printf("发送 ACK 确认包 %d\n", win->seq_expect);
                free(rsp);
                
                pthread_cond_signal(&(sock->wait_cond));  
            }else{
                seq = get_seq(pkt);
                insert_pkt_into_linked_list(&win->recv_buffer_header,pkt);
                rsp = create_packet(sock->my_port, ntohs(sock->conn.sin_port),
                    win->last_ack_received, win->seq_expect, 
                    sizeof(cmu_tcp_header_t), sizeof(cmu_tcp_header_t), ACK_FLAG_MASK, win->my_advice_window, 0, NULL, NULL, 0);
                sendto(sock->socket, rsp, sizeof(cmu_tcp_header_t), 0, 
                    (struct sockaddr*)&(sock->conn), conn_len);
                free(rsp);
            }
			break;
	}
}

int deliver_data(cmu_socket_t *sock, char *pkt, int data_len){
    if(sock->received_buf == NULL){
        sock->received_buf = malloc(data_len);
    }else{
        sock->received_buf = realloc(sock->received_buf, sock->received_len + data_len);
    }

    memcpy(sock->received_buf + sock->received_len, pkt + sizeof(cmu_tcp_header_t), data_len);
    sock->received_len += data_len;
    return WINDOW_INITIAL_WINDOW_SIZE - sock->received_len;
}

void resend(window_t * win){
    win->state = WINDOW_RESEND;
    printf("resending!\n");
}

void last_time_wait(int sig, void *ptr){
    static cmu_socket_t *sock;

    if(sock == NULL){
        sock = (cmu_socket_t *)ptr;
        return;
    }else{
        sock->state = CLOSED;
    }
}

long get_timeval(struct timeval *time){
    return 1l*(time->tv_sec*1000000)+time->tv_usec;
}

void set_timeval(struct timeval *time, long interval){
    long int sec = interval / 1000000;
    long int usec = interval % 1000000;
    time->tv_sec = sec;
    time->tv_usec = usec;
}

void set_timer(int sec, int usec, void (*handler)(int)){
    signal(SIGALRM,handler);
    struct itimerval itv;
    itv.it_interval.tv_sec=0;
    itv.it_interval.tv_usec=0;
    itv.it_value.tv_sec=sec;
    itv.it_value.tv_usec=usec;
    setitimer(ITIMER_REAL,&itv,NULL);
}

void unset_timer(){
    signal(SIGALRM,SIG_DFL);
    struct itimerval itv;
    itv.it_interval.tv_sec=0;
    itv.it_interval.tv_usec=0;
    itv.it_value.tv_sec=0;
    itv.it_value.tv_usec=0;
    setitimer(ITIMER_REAL,&itv,NULL);
}

void time_out(int sig, void *ptr){
    static window_t *win;

    if(win == NULL){
        win = (window_t *)ptr;
        return;
    }else{
        win->timer = 0;
        win->state = WINDOW_TIMEOUT;
        unset_timer();
    }
}

int get_window_size(int frame_size){
    return MSS*frame_size;
}

void insert_pkt_into_linked_list(recv_list *header, char *pkt){
    recv_list *cur = header;
    recv_list *prev;
    recv_list *slot = (recv_list *)malloc(sizeof(recv_list));
    int myseq = get_seq(pkt);
    slot->msg = pkt;
    int flag = 0;
    while(!flag){
        prev = cur;
        if(cur->next == NULL){
            cur->next = slot;
            slot->next = NULL;
            break;
        }
        cur = cur->next;
        int seq = get_seq(cur->msg);

        if(myseq > seq){
            continue;
        }
        else{
            slot->next = cur;
            prev->next = slot;
            break;
        }
    }
    return;
    
}

void adjust_rtt_value(window_t *win){

    struct timeval tim;
    gettimeofday(&tim,NULL);
    long t1 = get_timeval(&tim);
    long t2 = get_timeval(&win->time_send);

    long sampleRTT = t1 - t2;
    win->EstimatedRTT= (long)(((float)(1-ALPHA))*win->EstimatedRTT + ALPHA*sampleRTT);
    win->DevRTT = (long)((1-BETA)*win->DevRTT + BETA*abs(sampleRTT-win->EstimatedRTT));
    win->TimeoutInterval = win->EstimatedRTT + 4*win->DevRTT;
    win->send_seq = -1;
}

int min(int x, int y){
  return x<y?x:y;
}

void slide_window_close(window_t *win){
    signal(SIGALRM,SIG_DFL);
}