#include "ringbuf/ringbuf.h"
#include "common/logger.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* 释放frame->data（calloc出来的图像数据） */
static void FreeDataFromFrame(CameraFrame *frame)
{
    if (frame->data)
    {
        free(frame->data);
        frame->data = NULL;
    }
}

/* condition_copy使用：深拷贝，不改动frame；需要外部持有锁，函数内部不持有锁 */
static int deep_copy(Ringbuf *buf, const CameraFrame *frame)
{
    memset(&buf->framebuf[buf->write], 0, sizeof(CameraFrame));

    /* 深拷贝framebuf */
    buf->framebuf[buf->write].data = calloc(frame->length, sizeof(unsigned char));
    if (!buf->framebuf[buf->write].data)
    {
        ERR_PRINTF("calloc: %s", strerror(errno));
        return -1;
    }
    memcpy(buf->framebuf[buf->write].data, frame->data, frame->length);

    buf->framebuf[buf->write].height = frame->height;
    buf->framebuf[buf->write].width = frame->width;
    buf->framebuf[buf->write].length = frame->length;
    buf->framebuf[buf->write].pixelformat = frame->pixelformat;
    buf->framebuf[buf->write].buf_index = frame->buf_index;
    buf->framebuf[buf->write].frame_seq = frame->frame_seq;

    return 0;
}

/* condition_copy使用：浅拷贝，拷贝完毕将frame->date置空，转移所有权；需要外部持有锁，函数内部不持有锁 */
static void shallow_copy(Ringbuf *buf, CameraFrame *frame)
{
    buf->framebuf[buf->write].data = frame->data;
    frame->data = NULL;

    buf->framebuf[buf->write].height = frame->height;
    buf->framebuf[buf->write].width = frame->width;
    buf->framebuf[buf->write].length = frame->length;
    buf->framebuf[buf->write].pixelformat = frame->pixelformat;
    buf->framebuf[buf->write].buf_index = frame->buf_index;
    buf->framebuf[buf->write].frame_seq = frame->frame_seq;
}

/* PutFrameToRingbufOverwrite使用：根据buf->mode决定是使用深拷贝还是浅拷贝 */
static int condition_copy(Ringbuf *buf, CameraFrame *frame)
{ /* 覆盖最旧的数据 */
    if (buf->mode == COPY_SHALLOW)
    {
        /* 浅拷贝framebuf */
        shallow_copy(buf, frame);
    }
    else if (buf->mode == COPY_DEEP)
    {
        /* 深拷贝 */
        if (deep_copy(buf, frame) < 0)
        {
            ERR_PRINTF("deep_copy failed\n");
            return -1;
        }
    }
    else
    {
        ERR_PRINTF("CopyMode wrong\n");
        return -1;
    }

    return 0;
}

/* 构造函数 */
int RingbufInit(Ringbuf *buf, int cap)
{
    buf->framebuf = (CameraFrame *)calloc(cap, sizeof(CameraFrame));
    if (!buf->framebuf)
    {
        ERR_PRINTF("calloc: %s", strerror(errno));
        return -1;
    }

    pthread_mutex_init(&buf->mtx, NULL);
    pthread_cond_init(&buf->cv, NULL);

    buf->capacity = cap;
    buf->size = 0;
    buf->read = 0;
    buf->write = 0;
    buf->stop = 0;
    buf->mode = COPY_DEEP;

    DBG_PRINTF("Ringbuf init\n");

    return 0;
}

#if 0
/* 从外部frame中获取一帧数据存入Ringbuf */
int PutFrameToRingbuf(Ringbuf *buf, const CameraFrame *frame)
{
    if (!buf || !frame)
    {
        ERR_PRINTF("invalid arg\n");
        return -1;
    }

    pthread_mutex_lock(&buf->mtx);
    if (buf->size >= buf->capacity)
    {
        ERR_PRINTF("Ringbuf is full\n");
        pthread_mutex_unlock(&buf->mtx);
        return -1;
    }

    if (buf->mode == COPY_SHALLOW)
    {
        /* 浅拷贝framebuf */
        shallow_copy(buf, frame);
    }
    else if (buf->mode == COPY_DEEP)
    {
        /* 深拷贝 */
        if (deep_copy(buf, frame) < 0)
        {
            ERR_PRINTF("deep_copy failed\n");
            pthread_mutex_unlock(&buf->mtx);
            return -1;
        }
    }
    else
    {
        ERR_PRINTF("CopyMode wrong\n");
        pthread_mutex_unlock(&buf->mtx);
        return -1;
    }

    buf->write = (buf->write + 1) % buf->capacity;
    buf->size++;

    pthread_mutex_unlock(&buf->mtx);
    pthread_cond_broadcast(&buf->cv); // 有数据后唤醒消费者线程

    return 0;
}
#endif

