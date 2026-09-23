# 调试记录

​	本文记录我在开发**基于 i.MX6ULL 的 USB 摄像头 MJPEG 流媒体系统**时遇到的四个核心问题及排查过程。项目基于 V4L2 采集、ringbuf 分发、多线程推流，运行在ARM 开发板（韦东山i.MX 6ULL PRO, Linux 4.9.88）上。

> 记录目的：把踩过的坑整理成可复用的排查思路，而不是只记结论。

------



### **主要问题：**

​	**多消费者共享环形缓冲区**、**内存泄露问题**、**段错误**

------



#### 一、**多消费者共享环形缓冲区**

##### 			**1.  问题来源：**

​	server线程和lcd线程消费速度不一致

##### 			**2.  解决方案：**

###### 				**方案1：共用一个ringbuf**

​	**存在的问题：**

​	1）**单 read 指针：**一帧只能被一个消费者取走，另一个永远读不到，不可行。

​	2）每个消费者独立 read 指针：**看起来可行，但引入三个新问题——引用计数、覆盖策略、唤醒开销。

​		a)  引用计数：必须等所有消费者读完才能 free，需添加引用计数`refcount`

​		b)  覆盖策略：慢消费者拖累快消费者，覆盖和释放必须配合 `refcount`

​		c)  唤醒开销：若消费者共享 条件变量 `cv`，每次 broadcast 可能白醒；若消费者独立` cv `，则生产者要判断唤醒谁

###### 				**方案2：各自使用一个ringbuf**

​	每个消费者独享 ringbuf，完全独立，代价是多消耗一些空间。

**本项目选择方案 2**。

------



#### 二、**内存泄漏**

##### 			**1.  现象：**

​	程序可以正常运行，但运行几分钟后会因为内存占用过高而被OOM killer 杀掉

##### 			**2.  问题来源：**

​	`ringbuf` 覆盖旧` frame` 时，没有释放旧` frame` 里的 `data`。每次覆盖丢一帧，30fps 下每秒泄漏约 1MB。

##### 			**3.  设计初衷：**

​	server 线程和 lcd 线程消费速度不同，各需要一个独立`ringbuf`。采集线程每取一帧，要分发给两个`ringbuf`。

- 如果**两个 ringbuf 都用深拷贝**，需要两次深拷贝。
- 为**省一次拷贝**，采取了“深拷贝 + 浅拷贝”的策略：深拷贝给 `server_ringbuf`（独立副本），浅拷贝给 `lcd_ringbuf`（转移所有权）。

##### 	 		**4.  分析数据流向：**

​	基于上面的设计，一帧数据在系统里的完整流向如下：

```
采集线程:
  frame.data ──┐
                │
                ├─→ server_ringbuf（深拷贝，独立副本，ringbuf 管）
                │
                └─→ lcd_ringbuf（浅拷贝，所有权转移，ringbuf 管）
                     
消费者:
  GetFrameFromRingbuf → frame.data 转给消费者，frame 置 NULL
  消费者消费完 → free(frame.data)

ringbuf:
  覆盖时如果 frame.data 非空 → free(frame.data)
```

​	采集线程中通过`camera`模块获取数据，出队后深拷贝一帧数据（`frame_A`，栈上局部变量）随后入队，`frame_A `深拷贝给`server_ringbuf`（`frame_B`），随后浅拷贝给`lcd_ringbuf`（即将所有权转移给`lcd_ringbuf`），结果上来看，采集线程无需释放`frame_A.data`，后续的释放由其他模块操作。

​	 `Ringbuf`模块中，`ringbuf`（`buf`）是通过calloc得到的，后续不再分配因此程序运行期间无需释放，仅在结束时释放。但`buf->framebuf[i].data`（unsigned char*类型）每一次使用都是通过calloc得到的，每次使用完毕后必须要释放，因为每一次写入新数据时data指针的指向会改变，导致无法再释放之前的空间。

