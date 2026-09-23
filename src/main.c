#include "ringbuf/ringbuf.h"
#include "camera/camera_manager.h"
#include "common/logger.h"
#include "network/mjpeg_server.h"
#include "display/disp_manager.h"
#include "draw/draw.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <linux/videodev2.h>

static Ringbuf g_server_ringbuf;            // 网络线程Ringbuf
static Ringbuf g_lcd_ringbuf;               // LCD线程Ringbuf
static volatile sig_atomic_t g_running = 1; // 当值为false时执行退出代码

void *frame_collect_from_camera(void *arg);
void *mjpeg_server_accept(void *arg);
void *lcd_display(void *arg);
void *client_handler(void *arg);
void sig_handler(int sig);

int main(int argc, char *argv[])
{
    int ret;

    /* 设置信号处理函数 */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* 摄像头模块初始化 */
    CameraSystemRegister();
    if (SelectDefaultCamDevAndInit("USBCamera") < 0)
    {
        ERR_PRINTF("SelectDefaultCamDevAndInit() failed\n");
        return -1;
    }
    // ShowFormat();
    if (SetFormat(640, 480, V4L2_PIX_FMT_MJPEG) < 0)
    {
        ERR_PRINTF("SetFormat() failed\n");
        CamDevExit();
        return -1;
    }
    /* 申请内核缓冲区 */
    if (ReqBuf() < 0)
    {
        ERR_PRINTF("ReqBuf() failed\n");
        CamDevExit();
        return -1;
    }
    /* 启动流 */
    if (StreamOn() < 0)
    {
        ERR_PRINTF("StreamOn() failed\n");
        CamDevExit();
        return -1;
    }

    /* 显示系统初始化*/
    DispSystemRegister();
    if (SelectDefaultDispDevAndInit("fb") < 0)
    {
        ERR_PRINTF("SelectDefaultDispDevAndInit() failed\n");
        return -1;
    }
    CleanScreen(0x00000000); // 屏幕背景设置为黑色

    /* 绘制系统初始化 */
    DrawInit();

    /* mjpeg_server初始化 */
    int cap = 20;
    if (MJPEGServerInit() < 0)
    {
        ERR_PRINTF("MJPEGServerCreate() failed\n");
        return -1;
    }
    if (ClientListInit(cap) < 0)
    {
        ERR_PRINTF("ClientListInit() failed\n");
        MJPEGServerStop();
        return -1;
    }

    /* Ringbuf初始化 */
    if (RingbufInit(&g_server_ringbuf, 4) < 0)
    {
        ERR_PRINTF("RingbufInit(&g_server_ringbuf, 4) failed\n");
        return -1;
    }
    if (RingbufInit(&g_lcd_ringbuf, 4) < 0)
    {
        ERR_PRINTF("RingbufInit(&g_server_ringbuf, 4) failed\n");
        return -1;
    }
    g_lcd_ringbuf.mode = COPY_SHALLOW; // lcd环形缓冲区设置为浅拷贝

    /* 创建采集线程 */
    pthread_t frame_collect_tid;
    ret = pthread_create(&frame_collect_tid, NULL, frame_collect_from_camera, NULL);
    if (ret)
    {
        ERR_PRINTF("pthread_create(): %s", strerror(ret));
        goto clean_up;
    }

    /* 创建网路线程 */
    pthread_t mjpeg_server_tid;
    ret = pthread_create(&mjpeg_server_tid, NULL, mjpeg_server_accept, NULL);
    if (ret)
    {
        ERR_PRINTF("pthread_create(): %s", strerror(ret));
        goto clean_up;
    }

    /* 创建LCD线程 */
    pthread_t lcd_tid;
    ret = pthread_create(&lcd_tid, NULL, lcd_display, NULL);
    if (ret)
    {
        ERR_PRINTF("pthread_create(): %s", strerror(ret));
        goto clean_up;
    }

    while (g_running)
    {
        pause(); // 阻塞等待信号
    }

    /* 检测到中断信号后 */
    if (StreamOff() < 0)
    {
        ERR_PRINTF("StreamOff() failed\n");
        RingbufWakeup(&g_server_ringbuf);
        RingbufWakeup(&g_lcd_ringbuf);
        goto clean_up;
    }

    RingbufWakeup(&g_server_ringbuf);
    RingbufWakeup(&g_lcd_ringbuf);

    pthread_join(frame_collect_tid, NULL);
    DBG_PRINTF("frame_collect_thread exit\n");

    MJPEGServerStop(); // 关闭listen_fd，以便accept能退出
    pthread_join(mjpeg_server_tid, NULL);
    DBG_PRINTF("mjpeg_server_thread exit\n");

    pthread_join(lcd_tid, NULL);
    DBG_PRINTF("lcd_thread exit\n");

    /* 运行完毕：清理资源 */
    CamDevExit();
    DispDevExit();
    MJPEGServerExit();
    RingbufDestroy(&g_server_ringbuf);
    RingbufDestroy(&g_lcd_ringbuf);
    return 0;

/* 出错：清理资源 */
clean_up:
    CamDevExit();
    DispDevExit();
    MJPEGServerExit();
    RingbufDestroy(&g_server_ringbuf);
    RingbufDestroy(&g_lcd_ringbuf);
    return -1;
}

