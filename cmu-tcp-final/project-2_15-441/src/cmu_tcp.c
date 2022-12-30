/**
 * Copyright (C) 2022 Carnegie Mellon University
 *
 * This file is part of the TCP in the Wild course project developed for the
 * Computer Networks course (15-441/641) taught at Carnegie Mellon University.
 *
 * No part of the project may be copied and/or distributed without the express
 * permission of the 15-441/641 course staff.
 *
 *
 * This file implements the high-level API for CMU-TCP sockets.
 */

#include "cmu_tcp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "backend.h"

int cmu_socket(cmu_socket_t *sock, const cmu_socket_type_t socket_type,
               const int port, const char *server_ip) {
  int sockfd, optval;
  socklen_t len;
  struct sockaddr_in conn, my_addr;
  len = sizeof(my_addr);

  // UDP socket 申请
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("ERROR opening socket");
    return EXIT_ERROR;
  }
  
  // 初始化 cmu_socket
  sock->socket = sockfd;
  sock->their_port = port;
  sock->received_buf = NULL;
  sock->received_len = 0;
  pthread_mutex_init(&(sock->recv_lock), NULL);

  sock->sending_buf = NULL;
  sock->sending_len = 0;
  pthread_mutex_init(&(sock->send_lock), NULL);

  sock->type = socket_type;
  sock->dying = 0;
  pthread_mutex_init(&(sock->death_lock), NULL);

  sock->window.last_ack_received = 0;
  sock->window.last_seq_received = 0;
  sock->state = CLOSED;

  // FIXME: Sequence numbers should be randomly initialized. The next expected
  // sequence number should be initialized according to the SYN packet from the
  // other side of the connection.

  pthread_mutex_init(&(sock->window.ack_lock), NULL);

  if (pthread_cond_init(&sock->wait_cond, NULL) != 0) {
    perror("ERROR condition variable not set\n");
    return EXIT_ERROR;
  }

  // 根据参数创建发送或接收的 socket
  switch (socket_type) {
    case TCP_INITIATOR:
      if (server_ip == NULL) {
        perror("ERROR server_ip NULL");
        return EXIT_ERROR;
      }
      // socket 地址初始化
      memset(&conn, 0, sizeof(conn));
      conn.sin_family = AF_INET;
      conn.sin_addr.s_addr = inet_addr(server_ip);
      conn.sin_port = htons(port);
      sock->conn = conn;

      // 本机地址初始化
      my_addr.sin_family = AF_INET;
      my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
      my_addr.sin_port = 0;

      // 通信地址绑定 socket
      if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0) {
        perror("ERROR on binding");
        return EXIT_ERROR;
      }

      break;

    case TCP_LISTENER:
      // socket 地址初始化
      memset(&conn, 0, sizeof(conn));
      conn.sin_family = AF_INET;
      conn.sin_addr.s_addr = htonl(INADDR_ANY);
      conn.sin_port = htons((uint16_t)port);

      optval = 1;
      // setsockopt 配置 socket
      // SO_REUSEADDR 允许服务器bind一个地址，即使这个地址当前已经存在已建立的连接
      setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,
                 sizeof(int));
      if (bind(sockfd, (struct sockaddr *)&conn, sizeof(conn)) < 0) {
        perror("ERROR on binding");
        return EXIT_ERROR;
      }
      sock->conn = conn;
      break;

    default:
      perror("Unknown Flag");
      return EXIT_ERROR;
  }
  // listener 没有初始化 my_addr 使用 getsockname 获得
  getsockname(sockfd, (struct sockaddr *)&my_addr, &len);
  // 网络字节顺序转换主机字节顺序
  sock->my_port = ntohs(my_addr.sin_port);
  
  if (tcp_handshake(sock)){
    perror("ERROR on handshake");
    return EXIT_ERROR;
  }

  // 调用后端处理数据
  pthread_create(&(sock->thread_id), NULL, begin_backend, (void *)sock);
  return EXIT_SUCCESS;
}

