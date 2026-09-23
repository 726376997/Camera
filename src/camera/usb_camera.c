#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <pthread.h>

#include <linux/videodev2.h>

#include "camera/camera_manager.h"
#include "common/logger.h"

static int USBCameraInit(void);
static void USBCameraShowFormat(void);
static int USBCameraSetFormat(int width, int height, int pixelformat);
static int USBCameraReqBuf(void);
static int USBCameraStreamOn(void);
static int USBCameraStreamOff(void);
static int USBCameraGetCameraFrame(CameraFrame *buf, int timeout_ms);
static int USBCameraPutCameraFrame(const CameraFrame *buf);
static int USBCameraCamDevExit(void);

static CameraDevice g_tUSBCamera = {
    .cam_attr.name = "USBCamera",
    .fd = -1,
    .is_streaming = false,
    .frame_seq = 0,
    .format_count = 0,
    .buff_units = NULL,

    .CamDevInit = USBCameraInit,
    .ShowFormat = USBCameraShowFormat,
    .SetFormat = USBCameraSetFormat,
    .ReqBuf = USBCameraReqBuf,
    .StreamOn = USBCameraStreamOn,
    .StreamOff = USBCameraStreamOff,
    .GetCameraFrame = USBCameraGetCameraFrame,
    .PutCameraFrame = USBCameraPutCameraFrame,
    .CamDevExit = USBCameraCamDevExit,
};