/* 从外部frame中获取一帧数据存入Ringbuf，缓冲区满则覆盖旧数据，旧数据需要被释放
 * 负责释放
 * 返回1表示覆盖写入，返回0表示追加写入，返回-1表示错误
 */
int PutFrameToRingbufOverwrite(Ringbuf *buf, CameraFrame *frame)
{
    int overwritten = 0;

    if (!buf || !frame)
    {
        ERR_PRINTF("PutFrameToRingbufOverwrite(): invalid arg\n");
        return -1;
    }

    pthread_mutex_lock(&buf->mtx);
    if (buf->size >= buf->capacity)
    {
        /* 覆盖最旧的frame：如果frame里还有数据，释放oldest_frame->data */
        CameraFrame *oldest_frame = &buf->framebuf[buf->read];
        FreeDataFromFrame(oldest_frame);

        if (condition_copy(buf, frame) < 0)
        {
            ERR_PRINTF("condition_copy() failed\n");
            pthread_mutex_unlock(&buf->mtx);
            return -1;
        }

        buf->write = (buf->write + 1) % buf->capacity;
        buf->read = buf->write;

        overwritten = 1;
    }
    else
    {
        if (condition_copy(buf, frame) < 0)
        {
            ERR_PRINTF("condition_copy() failed\n");
            pthread_mutex_unlock(&buf->mtx);
            return -1;
        }

        buf->write = (buf->write + 1) % buf->capacity;
        buf->size++;
    }

    pthread_mutex_unlock(&buf->mtx);
    pthread_cond_broadcast(&buf->cv); // 有数据后唤醒消费者线程

    if (overwritten == 1)
    {
        DBG_PRINTF("Ringbuf is full, overwritten frame");
    }

    return overwritten;
}

/* 将Ringbuf中的一帧数据传输给外部的frame */
int GetFrameFromRingbuf(Ringbuf *buf, CameraFrame *frame)
{
    memset(frame, 0, sizeof(CameraFrame)); // 放在锁外面，减少持有锁的时间

    if (!buf || !frame)
    {
        ERR_PRINTF("GetFrameFromRingbuf(): invalid arg\n");
        return -1;
    }

    pthread_mutex_lock(&buf->mtx);
    while (buf->size == 0 && !buf->stop)
    {
        pthread_cond_wait(&buf->cv, &buf->mtx); // 没有数据时循环等待被唤醒，循环是为了避免虚假唤醒
    }

    /* 被要求停止且没数据，退出 */
    if (buf->stop && buf->size == 0)
    {
        pthread_mutex_unlock(&buf->mtx);
        return -1;
    }

    /* 浅拷贝，转移数据所有权 */
    frame->data = buf->framebuf[buf->read].data;
    buf->framebuf[buf->read].data = NULL;

    frame->height = buf->framebuf[buf->read].height;
    frame->width = buf->framebuf[buf->read].width;
    frame->length = buf->framebuf[buf->read].length;
    frame->pixelformat = buf->framebuf[buf->read].pixelformat;
    frame->buf_index = buf->framebuf[buf->read].buf_index;
    frame->frame_seq = buf->framebuf[buf->read].frame_seq;

    buf->read = (buf->read + 1) % buf->capacity;
    buf->size--;
    pthread_mutex_unlock(&buf->mtx);

    return 0;
}

/* 唤醒所有等待者，唤醒后再destroy */
void RingbufWakeup(Ringbuf *buf)
{
    pthread_mutex_lock(&buf->mtx);
    buf->stop = 1;
    pthread_cond_broadcast(&buf->cv);
    pthread_mutex_unlock(&buf->mtx);
}

/* 析构函数 */
void RingbufDestroy(Ringbuf *buf)
{
    int i;
    pthread_mutex_destroy(&buf->mtx);
    pthread_cond_destroy(&buf->cv);

    for (i = 0; i < buf->capacity; i++)
    {
        FreeDataFromFrame(&buf->framebuf[i]); // 释放所有frame->data
    }
    free(&buf->framebuf);
    buf->framebuf = NULL;

    buf->capacity = 0;
    buf->size = 0;
    buf->read = 0;
    buf->write = 0;
    buf->stop = 0;
}