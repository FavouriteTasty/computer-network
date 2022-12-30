/**
 * This file defines the API for the CMU TCP implementation.
 */

#ifndef PROJECT_2_15_441_INC_CMU_TCP_H_
#define PROJECT_2_15_441_INC_CMU_TCP_H_

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>

#include "cmu_packet.h"
#include "grading.h"

#define EXIT_SUCCESS 0
#define EXIT_ERROR -1
#define EXIT_FAILURE 1

#define MAX_SEQ_NUM 1000000

typedef enum{
  WINDOW_DEAULT = 0,
  WINDOW_RESEND = 1,
  WINDOW_TIMEOUT = 2,
  WINDOW_WAIT_ACK = 3, 
  WINDOW_SEND_END = 4,
} window_state;

/* 滑窗接收窗口单位 */
typedef struct recv_list {
	uint8_t recv_or_not;
	uint8_t *msg;
	struct recv_list *next; /* 组织成链表的形式 */
} recv_list;

/**
 * 滑动窗口结构体
 */
typedef struct {
  uint32_t next_seq_expected;     // 发送方 seq
  uint32_t last_ack_received;     // 接收方 ack
  uint32_t last_seq_received;
  uint32_t advice_window;
  uint32_t my_advice_window;
  uint32_t seq_expect;
  uint32_t dup_ack_num;

  uint8_t timer;

  pthread_mutex_t ack_lock;       // 多线程 ack

  /* newly added */
  
  uint8_t send_buffer[WINDOW_INITIAL_WINDOW_SIZE+1];

  int DAT;
  int LAR;
  int LFS;
  int send_seq;

  uint32_t lar; // last ack recv
  uint32_t lbs; // last byte sent
  uint32_t offset; // 在 cmu_socket_t 中发送缓冲区的偏移量
  uint32_t size;
  window_state state; // 窗口状态
  struct timeval time_send;
  uint32_t TimeoutInterval;  
	uint32_t EstimatedRTT;  
	uint32_t DevRTT; 

  recv_list recv_buffer_header;
} window_t;

/**
 * CMU-TCP socket types. (DO NOT CHANGE.)
 * socket 类型枚举
 */
typedef enum {
  TCP_INITIATOR = 0,  // 发送方
  TCP_LISTENER = 1,   // 接受方
} cmu_socket_type_t;

/**
 * 
*/
typedef enum{
  CLOSED = 0,
  SYN_SENT = 1,
  ESTABLISHED = 2,
  LISTEN = 3,
  SYN_RCVD = 4,
  FIN_WAIT_1 = 5,
  FIN_WAIT_2 = 6,
  TIME_WAIT = 7,
  CLOSE_WAIT = 8,
  LAST_ACK = 9,
} cmu_tcp_state;

/**
 * This structure holds the state of a socket. 
 * socket 结构体
 */
typedef struct {
  int socket;                     // socket 端口号
  pthread_t thread_id;            // 后端运行的线程号
  uint16_t my_port;               // 本机端口
  
  struct sockaddr_in conn;        // 通讯目标 socket 地址
  uint8_t* received_buf;          // 接收缓冲区
  int received_len;               // 接收数据大小
  pthread_mutex_t recv_lock;      // 接收缓冲区锁
  pthread_cond_t wait_cond;       // 接收缓冲区等待区
  uint8_t* sending_buf;           // 发送缓冲区
  int sending_len;                // 发送数据大小
  cmu_socket_type_t type;         // 发送 or 接收 initiator or listener
  pthread_mutex_t send_lock;      // 发送缓冲区锁
  int dying;                      // 连接是否关闭，默认为 false
  pthread_mutex_t death_lock;     
  window_t window;                // 滑动窗口

  /* newly added */
  cmu_tcp_state state;
  uint16_t their_port;

} cmu_socket_t;

// PJ_CP2 declarations

/**
 * @brief tcp 握手实现
 * 
 * @param sock 指针
 * @return EXIT_ERROR, EXIT_SUCCESS, EXIT_FAILURE 
 */
int tcp_handshake(cmu_socket_t *sock);

/**
 * @brief tcp 发送方握手实现
 * 
 * @param sock 指针
 * @return EXIT_ERROR, EXIT_SUCCESS, EXIT_FAILURE  
 */
int tcp_handshake_initiator(cmu_socket_t *sock);

/**
 * @brief tcp 接收方握手实现
 * 
 * @param sock 指针
 * @return EXIT_ERROR, EXIT_SUCCESS, EXIT_FAILURE 
 */
int tcp_handshake_listener(cmu_socket_t *sock);

/*
 * DO NOT CHANGE THE DECLARATIONS BELOW
 */

/**
 * Read mode flags supported by a CMU-TCP socket.
 * 读取数据模式
 */
typedef enum {
  NO_FLAG = 0,  // Default behavior: block indefinitely until data is available. 直到等到数据
  NO_WAIT,      // Return immediately if no data is available. 没有数据立刻返回
  TIMEOUT,      // Block until data is available or the timeout is reached. 等待数据直到超时
} cmu_read_mode_t;

/**
 * Constructs a CMU-TCP socket.
 *
 * An Initiator socket is used to connect to a Listener socket.
 *
 * @param sock The structure with the socket state. It will be initialized by
 *             this function.
 * @param socket_type Indicates the type of socket: Listener or Initiator.
 * @param port Port to either connect to, or bind to. (Based on socket_type.)
 * @param server_ip IP address of the server to connect to. (Only used if the
 *                 socket is an initiator.)
 *
 * @return 0 on success, -1 on error.
 */
int cmu_socket(cmu_socket_t* sock, const cmu_socket_type_t socket_type,
               const int port, const char* server_ip);

/**
 * Closes a CMU-TCP socket.
 *
 * @param sock The socket to close.
 *
 * @return 0 on success, -1 on error.
 */
int cmu_close(cmu_socket_t* sock);

/**
 * Reads data from a CMU-TCP socket.
 *
 * If there is data available in the socket buffer, it is placed in the
 * destination buffer.
 *
 * @param sock The socket to read from.
 * @param buf The buffer to read into.
 * @param length The maximum number of bytes to read.
 * @param flags Flags that determine how the socket should wait for data. Check
 *             `cmu_read_mode_t` for more information. `TIMEOUT` is not
 *             implemented for CMU-TCP.
 *
 * @return The number of bytes read on success, -1 on error.
 */
int cmu_read(cmu_socket_t* sock, void* buf, const int length,
             cmu_read_mode_t flags);

/**
 * Writes data to a CMU-TCP socket.
 *
 * @param sock The socket to write to.
 * @param buf The data to write.
 * @param length The number of bytes to write.
 *
 * @return 0 on success, -1 on error.
 */
int cmu_write(cmu_socket_t* sock, const void* buf, int length);

/*
 * You can declare more functions after this point if you need to.
 */

#endif  // PROJECT_2_15_441_INC_CMU_TCP_H_