static void v4l2_enum_formats(void)
{
    struct v4l2_fmtdesc fmtdesc = {0};
    int i;

    /* 枚举摄像头所支持的所有像素格式以及描述信息 */
    fmtdesc.index = 0;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    while (ioctl(g_tUSBCamera.fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
    {
        i = fmtdesc.index;

        if (i >= g_tUSBCamera.format_count)
        {
            ERR_PRINTF("Too many camera format\n");
            break;
        }

        // 将枚举出来的格式以及描述信息存放在数组中
        g_tUSBCamera.cam_fmts[i].pixelformat = fmtdesc.pixelformat;
        snprintf(g_tUSBCamera.cam_fmts[i].description, DESCRIPTION_LEN, "%s", fmtdesc.description);

        fmtdesc.index++;
    }

    g_tUSBCamera.format_count = fmtdesc.index;
}

static void v4l2_print_formats(void)
{
    struct v4l2_frmsizeenum frmsize = {0};
    struct v4l2_frmivalenum frmival = {0};
    int i;
    int fps;
    frmsize.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    frmival.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (i = 0; i < g_tUSBCamera.format_count; i++)
    {
        printf("format<0x%x>, description<%s>\n", g_tUSBCamera.cam_fmts[i].pixelformat,
               g_tUSBCamera.cam_fmts[i].description);
        /* 枚举出摄像头所支持的所有视频采集分辨率 */
        frmsize.index = 0;
        frmsize.pixel_format = g_tUSBCamera.cam_fmts[i].pixelformat;
        frmival.pixel_format = g_tUSBCamera.cam_fmts[i].pixelformat;

        while (ioctl(g_tUSBCamera.fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0)
        {
            printf("size<%d*%d> ", frmsize.discrete.width, frmsize.discrete.height);
            frmsize.index++;

            /* 获取摄像头视频采集帧率 */
            frmival.index = 0;
            frmival.width = frmsize.discrete.width;
            frmival.height = frmsize.discrete.height;

            while (ioctl(g_tUSBCamera.fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0)
            {
                fps = frmival.discrete.denominator / frmival.discrete.numerator;
                printf("<%dfps>", fps);
                frmival.index++;
            }
            printf("\n");
        }
        printf("\n");
    }
}

static int queue_all_buffers(void)
{
    int i;

    for (i = 0; i < g_tUSBCamera.buffer_count; i++)
    {
        struct v4l2_buffer buf = {0};

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(g_tUSBCamera.fd, VIDIOC_QBUF, &buf) < 0)
        {
            ERR_PRINTF("ioctl error: VIDIOC_QBUF[%d]: %s\n", i, strerror(errno));
            return -1;
        }
        else
        {
            DBG_PRINTF("QUERYBUF[%d] flags=0x%x queued=%d\n",
                       i, buf.flags, !!(buf.flags & V4L2_BUF_FLAG_QUEUED));
        }

        g_tUSBCamera.buff_units[i].is_in_queue = true;
    }

    return 0;
}

static int dequeue_buffer(struct v4l2_buffer *buf, int timeout_ms)
{
    (void)timeout_ms; // 阻塞 DQBUF 本身不支持超时，参数保留只为接口兼容

    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;

    if (!g_tUSBCamera.is_streaming)
    {
        return -1;
    }

    /* 阻塞等待一帧，直到有 buffer 可出队或被信号打断 */
    if (ioctl(g_tUSBCamera.fd, VIDIOC_DQBUF, buf) < 0)
    {
        if (!g_tUSBCamera.is_streaming)
        {
            // 流已停，正常退出
            DBG_PRINTF("DQBUF after streamoff: %s\n", strerror(errno));
            return -1;
        }

        ERR_PRINTF("VIDIOC_DQBUF failed: %s\n", strerror(errno));
        return -1;
    }

    if (buf->index >= (unsigned int)g_tUSBCamera.buffer_count)
    {
        ERR_PRINTF("invalid buf index %u\n", buf->index);
        /* 出错了要把 buffer 还回去，否则越来越少 */
        ioctl(g_tUSBCamera.fd, VIDIOC_QBUF, buf);
        return -1;
    }

    g_tUSBCamera.buff_units[buf->index].is_in_queue = false;
    return 0;
}

#if 0
static int dequeue_buffer(struct v4l2_buffer *buf, int timeout_ms)
{
    DBG_PRINTF("dequeue_buffer select, tid=%ld fd=%d\n",
               (long)pthread_self(), g_tUSBCamera.fd);
    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;

    if (timeout_ms <= 0)
    {
        ERR_PRINTF("invalid timeout_ms= %d\n", timeout_ms);
        return -1;
    }

    DBG_PRINTF("select on fd=%d\n", g_tUSBCamera.fd);

    if (g_tUSBCamera.is_streaming)
    {
        if (ioctl(g_tUSBCamera.fd, VIDIOC_DQBUF, buf) == 0)
        {
            DBG_PRINTF("prime DQBUF ok, bytesused=%u\n", buf->bytesused);
            ioctl(g_tUSBCamera.fd, VIDIOC_QBUF, buf);
        }
    }

    while (g_tUSBCamera.is_streaming)
    {
        // 使用 select 实现超时
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(g_tUSBCamera.fd, &fds);

        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(g_tUSBCamera.fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue; // 超时继续
            }

            ERR_PRINTF("select: %s\n", strerror(errno));
            return -1;
        }
        if (ret == 0)
        {
            ERR_PRINTF("select timeout: no frame in %d ms\n", timeout_ms);
            continue; // 超时继续
        }

        if (ioctl(g_tUSBCamera.fd, VIDIOC_DQBUF, buf) < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
                continue;
            }

            ERR_PRINTF("VIDIOC_DQBUF failed: %s\n", strerror(errno));
            return -1;
        }

        if (buf->index >= (unsigned int)g_tUSBCamera.buffer_count)
        {
            ERR_PRINTF("invalid buf index %u\n", buf->index);
            return -1;
        }
        g_tUSBCamera.buff_units[buf->index].is_in_queue = false;
    }

    return -1;
}
#endif

static int map_buffers(void)
{
    int i;

    for (i = 0; i < g_tUSBCamera.buffer_count; i++)
    {
        struct v4l2_buffer buf = {0};

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(g_tUSBCamera.fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            ERR_PRINTF("VIDIOC_QUERYBUF[%d] failed: %s", i, strerror(errno));
            return -1;
        }

        g_tUSBCamera.buff_units[i].start = mmap(NULL, buf.length,
                                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                                g_tUSBCamera.fd, buf.m.offset);
        if (MAP_FAILED == g_tUSBCamera.buff_units[i].start)
        {
            ERR_PRINTF("mmap error: %s", strerror(errno));
            g_tUSBCamera.buff_units[i].start = NULL;
            return -1;
        }

        g_tUSBCamera.buff_units[i].length = buf.length;
        g_tUSBCamera.buff_units[i].index = i;
        g_tUSBCamera.buff_units[i].is_in_queue = false;
    }

    return 0;
}

static int unmap_buffers(void)
{
    if (!g_tUSBCamera.buff_units)
    {
        ERR_PRINTF("no buffer needs to be unmmap\n");
        return 0;
    }

    for (int i = 0; i < g_tUSBCamera.buffer_count; i++)
    {
        if (g_tUSBCamera.buff_units[i].start && g_tUSBCamera.buff_units[i].start != MAP_FAILED)
        {
            munmap(g_tUSBCamera.buff_units[i].start, g_tUSBCamera.buff_units[i].length);
            g_tUSBCamera.buff_units[i].start = NULL;
        }
    }

    free(g_tUSBCamera.buff_units);
    g_tUSBCamera.buff_units = NULL;
    g_tUSBCamera.buffer_count = 0;

    // 释放 V4L2 驱动侧缓冲区资源，否则后续 VIDIOC_S_FMT 会返回 EBUSY
    if (g_tUSBCamera.fd >= 0)
    {
        struct v4l2_requestbuffers req = {0};

        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(g_tUSBCamera.fd, VIDIOC_REQBUFS, &req) < 0)
        {
            ERR_PRINTF("VIDIOC_REQBUFS(0) failed: %s", strerror(errno));
            return -1;
        }
    }

    return 0;
}