int cmu_close(cmu_socket_t *sock) {

  // TODO: FIN bonus

  while (1){
    while(pthread_mutex_lock(&(sock->send_lock)) != 0);
    if(((sock->window.DAT == sock->window.LAR) && (sock->sending_len == 0))){
      // printf("closing: DAT: %d, LAR: %d, len: %d\n",sock->window.DAT,sock->window.LAR,sock->sending_len);
      pthread_mutex_unlock(&(sock->send_lock));
      break;
    }
    // printf("close: DAT: %d, LAR: %d, len: %d\n",sock->window.DAT,sock->window.LAR,sock->sending_len);
    pthread_mutex_unlock(&(sock->send_lock));
    sleep(1);
  }
  /* 连接关闭 */
  while(pthread_mutex_lock(&(sock->death_lock)) != 0);
  /* 进入等待结束一阶段 */
  if(sock->state == ESTABLISHED){
    /* 发送FIN包 */
    char *packet = create_packet(sock->my_port, sock->their_port, 
              sock->window.last_ack_received,
              sock->window.last_seq_received, 
              sizeof(cmu_tcp_header_t), sizeof(cmu_tcp_header_t), FIN_FLAG_MASK,
            sock->window.my_advice_window, 0, NULL, NULL, 0);
    sendto(sock->socket, packet, sizeof(cmu_tcp_header_t), 0, 
              (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
    free(packet);
    sock->state = FIN_WAIT_1;
  }
  sock->dying = 1;
  pthread_mutex_unlock(&(sock->death_lock));
  /* 回收线程 */
  pthread_join(sock->thread_id, NULL); 
  /* 释放缓冲区 */
  if(sock != NULL){
    if(sock->received_buf != NULL)
      free(sock->received_buf);
    if(sock->sending_buf != NULL)
      free(sock->sending_buf);
  }
  else{
    perror("ERORR Null scoket\n");
    return EXIT_ERROR;
  }
  /* 关闭UDP socket */
  return close(sock->socket);
}

int cmu_read(cmu_socket_t *sock, void *buf, int length, cmu_read_mode_t flags) {
  uint8_t *new_buf;
  int read_len = 0;

  if (length < 0) {
    perror("ERROR negative length");
    return EXIT_ERROR;
  }

  // 取锁
  while (pthread_mutex_lock(&(sock->recv_lock)) != 0) {
  }

  switch (flags) {
    // 不停等待 没有 break
    case NO_FLAG:
      while (sock->received_len == 0) {
        pthread_cond_wait(&(sock->wait_cond), &(sock->recv_lock));
      }
    // Fall through. 等待结束进入 NO_WAIT
    // 没有数据不等待
    case NO_WAIT:
      // 缓冲区有数据
      if (sock->received_len > 0) {
        if (sock->received_len > length)
          read_len = length;
        else
          read_len = sock->received_len;

        // 拷贝到指定 buf 中
        memcpy(buf, sock->received_buf, read_len);
        // 如果读取的长度 小于缓冲区长度 即没完全读完
        if (read_len < sock->received_len) {
          // 重置接收缓冲区 去掉已读
          new_buf = malloc(sock->received_len - read_len);
          memcpy(new_buf, sock->received_buf + read_len,
                 sock->received_len - read_len);
          free(sock->received_buf);
          sock->received_len -= read_len;
          sock->received_buf = new_buf;
        } else {
          // 否则直接释放
          free(sock->received_buf);
          sock->received_buf = NULL;
          sock->received_len = 0;
        }
      }
      break;
    default:
      perror("ERROR Unknown flag.\n");
      read_len = EXIT_ERROR;
  }
  pthread_mutex_unlock(&(sock->recv_lock));
  return read_len;
}

int cmu_write(cmu_socket_t *sock, const void *buf, int length) {
  while (pthread_mutex_lock(&(sock->send_lock)) != 0) {
  }
  // 缓冲区太小或不存在则申请
  if (sock->sending_buf == NULL)
    sock->sending_buf = malloc(length);
  else
    sock->sending_buf = realloc(sock->sending_buf, length + sock->sending_len);
  // 发送内容写入发送缓冲区
  memcpy(sock->sending_buf + sock->sending_len, buf, length);
  sock->sending_len += length;

  pthread_mutex_unlock(&(sock->send_lock));
  return EXIT_SUCCESS;
}

int tcp_handshake(cmu_socket_t *sock){
  while(sock->state != ESTABLISHED){
    switch (sock->type)
    {
    case TCP_INITIATOR:
      // printf("Initiator State: %d\n", sock->state);
      tcp_handshake_initiator(sock);
      break;
    case TCP_LISTENER:
      // printf("Listener State: %d\n", sock->state);
      tcp_handshake_listener(sock);
      break;
    default:
      break;
    }
  }
  return EXIT_SUCCESS;
}

int tcp_handshake_initiator(cmu_socket_t *sock){
  srand((unsigned)time(NULL));
  uint8_t *packet;
  cmu_tcp_header_t *header;
  uint32_t seq, ack;

  switch (sock->state){

    case CLOSED:{
        seq = rand() % MAX_SEQ_NUM;
        /* SYN */
        uint16_t src = sock->my_port;
        uint16_t dst = ntohs(sock->conn.sin_port);
        packet = create_packet(src, dst,
              seq,0, sizeof(cmu_tcp_header_t), sizeof(cmu_tcp_header_t), SYN_FLAG_MASK,
              0, 0, NULL, NULL, 0);
        sendto(sock->socket, packet, sizeof(cmu_tcp_header_t), 0, 
            (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
        free(packet);
        sock->state = SYN_SENT;
        sock->window.last_ack_received = seq;
        sock->window.last_seq_received = 0;
        #ifdef LOG
          printf("Initiator: handshake 1st. seq = %d\n", seq);
        #endif
        break;
    }
    
    case SYN_SENT:{ /* after send */
        header = check_for_data(sock, TIMEOUT);
        
        if ((get_flags(header)) == (SYN_FLAG_MASK|ACK_FLAG_MASK)) {
          ack = get_seq(header) + 1;
          seq = get_ack(header);
          #ifdef LOG
            printf("Initiator: handshake 2nd received. seq = %d ack = %d\n", ack-1, seq);
          #endif
          sock->window.size = WINDOW_INITIAL_WINDOW_SIZE;
          uint16_t src = sock->my_port;
          uint16_t dst = ntohs(sock->conn.sin_port);
          packet = create_packet(src, dst, seq,
            ack, sizeof(cmu_tcp_header_t), sizeof(cmu_tcp_header_t),ACK_FLAG_MASK,
            WINDOW_INITIAL_WINDOW_SIZE, 0, NULL, NULL, 0);
          sendto(sock->socket, packet, sizeof(cmu_tcp_header_t), 0, 
              (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
          free(packet);
          sock->state = ESTABLISHED;
          sock->window.last_ack_received = ack;
          sock->window.last_seq_received = seq;
          #ifdef LOG
            printf("Initiator: handshake 2nd. seq = %d ack = %d\n", seq, ack);
            printf("Established!\n");
          #endif
        }
        else{
          sock->state = CLOSED;
        }
        free(header);
        break;
    }

    default:
      break;
  }
  return EXIT_SUCCESS;
}

int tcp_handshake_listener(cmu_socket_t *sock){
  srand((unsigned)time(NULL));
  uint8_t *packet;
  cmu_tcp_header_t *header;

  switch (sock->state){

    case CLOSED:
        sock->state = LISTEN;
        #ifdef LOG
          printf("Listener: handshake waiting.\n");
        #endif
        break;

    case LISTEN:{ 
        uint32_t seq;
        header = check_for_data(sock, NO_FLAG);

        if ((get_flags(header)&SYN_FLAG_MASK) == SYN_FLAG_MASK) {
          uint32_t ack = get_seq(header) + 1;
          seq = rand() % MAX_SEQ_NUM;
          #ifdef LOG
            printf("Listener: handshake 1st received. seq = %d\n", ack-1);
          #endif
          uint16_t src = sock->my_port;
          uint16_t dst = ntohs(sock->conn.sin_port);
          packet = create_packet(src, dst, seq,
              ack, sizeof(cmu_tcp_header_t), sizeof(cmu_tcp_header_t), (SYN_FLAG_MASK|ACK_FLAG_MASK),
            WINDOW_INITIAL_WINDOW_SIZE, 0, NULL, NULL, 0);
          sendto(sock->socket, packet, sizeof(cmu_tcp_header_t), 0, 
              (struct sockaddr*) &(sock->conn), sizeof(sock->conn));
          free(packet);
          sock->state = SYN_RCVD;
          sock->window.last_ack_received = ack;
          sock->window.last_seq_received = seq;
          #ifdef LOG
            printf("Listener: handshake 1st. seq = %d ack = %d\n", seq, ack);
          #endif
        }
        free(header);
        break;
    }

    case SYN_RCVD:{
        header = check_for_data(sock, TIMEOUT);
        int flag = ((get_flags(header) & ACK_FLAG_MASK) == ACK_FLAG_MASK);
        uint32_t ack = get_seq(header);
        uint32_t seq = get_ack(header);
        #ifdef LOG
          printf("Listener: handshake 2nd received. seq = %d ack = %d\n", ack, seq);
        #endif
        sock->window.size = WINDOW_INITIAL_WINDOW_SIZE;
        if(flag && ack == sock->window.last_ack_received && seq == sock->window.last_seq_received+1){
          sock->state = ESTABLISHED;
          sock->window.last_ack_received = ack;
          sock->window.last_seq_received = seq;
          #ifdef LOG
            printf("Established!\n");
          #endif
        }
        else{
          sock->state = LISTEN;
        }
        free(header);
    }
  default:
    break;
  }
  return EXIT_SUCCESS;
}