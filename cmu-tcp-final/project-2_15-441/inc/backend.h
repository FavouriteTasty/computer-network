/**
 * This file defines the function signatures for the CMU-TCP backend that should
 * be exposed. The backend runs in a different thread and handles all the socket
 * operations separately from the application.
 */

#ifndef PROJECT_2_15_441_INC_BACKEND_H_
#define PROJECT_2_15_441_INC_BACKEND_H_

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>

#include "cmu_packet.h"
#include "cmu_tcp.h"

#define ALPHA 0.125
#define BETA 0.25
#define MAX_BUFFER_SIZE (1<<15)

/**
 * Updates the socket information to represent the newly received packet.
 * 根据新收到的信息更新 socket
 *
 * In the current stop-and-wait implementation, this function also sends an
 * acknowledgement for the packet.
 * 在 sw 算法中该函数也负责发送 ack
 * 
 * @param sock The socket used for handling packets received.  用于处理收到信息的 socket
 * @param pkt The packet data received by the socket.   收到的信息
 */
void handle_message(cmu_socket_t *sock, uint8_t *pkt);

/**
 * Checks if the socket received any data.
 * 判断 socket 是否收到数据
 * 
 * It first peeks at the header to figure out the length of the packet and then
 * reads the entire packet.
 *
 * @param sock The socket used for receiving data on the connection. 用于接收的 socket
 * @param flags Flags that determine how the socket should wait for data. Check
 *             `cmu_read_mode_t` for more information.
 *             标志位: 判断 socket 如何等待数据
 */
cmu_tcp_header_t *check_for_data(cmu_socket_t *sock, cmu_read_mode_t flags);

/**
 * Breaks up the data into packets and sends a single packet at a time.
 * 对数据分包且每次发送一个包
 *
 * @param sock The socket to use for sending data. 用于发送的 socket
 * @param data The data to be sent. 将要发送的数据
 * @param buf_len The length of the data being sent. 发送数据的长度
 */
void single_send(cmu_socket_t *sock, uint8_t *data, int buf_len);

/**
 * Launches the CMU-TCP backend.
 * 启动 TCP 后端
 *
 * @param in the socket to be used for backend processing. 传入一个指向 cmu_socket 结构体的指针 并对它进行操作
 */
void* begin_backend(void* in);

/**
 * @brief 对窗口参数进行初始化
 * 
 * @param sock 
 */
void window_init(cmu_socket_t *sock);

/**
 * @brief 使用滑动窗口方法
 * 由于滑动窗口每次发送的大小和窗口本身相关所以去掉了 data 和 len 的相关参数 
 * 直接控制结构体内的 window 结构体 
 * 
 * @param sock 
 */
void window_routine(cmu_socket_t *sock);

/**
 * @brief 滑动窗口发送方法
 * 
 * @param sock 
 */
void window_send(cmu_socket_t *sock);

/**
 * @brief 处理收到的内容
 * 
 * @param sock 
 * @param flags
 */
void window_check_for_data(cmu_socket_t *sock, cmu_read_mode_t flags);

/**
 * @brief 处理收到的信息
 * 
 * @param sock 
 */
void window_handle_message(cmu_socket_t *sock, uint8_t *pkt);

void copy_string_to_buffer(cmu_socket_t* sock);

int copy_string_from_buffer(window_t *win, int idx, char *data, int max_len);

int deliver_data(cmu_socket_t *sock, char *pkt, int data_len);

void resend(window_t * win);

void last_time_wait(int sig, void *ptr);

long get_timeval(struct timeval *time);

void set_timeval(struct timeval *time, long interval);

void set_timer(int sec, int usec, void (*handler)(int));

void unset_timer();

void time_out(int sig, void *ptr);

int get_window_size(int frame_size);

void insert_pkt_into_linked_list(recv_list *header, char *pkt);

void adjust_rtt_value(window_t *win);

int min(int x, int y);

void slide_window_close(window_t *win);

#endif  // PROJECT_2_15_441_INC_BACKEND_H_
