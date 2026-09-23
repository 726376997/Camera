#ifndef MJPEG_SERVER_H_
#define MJPEG_SERVER_H_

#include "camera/camera_manager.h"
#include "ringbuf/ringbuf.h"

#include <pthread.h>

#define PORT 8080
#define BOUNDARY "frame"
#define MAX_CLIENTS 20

// 在线状态 枚举类型
typedef enum
{
    OFFLINE = 0,
    ONLINE = 1
} Online_status;

// 客户端信息结构
typedef struct
{
    char ip_port[32];
    int sockfd;
    Online_status online_status;
} Client_info;

typedef struct
{
    int cap;  // 最大容量
    int size; // 实际数量
    Client_info *clients;
    pthread_mutex_t mtx;
} Client_list;

typedef struct
{
    Client_info client_info;
    Ringbuf *buf;
} Client_Data;

int MJPEGServerInit(void);
int ClientListInit(int cap);
int MJPEGServerAccept(Ringbuf *buf);
void MJPEGServerStop(void);
void MJPEGServerExit(void);

#endif /* MJPEG_SERVER_H_ */