/* 采集线程处理函数：
 * 1. 从摄像头获取帧数据
 * 2. 出队
 * 3. 将帧数据深拷贝一份
 * 4. 入队
 * 5. 将深拷贝的数据深拷贝到server的环形缓冲区中
 * 6. 将深拷贝的数据浅拷贝到lcd的环形缓冲区中（所有权交给lcd缓冲区）
 * 无需参数
 */
void *frame_collect_from_camera(void *arg)
{
    DBG_PRINTF("collect thread start, tid=%ld\n", (long)pthread_self());

    /* 将内核缓冲区数据拷贝到Ringbuf */
    CameraFrame frame;
    CameraFrame tmp;

    while (USBCameraStatus())
    {
        DBG_PRINTF("collect loop, tid=%ld\n", (long)pthread_self());
        memset(&frame, 0, sizeof(CameraFrame));
        memset(&tmp, 0, sizeof(CameraFrame));

        if (GetCameraFrame(&tmp, 3000) < 0) // 出队，从采集线程获得frame
        {
            ERR_PRINTF("GetCameraFrame failed\n");
            continue;
        }

        /* 深拷贝frame，并迅速将缓冲入队 */
        frame.data = calloc(tmp.length, sizeof(unsigned char));
        if (!frame.data)
        {
            ERR_PRINTF("calloc: %s", strerror(errno));
            return NULL;
        }
        memcpy(frame.data, tmp.data, tmp.length);

        frame.width = tmp.width;
        frame.height = tmp.height;
        frame.length = tmp.length;
        frame.pixelformat = tmp.pixelformat;
        frame.buf_index = tmp.buf_index;
        frame.frame_seq = tmp.frame_seq;

        if (PutCameraFrame(&tmp) < 0) // 入队
        {
            free(frame.data);
            frame.data = NULL;
            return NULL;
        }

        PutFrameToRingbufOverwrite(&g_server_ringbuf, &frame); // 先server，因为要深拷贝
        PutFrameToRingbufOverwrite(&g_lcd_ringbuf, &frame);    // 后lcd，浅拷贝，如果在server之前可能导致server拷贝出错

        frame.data = NULL; // 不负责释放frame->data，由lcd线程负责
    }
    DBG_PRINTF("collect thread exit, tid=%ld\n", (long)pthread_self());
    return NULL;
}

/* 网络线程处理函数：负责accept客户端，并创建客户端线程
 * 参数：server_fd、Ringbuf
 */
void *mjpeg_server_accept(void *arg)
{
    MJPEGServerAccept(&g_server_ringbuf);
    return NULL;
}

/* LCD显示线程，负责将MJPEG格式转化为RGB，并在LCD上显示
 * 无需参数
 */
void *lcd_display(void *arg)
{
    while (!g_lcd_ringbuf.stop)
    {
        CameraFrame frame = {0};
        /* 会获得frame.data指针的所有权，后续需要负责释放 */
        if (GetFrameFromRingbuf(&g_lcd_ringbuf, &frame) < 0)
        {
            continue;
        }

        unsigned char *rgb = NULL;
        int w = 0, h = 0;
        if (jpeg_to_rgb(frame.data, frame.length,
                        &rgb, &w, &h) == 0)
        {
            lcd_show_rgb(rgb, w, h);
            free(rgb);
            rgb = NULL;
        }

        free(frame.data);
        frame.data = NULL;
    }

    return NULL;
}

// 自定义信号处理函数，用于改变while循环的条件
void sig_handler(int sig)
{
    g_running = 0;
}