#ifndef RINGBUF_H_
#define RINGBUF_H_

#include "camera/camera_manager.h"

#include <pthread.h>

typedef enum
{
    COPY_SHALLOW, // 浅拷贝模式
    COPY_DEEP     // 深拷贝模式
} CopyMode;

typedef struct Ringbuf
{
    CameraFrame *framebuf; // 帧缓冲
    int read;              // 读指针
    int write;             // 写指针
    int size;              // 当前元素数
    int capacity;          // 最大容量
    int stop;              // 状态标志
    CopyMode mode;

    pthread_mutex_t mtx; // 保护frame
    pthread_cond_t cv;   // 新帧到来时广播
} Ringbuf;

int RingbufInit(Ringbuf *buf, int cap);
int PutFrameToRingbuf(Ringbuf *buf, const CameraFrame *frame);
int PutFrameToRingbufOverwrite(Ringbuf *buf, CameraFrame *frame);
int GetFrameFromRingbuf(Ringbuf *buf, CameraFrame *frame);
void RingbufWakeup(Ringbuf *buf);
void RingbufDestroy(Ringbuf *buf);

#endif /* RINGBUF_H_ */