- `PutFrameToRingbufOverwrite`函数当发生数据覆盖写入时，如果 `frame` 里还有未释放的 `data`，由 `ringbuf` 释放；如果`frame `已被消费者取走（`data` 置 `NULL`），不用释放；
- `GetFrameFromRingbuf`函数浅拷贝把 `frame` 的` data` 指针转给消费者，`frame` 置 NULL。所有权从` ringbuf` 转移到消费者，消费者消费完负责释放。

##### 			**5.  解决方案：**

​	制定统一规则：深拷贝创建独立副本，被调用者需要负责指针的管理和释放；浅拷贝转移指针所有权，调用者负责指针的管理和释放。

##### 			**6.  总结：**

​	**直接原因：** `PutFrameToRingbufOverwrite` 覆盖旧 `frame` 时没有释放旧 `data`，每次覆盖泄漏一帧（35KB）。30fps 下每秒泄漏 1MB，几分钟后 OOM。

​	**深层原因：** 深拷贝和浅拷贝混用，导致所有权规则不对称——深拷贝路径的 `data` 由 `server_ringbuf `管，浅拷贝路径的 `data `由 `lcd_ringbuf` 接管；释放点又分散在"入队失败 / 覆盖 / 消费完"三处。规则一多，就容易漏。**这次是漏在覆盖路径上。**

​	**设计权衡：** 浅拷贝只省了一次 35KB 的拷贝，换来的是代码逻辑复杂度显著增加、bug 风险上升，**性价比很低**。

​	**改进：**

​		1.  尽量统一拷贝模式，不要在同一份代码里混用深/浅拷贝

​		2.  明确约定"谁分配谁释放、谁接管谁释放"，写进注释或头文件

​		3.  用 `-fsanitize=address` 或 `valgrind` 定期检查，泄漏早发现

------



#### 三、**段错误**

##### 			**1.  现象：**

​	编译通过，程序也可以运行，但客户端一连上就崩溃并报段错误

##### 			**2.  排查过程**

​	1）生成core文件，gdb 打开 core，bt查看调用栈，发现崩在 libc 的 `_IO_str_overflow`（`sprintf` 内部）

​	2）崩在 libc 说明真正的 bug 不在崩溃点，而是之前有堆破坏

​	3）往前推，检查 `Client_Data` 的分配，发现用了 `calloc(1, sizeof(int))`

​	4）改成 `sizeof(Client_Data)`，问题消失

##### 			**3.  问题来源：**

​	`calloc` 分配结构体时用了错误的大小（`sizeof(int)` 而非 `sizeof(Client_Data)`），实际大小小于预期大小，导致后续字段赋值越界写，踩坏堆。

**4.  解决方案**

 	改为`sizeof(实际类型名)`

##### 			**5.  反思：**

​	养成 `calloc(1, sizeof(类型名))` 的习惯；用 `-fsanitize=address` 编译，越界写第一次就能发现；**崩在哪 ≠ 错在哪**，backtrace 是起点不是终点。

------



#### 四、`close` 无法唤醒 `accept`（多线程退出问题）

##### **1.  现象**：

​	Ctrl+C 后程序不退出，accept 线程卡在 `accept()` 上，主线程 `pthread_join` 永远等不到。

##### **2.  排查过程**：

​	gdb `thread apply all bt` 看到 accept 线程阻塞在 `accept()`，但 `MJPEGServerStop()` 里的 `close(g_listen_fd)` 已经执行了。

##### **3.  问题来源**：

​	Linux 多线程下，`close(fd)` 只递减文件描述符的引用计数，不会唤醒阻塞在该 fd 上的 `accept()`。这跟单线程的行为不同——单线程下`close` 会让 `accept` 返回 `EBADF`，多线程下不行。

##### **4.  解决方案**：

​	`MJPEGServerStop()` 里先 `shutdown(g_listen_fd, SHUT_RDWR)`唤醒 accept，再 `close`。

##### **5.  反思**：

​	多线程程序的退出，不能只靠"关资源"，要用能唤醒阻塞系统调用的方式（`shutdown` 唤醒 `accept`、`STREAMOFF` 唤醒 `DQBUF`、`cond_broadcast` 唤醒条件变量等待）。





