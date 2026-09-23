# 基于 i.MX6ULL 的 USB 摄像头 MJPEG 流媒体系统

​	基于 100ask i.MX 6ULL  Pro开发板，通过 USB 摄像头（UVC/MJPEG）采集视频，实现 MJPEG-over-HTTP 推流与本地 LCD 实时预览的嵌入式 Linux 应用开发。

------

## 项目概述

​	在低成本的 ARM Cortex-A7 开发板上实现了一个完整的网络摄像头：从 V4L2 采集一帧 MJPEG 数据，通过环形缓冲区分发给网络消费者和 LCD 消费者，浏览器可直接观看实时画面，同时本地屏幕同步显示。

**核心功能**

| 功能         | 说明                                                         |
| :----------- | :----------------------------------------------------------- |
| 视频采集     | V4L2 引擎，USB UVC 摄像头 MJPEG 格式，mmap 零拷贝 + 4 缓冲池 |
| 流媒体服务器 | MJPEG-over-HTTP（multipart/x-mixed-replace），浏览器直接观看 |
| 本地显示     | LCD framebuffer 实时预览，JPEG 解码 → RGB → 居中显示         |
| 多消费者分发 | 环形缓冲区，网络和 LCD 各自独立消费，互不阻塞                |
| 优雅退出     | 信号处理 + 线程唤醒机制，Ctrl+C 后所有线程安全退出           |
| 客户端管理   | 多浏览器同时连接，客户端列表管理，断开检测                   |

------

## 硬件需求

| 组件     | 说明                                                         |
| :------- | :----------------------------------------------------------- |
| 开发板   | 100ask i.MX6ULL（Cortex-A7 @ 528MHz，512MB DDR3）            |
| 屏幕     | LCD（framebuffer `/dev/fb0`）                                |
| 摄像头   | USB UVC 摄像头，支持 MJPEG 格式                              |
| 操作系统 | Linux 4.9.88（Buildroot，内核需支持 UVC + V4L2 + framebuffer） |

------

## 快速开始

### 交叉编译

bash

```
# 使用Makafile
export ARCH=arm 
export CROSS_COMPILE=arm-buildroot-linux-gnueabihf- 
export PATH=$PATH:/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin # 根据实际情况来填写PATH
make
```



### 部署到开发板

bash

```
adb push camera_test /root/
```



### 开发板运行

bash

```
# MJPEG 模式（摄像头硬件输出 JPEG，推荐）
./camera_test

# 程序启动后：
#   - 监听 8080 端口
#   - LCD 显示实时画面
#   - 浏览器访问 http://<开发板IP>:8080/
```



### 观看视频流

| 客户端     | 地址                             |
| :--------- | :------------------------------- |
| 浏览器首页 | `http://192.168.5.9:8080/`       |
| MJPEG 流   | `http://192.168.5.9:8080/stream` |

------

## 项目结构

text

```
Camera/
├── src/
│   ├── camera/
│   │   ├── usb_camera.c         # V4L2 采集引擎（ioctl、mmap、buffer 管理）
│   │   ├── camera_manager.c     # 摄像头模块抽象层
│	│	└── Makefile			 # camera 目录子Makefile
│   │── display/
│	│	├── framebuffer			 # LCD framebuffer 显示引擎
│	│	├── disp_manager.c		 # 显示模块抽象层
│   │   └── Makefile       		 # display 目录子Makefile 
│   │── draw/
│	│	├── draw.c				 # JPEG 解码 + LCD framebuffer 绘制	
│	│	└── Makefile			 # draw 目录子Makefile
│   ├── network/
│   │   ├── mjpeg_server.c       # MJPEG-over-HTTP 服务器（multipart/x-mixed-replace）
│	│	└── Makefile			 # network 目录子Makefile
│   └── ringbuf/
│   │   └── ringbuf.c            # 环形缓冲区（深/浅拷贝、覆盖策略）
│	│	└── Makefile			 # ringbuf 目录子Makefile
│   └── main.c                   # 程序入口，线程编排（采集/网络/LCD）
├── include/
│   ├── camera/			(camera_manager.h)
│   ├── display/		(disp_manager.h)
│   ├── draw/			(draw.h)
│   ├── network/		(mjpeg_server.h)
│   ├── ringbuf/		(ringbuf.h)
│   └── common/			(logger.h)
├── docs/
│   └── DEBUGGING.md             # 调试问题总结
├── Makefile
├── Makefile.build
└── README.md
```



------

## 系统架构

共 **3 个固定线程** + 每个客户端一个动态线程：

text