static int USBCameraInit(void)
{
    struct v4l2_capability cap = {0};
    char *device = "/dev/video1";
    /* 打开摄像头 */
    g_tUSBCamera.fd = open(device, O_RDWR);
    if (g_tUSBCamera.fd < 0)
    {
        ERR_PRINTF("open %s error: %s\n", device, strerror(errno));
        return -1;
    }
    DBG_PRINTF("open %s -> fd=%d\n", device, g_tUSBCamera.fd);

    /* 查询设备功能 */
    if (ioctl(g_tUSBCamera.fd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        ERR_PRINTF("query capabilities failed\n");
        close(g_tUSBCamera.fd);
        g_tUSBCamera.fd = -1;
        return -1;
    }

    /* 判断是否是视频采集设备 */
    if (!(V4L2_CAP_VIDEO_CAPTURE & cap.capabilities))
    {
        ERR_PRINTF("Error: %s: No capture video device!\n", device);
        close(g_tUSBCamera.fd);
        g_tUSBCamera.fd = -1;
        return -1;
    }

    /* 检查是否支持 streaming I/O */
    if (!(V4L2_CAP_STREAMING & cap.capabilities))
    {
        ERR_PRINTF("Device does not support streaming I/O\n");
        close(g_tUSBCamera.fd);
        g_tUSBCamera.fd = -1;
        return -1;
    }

    return 0;
}

static void USBCameraShowFormat(void)
{
    v4l2_enum_formats();
    v4l2_print_formats();
}

static int USBCameraSetFormat(int width, int height, int pixelformat)
{
    struct v4l2_format fmt = {0};
    struct v4l2_streamparm streamparm = {0};
    /* 设置帧格式 */
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // type 类型
    fmt.fmt.pix.width = width;              // 视频帧宽度
    fmt.fmt.pix.height = height;            // 视频帧高度
    fmt.fmt.pix.pixelformat = pixelformat;  // 像素格式
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(g_tUSBCamera.fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        ERR_PRINTF("ioctl error: VIDIOC_S_FMT: %s\n", strerror(errno));
        return -1;
    }

    /*** 判断是否已经设置为MJPEG 像素格式
    如果没有设置成功表示该设备不支持 MJPEG 像素格式 */
    if (V4L2_PIX_FMT_MJPEG != fmt.fmt.pix.pixelformat)
    {
        ERR_PRINTF("Error: the device does not support MJPEG format!\n");
        return -1;
    }

    // 读取实际设置的值（驱动可能调整了分辨率）
    g_tUSBCamera.cam_attr.width = fmt.fmt.pix.width;             // 获取实际的帧宽度
    g_tUSBCamera.cam_attr.height = fmt.fmt.pix.height;           // 获取实际的帧高度
    g_tUSBCamera.cam_attr.pixelformat = fmt.fmt.pix.pixelformat; // 获取实际的像素格式

    printf("视频帧大小<%d * %d>\n", g_tUSBCamera.cam_attr.width, g_tUSBCamera.cam_attr.height);

    /* 获取 streamparm */
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(g_tUSBCamera.fd, VIDIOC_G_PARM, &streamparm);

    /** 判断是否支持帧率设置 **/
    if (V4L2_CAP_TIMEPERFRAME & streamparm.parm.capture.capability)
    {
        streamparm.parm.capture.timeperframe.numerator = 1;    // 分子
        streamparm.parm.capture.timeperframe.denominator = 30; // 分母，即30fps
        if (ioctl(g_tUSBCamera.fd, VIDIOC_S_PARM, &streamparm) < 0)
        {
            ERR_PRINTF("ioctl error: VIDIOC_S_PARM: %s\n", strerror(errno));
            return -1;
        }
    }

    // 检查 bytesperline，确认无 padding 问题
    ERR_PRINTF("Format set: %dx%d, fmt='%c%c%c%c', stride=%d\n",
               g_tUSBCamera.cam_attr.width, g_tUSBCamera.cam_attr.height,
               (g_tUSBCamera.cam_attr.pixelformat >> 0) & 0xFF, (g_tUSBCamera.cam_attr.pixelformat >> 8) & 0xFF,
               (g_tUSBCamera.cam_attr.pixelformat >> 16) & 0xFF, (g_tUSBCamera.cam_attr.pixelformat >> 24) & 0xFF,
               fmt.fmt.pix.bytesperline);
    return 0;
}

static int USBCameraReqBuf(void)
{
    struct v4l2_requestbuffers reqbuf = {0};

    /* 申请帧缓冲 */
    reqbuf.count = FRAMEBUFFER_COUNT; // 帧缓冲的数量
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(g_tUSBCamera.fd, VIDIOC_REQBUFS, &reqbuf) < 0)
    {
        ERR_PRINTF("ioctl error: VIDIOC_REQBUFS: %s\n", strerror(errno));
        return -1;
    }

    if (reqbuf.count < FRAMEBUFFER_COUNT)
    {
        ERR_PRINTF("Insufficient buffer memory: only %u buffers", reqbuf.count);
        return -1;
    }

    g_tUSBCamera.buff_units = calloc(reqbuf.count, sizeof(BufferUnit)); // calloc会初始化为0，malloc不会
    if (!g_tUSBCamera.buff_units)
    {
        ERR_PRINTF("calloc buff_units failed\n");
        return -1;
    }
    g_tUSBCamera.buffer_count = reqbuf.count;
    DBG_PRINTF("Request %d buffers success\n", reqbuf.count);

    /* 建立内存映射 */
    if (map_buffers() < 0)
    {
        ERR_PRINTF("map_buffers failed\n");
        return -1;
    }

    DBG_PRINTF("map_buffers() success\n");
    /* 入队 */
    if (queue_all_buffers() < 0)
    {
        ERR_PRINTF("queue_all_buffers failed\n");
        return -1;
    }

    DBG_PRINTF("queue_all_buffers() success\n");
    return 0;
}

