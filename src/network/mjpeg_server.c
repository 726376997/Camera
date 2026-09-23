/* 创建服务端（启动端口复用）、初始化客户端列表
 * accept客户端并启动客户端处理线程
 */

#include "network/mjpeg_server.h"
#include "common/logger.h"
#include "ringbuf/ringbuf.h"
#include "camera/camera_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>

static void *client_handler(void *arg);
static int send_all(int fd, const void *buf, size_t len);
static void send_mjpeg_header(int fd);
static int send_jpeg_part(int fd, const CameraFrame *frame);
static int add_client(Client_info *client);
static int find_client(int client_fd);
static void remove_client(int client_fd);

static Client_list g_client_list; // 客户端列表
static int g_listen_fd;
struct sockaddr_in g_server_addr;
static volatile sig_atomic_t g_server_running = 1;

/* 创建mjpeg_server，错误时已清理资源 */
int MJPEGServerInit(void)
{
    // 创建socket
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0)
    {
        ERR_PRINTF("bind: %s\n", strerror(errno));
        return -1;
    }

    // 设置端口复用
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 清空server_addr结构体并赋值
    memset(&g_server_addr, 0, sizeof(struct sockaddr_in));

    g_server_addr.sin_family = AF_INET;
    g_server_addr.sin_addr.s_addr = INADDR_ANY;
    g_server_addr.sin_port = htons(PORT);

    // bind
    if (bind(g_listen_fd, (struct sockaddr *)&g_server_addr, sizeof(struct sockaddr_in)) < 0)
    {
        ERR_PRINTF("bind: %s\n", strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    // listen
    if (listen(g_listen_fd, 10) < 0)
    {
        ERR_PRINTF("listen: %s\n", strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    printf("Server started on port %d\n", PORT);

    return 0;
}

/* 初始化客户端列表 */
int ClientListInit(int cap)
{
    g_client_list.clients = calloc(cap, sizeof(Client_info));
    if (!g_client_list.clients)
    {
        ERR_PRINTF("calloc: %s\n", strerror(errno));
        return -1;
    }
    pthread_mutex_init(&g_client_list.mtx, NULL);
    g_client_list.cap = cap;
    g_client_list.size = 0;

    return 0;
}

/* accept客户端，创建客户端线程并传入一帧数据 */
int MJPEGServerAccept(Ringbuf *buf)
{
    Client_Data *client_data;
    char ip[32];
    int port;
    int ret;
    int count = 1;

    while (g_server_running)
    {
        struct sockaddr_in client_addr = {0};
        socklen_t addr_len = sizeof(client_addr);

        DBG_PRINTF("accept loop: running=%d, fd=%d\n", g_server_running, g_listen_fd);
        /* 连接新客户端 */
        int fd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (fd < 0)
        {
            DBG_PRINTF("accept returned -1, errno=%d\n", errno);
            if (errno == EINTR)
                continue; // 被信号打断
            if (errno == EBADF)
                break; // listen_fd 被关闭
            if (errno == EINVAL)
                break; // shutdown 唤醒
            if (errno == ECONNABORTED)
                break; // shutdown 唤醒
            if (!g_server_running)
                break;
            ERR_PRINTF("accept: %s", strerror(errno));
            continue;
        }
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        port = ntohs(client_addr.sin_port);
        printf("the %d client %s:%d has connected\n", count++, ip, port);

        /* 禁用Nagle算法，降低小数据包的发送延迟 */
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        /* 设置send超时，处理客户端假死情况 */
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        /* 设置recv超时，防止收到中断信号后依旧阻塞 */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* client_data正常情况由client_handler来释放 */
        client_data = calloc(1, sizeof(Client_Data));
        if (!client_data)
        {
            ERR_PRINTF("malloc: %s", strerror(errno));
            close(fd);
            return -1;
        }
        client_data->buf = buf;

        /* 设置client_info */
        snprintf(client_data->client_info.ip_port,
                 sizeof(client_data->client_info.ip_port),
                 "%s:%d", ip, port);
        client_data->client_info.sockfd = fd;

        /* 创建客户端线程 */
        pthread_t client_tid;
        ret = pthread_create(&client_tid, NULL, client_handler, client_data);
        if (ret)
        {
            ERR_PRINTF("pthread_create: %s", strerror(ret));
            free(client_data);
            client_data = NULL;
            close(fd);
            return -1;
        }

        /* 分离线程，自动回收资源 */
        pthread_detach(client_tid);
    }

    return 0;
}

/* 关闭服务器 */
void MJPEGServerStop(void)
{
    printf("MJPEGServerStop called, fd=%d\n", g_listen_fd);
    g_server_running = 0;
    if (g_listen_fd >= 0)
    {
        shutdown(g_listen_fd, SHUT_RDWR); // 1. 主动唤醒accept
        close(g_listen_fd);               // 2. 释放引用
        printf("closed listen_fd\n");
        g_listen_fd = -1;
    }
}

/* 释放资源 */
void MJPEGServerExit(void)
{
    free(g_client_list.clients);
    g_client_list.clients = NULL;
}

/* 循环发送buf，直至全部发送完毕 */
static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    ssize_t ret;
    size_t sent = 0;
    while (sent < len)
    {
        ret = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            ERR_PRINTF("send: %s\n", strerror(errno));
            return -1;
        }

        if (ret == 0)
        {
            printf("the ret of send is 0\n");
            return -1;
        }

        sent += ret;
    }
    return 0;
}

/* 发送响应头 */
static void send_mjpeg_header(int fd)
{
    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    send_all(fd, header, strlen(header));

    DBG_PRINTF("mjpeg header sent, fd=%d\n", fd);
}

/* 发送jpeg */
static int send_jpeg_part(int fd, const CameraFrame *frame)
{
    const unsigned char *jpeg = frame->data;
    size_t len = frame->length;
    char part[128];
    int ret;

    ret = snprintf(part, sizeof(part),
                   "--" BOUNDARY "\r\n"
                   "Content-Type: image/jpeg\r\n"
                   "Content-Length: %zu\r\n"
                   "\r\n",
                   len);

    if (send_all(fd, part, ret) < 0)
    {
        ERR_PRINTF("send_all(fd, part, n) failed\n");
        return -1;
    }

    if (send_all(fd, jpeg, len) < 0)
    {
        ERR_PRINTF("send_all(fd, jpeg, len) failed\n");
        return -1;
    }

    if (send_all(fd, "\r\n", 2) < 0)
    {
        ERR_PRINTF("send_all(fd, \"\r\n\", 2) failed\n");
        return -1;
    }

    DBG_PRINTF("sent jpeg %zu bytes\n", (size_t)frame->length);

    return 0;
}

/* 将新客户端加入客户端列表，需提前设置好client_info的ip_port和fd */
static int add_client(Client_info *client)
{
    int i;
    int index;
    bool is_full = true;

    pthread_mutex_lock(&g_client_list.mtx);
    // 找到client_list中的空位（离线客户端也算空位）
    for (i = 0; i < g_client_list.cap; i++)
    {
        if (!g_client_list.clients[i].online_status)
        {
            index = i;
            is_full = false;
            break;
        }
    }

    // 如果客户端数组已满，且无离线客户端，则发送错误信息
    if (is_full)
    {
        const char *html =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html><body>Client_list is full</body></html>";
        send_all(client->sockfd, html, strlen(html));
        pthread_mutex_unlock(&g_client_list.mtx);
        close(client->sockfd); // close可能会阻塞，如果放到mutex_unlock之前可能会延长占用锁的时间，造成性能浪费
        return -1;
    }

    g_client_list.clients[index] = *client;
    g_client_list.clients[index].online_status = ONLINE;
    g_client_list.size++;
    pthread_mutex_unlock(&g_client_list.mtx);

    return 0;
}

/* 通过 socket 从客户端列表中查找对应的索引，返回 -1 表示未找到 */
static int find_client(int client_fd)
{
    int i;
    if (client_fd > 0)
    {
        for (i = 0; i < MAX_CLIENTS; i++)
        {
            if (g_client_list.clients[i].online_status != OFFLINE && g_client_list.clients[i].sockfd == client_fd)
            {
                return i;
            }
        }
    }

    ERR_PRINTF("don't find client(client_fd=%d)\n", client_fd);
    return -1;
}

/* 从客户端列表中删除客户端 */
static void remove_client(int client_fd)
{
    pthread_mutex_lock(&g_client_list.mtx);
    int idx = find_client(client_fd);
    if (idx >= 0)
    {
        g_client_list.clients[idx].online_status = OFFLINE;
        g_client_list.clients[idx].sockfd = -1;
        g_client_list.size--;
    }
    pthread_mutex_unlock(&g_client_list.mtx);
}

/* 客户端线程处理函数：负责读取客户端http请求，并根据请求将数据通过http协议传输给客户端
 * 参数：Client_Data
 * 负责释放frame.data
 */
static void *client_handler(void *arg)
{
    Client_Data client_data = *(Client_Data *)arg;
    int fd = client_data.client_info.sockfd;
    Ringbuf *buf = client_data.buf;
    free(arg);

    DBG_PRINTF("client_handler fd=%d ip=%s\n", fd, client_data.client_info.ip_port);

    if (fd < 0)
    {
        ERR_PRINTF("invalid fd\n");
        return NULL;
    }

    /* 将客户端加入客户端列表 */
    if (add_client(&client_data.client_info) < 0)
    {
        ERR_PRINTF("add_client() failed\n");
        return NULL;
    }

    // 1. 读 HTTP 请求，解析路径
    char req[2048];
    int ret = recv(fd, req, sizeof(req) - 1, 0);
    if (ret <= 0)
    {
        close(fd);
        client_data.client_info.sockfd = -1;
        return NULL;
    }
    req[ret] = '\0';

    char method[16], path[256], version[16];
    sscanf(req, "%15s %255s %15s", method, path, version);

    // 2. 如果是 /，发 HTML，关闭
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0)
    {
        const char *html =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html><body><img src=\"/stream\"></body></html>";
        send_all(fd, html, strlen(html));
        remove_client(fd);
        close(fd);
        return NULL;
    }

    // 3. 如果是 /stream，发 MJPEG 头，循环从Ringbuf取最新 JPEG 发送
    if (strcmp(method, "GET") == 0 && strcmp(path, "/stream") == 0)
    {
        send_mjpeg_header(fd);
        while (1)
        {
            CameraFrame frame = {0};
            /* 会获得frame.data指针的所有权，后续需要负责释放 */
            if (GetFrameFromRingbuf(buf, &frame) < 0)
            {
                continue;
            }
            if (send_jpeg_part(fd, &frame) < 0)
            {
                free(frame.data);
                frame.data = NULL;
                break;
            }

            usleep(33333); // 30fps，每 33ms 发一张

            /* 负责释放frame.data */
            free(frame.data);
            frame.data = NULL;
        }
        remove_client(fd);
        close(fd);
        return NULL;
    }

    // 4. 客户端断开，关闭 fd并从客户端列表中清除
    if (client_data.client_info.sockfd > 0)
    {
        const char *notfound =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        send_all(fd, notfound, strlen(notfound));
        remove_client(fd);
        close(fd);
    }

    return NULL;
}