```
采集线程 (frame_collect)            网络线程 (mjpeg_server_accept)
========================            ================================
while (USBCameraStatus()):          while (g_server_running):
  DQBUF (阻塞)                        accept()
    ↓                                   ↓
  深拷贝 → frame_A                   创建 client_handler 线程
    ↓                                   ↓
  QBUF (归还 buffer) 				  client_handler:
  	↓								   GetFrameFromRingbuf
  PutFrameToRingbufOverwrite     	   send_jpeg_part   
    ├─→ g_server_ringbuf (深拷贝)   	     ↓
    └─→ g_lcd_ringbuf    (浅拷贝)       循环直至客户端断开             
    ↓
  等待下一帧

LCD 线程 (lcd_display)
======================
while (!g_lcd_ringbuf.stop):
  GetFrameFromRingbuf(&g_lcd_ringbuf)
    ↓
  jpeg_to_rgb (libjpeg 解码)
    ↓
  lcd_show_rgb (居中显示到 framebuffer)
    ↓
  free(rgb)
```



**线程同步**

- 采集线程阻塞在 `VIDIOC_DQBUF`，由驱动唤醒；
- `g_server_ringbuf` 和 `g_lcd_ringbuf` 各自独立，消费者互不干扰；
- `RingbufStop()` 通过设置 `stop = 1` + `pthread_cond_broadcast` 唤醒消费者；
- `MJPEGServerStop()` 通过 `shutdown()` + `close()` 唤醒 accept；
- 主线程 `signal` 同步等待信号，串行执行退出流程。

------

## 关键技术点

| 模块       | 技术实现                                                     |
| :--------- | :----------------------------------------------------------- |
| V4L2 采集  | ioctl + mmap 零拷贝、4 缓冲池                                |
| 环形缓冲区 | 深拷贝/浅拷贝双模式、覆盖策略、条件变量通知                  |
| MJPEG 流   | HTTP multipart/x-mixed-replace、每客户端独立线程             |
| LCD 显示   | libjpeg 解码、RGB 居中偏移计算、framebuffer 直接写入         |
| 多线程退出 | `shutdown` 唤醒 accept、`STREAMOFF` 唤醒 DQBUF、`RingbufStop` 唤醒消费者 |
| 内存管理   | 所有权转移规则、覆盖释放、消费者释放                         |
| 调试       | gdb core 分析、`/proc/<pid>/status` 内存监控                 |

------

## 主要问题与解决

### 1. 多消费者共享环形缓冲区

**问题**：server 线程和 LCD 线程消费速度不一致，共享一个 ringbuf 会引入引用计数、覆盖策略、唤醒开销三重复杂度。

**方案**：各消费者独立 ringbuf，互不干扰，代价是多一份内存副本。

详见 [docs/DEBUGGING.md](https://docs/DEBUGGING.md#多消费者共享环形缓冲区)。

### 2. 内存泄漏（OOM）

**现象**：程序运行几分钟后被 OOM killer 杀掉，VmRSS 从 5MB 涨到 372MB。

**根因**：`PutFrameToRingbufOverwrite` 覆盖旧 slot 时未释放旧的 `data`，每帧泄漏 35KB。

**修复**：覆盖非空`frame` 时 `free(frame->data)`；统一"谁接管谁释放"的所有权规则。

详见 [docs/DEBUGGING.md](https://docs/DEBUGGING.md#内存泄漏)。

### 3. 段错误（客户端一连上就崩）

**现象**：`gdb ./camera_test core` 显示崩在 libc 的 `_IO_str_overflow`。

**根因**：`calloc(1, sizeof(int))` 只分配了 4 字节，但 `Client_Data` 远大于此，后续字段赋值越界写踩坏堆。

**修复**：`calloc(1, sizeof(Client_Data))`；养成 `calloc(1, sizeof(类型名))` 的习惯。

详见 [docs/DEBUGGING.md](https://docs/DEBUGGING.md#段错误)。

### 4. `close` 无法唤醒 accept

**现象**：Ctrl+C 后程序不退出，gdb 显示 accept 线程仍阻塞在 `accept()`。

**根因**：Linux 多线程下，`close(fd)` 不会唤醒阻塞在该 fd 上的 `accept()`——它只递减引用计数，不销毁 socket 对象。

**修复**：`MJPEGServerStop()` 里先 `shutdown(g_listen_fd, SHUT_RDWR)` 唤醒 accept，再 `close`。

详见 [docs/DEBUGGING.md](https://docs/DEBUGGING.md#多消费者共享环形缓冲区)。

------

## 配置

程序当前使用硬编码配置，端口固定为 8080。

------

## 性能数据（i.MX6ULL Cortex-A7）

| 操作             | 耗时                          |
| :--------------- | :---------------------------- |
| MJPEG 推流       | 零拷贝（原样转发）            |
| 运行内存（稳定） | ~5 MB                         |
| 显示帧率上限     | 30fps（`usleep(33333)` 节拍） |

------

## 调试记录

完整的调试过程、问题分析、解决方案见 [docs/DEBUGGING.md](https://docs/DEBUGGING.md)。

包括：

- 多消费者共享 ringbuf 的方案对比
- 内存泄漏的定位（VmRSS 监控 → 覆盖路径漏 free）
- 段错误的排查（gdb core → backtrace → 往前推）
- close 不唤醒 accept 的多线程陷阱
- 深浅拷贝混用的所有权设计反思

------

## 许可证

MIT