static int USBCameraStreamOn(void)
{
    if (g_tUSBCamera.is_streaming)
    {
        ERR_PRINTF("Already streaming");
        return 0;
    }

    /* 打开摄像头、摄像头开始采集数据 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g_tUSBCamera.fd, VIDIOC_STREAMON, &type) < 0)
    {
        ERR_PRINTF("ioctl error: VIDIOC_STREAMON: %s\n", strerror(errno));
        return -1;
    }

    g_tUSBCamera.is_streaming = true;
    DBG_PRINTF("Stream on, fd=%d\n", g_tUSBCamera.fd);
    return 0;
}

static int USBCameraStreamOff(void)
{
    enum v4l2_buf_type type;

    // 停止视频采集
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g_tUSBCamera.fd, VIDIOC_STREAMOFF, &type) < 0)
    {
        ERR_PRINTF("VIDIOC_STREAMOFF failed: %s\n", strerror(errno));
        return -1;
    }
    g_tUSBCamera.is_streaming = false;
    return 0;
}

static int USBCameraGetCameraFrame(CameraFrame *buf, int timeout_ms)
{
    int ret;
    struct v4l2_buffer vbuf;
    memset(&vbuf, 0, sizeof(vbuf));

    // 先出队，这样应用层才能正确访问
    ret = dequeue_buffer(&vbuf, timeout_ms);
    if (ret < 0)
    {
        ERR_PRINTF("dequeue_buffer failed\n");
        return -1;
    }

    // 校验缓冲区索引
    if ((vbuf.index >= (unsigned int)g_tUSBCamera.buffer_count) || !g_tUSBCamera.buff_units)
    {
        ERR_PRINTF("Invalid buffer index: %u (nbufs=%d)", vbuf.index, g_tUSBCamera.buffer_count);
        return -1;
    }

    // 校验是否有数据
    if (vbuf.bytesused == 0)
    {
        ERR_PRINTF("zero-length frame\n");
        if (ioctl(g_tUSBCamera.fd, VIDIOC_QBUF, &vbuf) < 0)
        {
            ERR_PRINTF("VIDIOC_QBUF (index=%d) failed: %s", vbuf.index, strerror(errno));
        }
        return -1;
    }

    DBG_PRINTF("buf[%d], byteused=%d", vbuf.index, vbuf.bytesused);

    // 填充 CameraFrameBuffer
    buf->data = g_tUSBCamera.buff_units[vbuf.index].start;
    buf->length = vbuf.bytesused;
    buf->width = g_tUSBCamera.cam_attr.width;
    buf->height = g_tUSBCamera.cam_attr.height;
    buf->pixelformat = (g_tUSBCamera.cam_attr.pixelformat == V4L2_PIX_FMT_YUYV)
                           ? V4L2_PIX_FMT_YUYV
                           : V4L2_PIX_FMT_MJPEG;
    buf->buf_index = vbuf.index;
    buf->frame_seq = g_tUSBCamera.frame_seq++;

    return 0;
}

static int USBCameraPutCameraFrame(const CameraFrame *buf)
{
    int idx = buf->buf_index;
    struct v4l2_buffer vbuf = {0};

    if (!buf || !buf->data)
    {
        ERR_PRINTF("invalid CameraFrame buf\n");
        return -1;
    }

    if (idx < 0 || idx > g_tUSBCamera.buffer_count)
    {
        ERR_PRINTF("putFrame: buffer pointer not found in pool");
        return -1;
    }

    vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf.memory = V4L2_MEMORY_MMAP;
    vbuf.index = idx;

    if (ioctl(g_tUSBCamera.fd, VIDIOC_QBUF, &vbuf) < 0)
    {
        ERR_PRINTF("VIDIOC_QBUF (index=%d) failed: %s", idx, strerror(errno));
        return -1;
    }

    g_tUSBCamera.buff_units[idx].is_in_queue = true;
    return 0;
}

static int USBCameraCamDevExit(void)
{

    // 1. unmmap
    if (unmap_buffers() < 0)
    {
        ERR_PRINTF("unmap_buffers failed\n");
        return -1;
    }

    // 2. 关闭设备
    if (g_tUSBCamera.fd < 0)
    {
        ERR_PRINTF("err: when close, find fd < 0\n");
        return -1;
    }

    close(g_tUSBCamera.fd);
    g_tUSBCamera.fd = -1;
    printf("Capture stopped, resource released and device shut down\n");

    return 0;
}

bool USBCameraStatus(void)
{
    return g_tUSBCamera.is_streaming;
}

void USBCameraRegister(void)
{
    RegisterCamDev(&g_tUSBCamera);
}
