#ifndef CAMERA_MANAGER_H_
#define CAMERA_MANAGER_H_

#include <stdbool.h>

#define CAMERA_DEVICES_NAME "/dev/video1" /* camera的设备名 */
#define FRAMEBUFFER_COUNT 4               // 帧缓冲数量
#define DESCRIPTION_LEN 32                // camera像素格式描述长度
#define CAMERAFORMAT_COUNT 50             // camera像素格式数量
#define WIDTH 640                         // camera像素宽度
#define HEIGHT 480                        // camera像素高度

/*** 摄像头属性信息 ***/
typedef struct CameraAttribute
{
    char *name;
    int width;       // 选用的像素宽度
    int height;      // 选用的像素高度
    int pixelformat; // 选用的像素格式
} CameraAttr;

/*** 摄像头像素格式及其描述信息 ***/
typedef struct CameraFormat
{
    char description[DESCRIPTION_LEN]; // 字符串描述信息
    int pixelformat;                   // 像素格式
} CameraFormat;

/*** 描述一个帧数据的信息 ***/
typedef struct CameraFrame
{
    unsigned char *data;          // 帧数据
    unsigned int width;           // 视频帧宽度
    unsigned int height;          // 视频帧高度
    int pixelformat;              // 像素格式信息
    unsigned long length;         // 帧数据长度（字节）
    unsigned long long frame_seq; // 记录业务帧序号（递增）

    /* ===== 以下为驱动层内部字段 =====
     * 应用层请勿直接读写，仅供 Camera1GetCameraFB / Camera1PutCameraFB 使用。
     * 归还缓冲区请调用 PutCameraFB，不要手动构造 v4l2_buffer。
     */
    int buf_index; // 记录 V4L2 槽位（内核缓冲区索引），仅用于便捷入队，不应该被上层应用使用

} CameraFrame;

/*** V4L2 mmap 缓冲区单元 ***/
typedef struct BufferUnit
{
    void *start;          // mmap 映射地址
    unsigned long length; // 缓冲区长度（字节）
    int index;            // 缓冲区索引
    bool is_in_queue;     // 是否在 V4L2 队列中
} BufferUnit;

/*** 摄像头设备 ***/
typedef struct CameraDevice
{
    bool is_streaming; // 推流状态
    int fd;
    int format_count;                          // 设备支持的像素格式数量
    int frame_seq;                             // 记录业务帧序号（递增）
    BufferUnit *buff_units;                    // mmap缓冲区单元数组指针
    int buffer_count;                          // mmap缓冲区单元数量
    CameraAttr cam_attr;                       // 设备名称，选用的像素宽度、高度和格式
    CameraFormat cam_fmts[CAMERAFORMAT_COUNT]; // 支持的像素格式

    int (*CamDevInit)(void);                                  // 打开视频设备文件
    void (*ShowFormat)(void);                                 // 显示支持的像素格式、分辨率、fps
    int (*SetFormat)(int width, int height, int pixelformat); // 设置像素格式
    int (*ReqBuf)(void);                                      // 申请帧缓冲
    int (*StreamOn)(void);                                    // 启动录制，mmap
    int (*StreamOff)(void);                                   // 结束录制
    int (*CamDevExit)(void);                                  // munmap, close(fd)
    int (*GetCameraFrame)(CameraFrame *buf, int timeout_ms);  // 向外界提供获取帧缓冲接口
    int (*PutCameraFrame)(const CameraFrame *buf);            // 向外界提供QBUF接口，调用GetCameraFB后必须调用PutCameraFB将缓冲区归还给驱动，应用层只负责使用，不负责释放缓冲区

    struct CameraDevice *ptNext;
} CameraDevice;

void USBCameraRegister(void);
bool USBCameraStatus(void);

void RegisterCamDev(CameraDevice *ptCamDev);
void CameraSystemRegister(void);
int SelectDefaultCamDevAndInit(char *name);
void ShowFormat(void);
int SetFormat(int width, int height, int pixelformat);
int ReqBuf(void);
int StreamOn(void);
int StreamOff(void);
int GetCameraFrame(CameraFrame *buf, int timeout_ms);
int PutCameraFrame(const CameraFrame *buf);
int CamDevExit(void);

#endif /* CAMERA_MANAGER_H_ */
