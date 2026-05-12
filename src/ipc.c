#include "ipc.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

/*
 * ipc_init() - 创建并绑定Unix Domain Socket，启动监听
 */
int ipc_init(IpcContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        ctx->client_fds[i] = -1;
    }

    /* 删除可能存在的旧socket文件 */
    unlink(IPC_SOCK_PATH);

    /* 创建socket */
    ctx->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
        LOG_ERROR("socket(AF_UNIX) failed: %s", strerror(errno));
        return -1;
    }

    /* 绑定地址 */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind(%s) failed: %s", IPC_SOCK_PATH, strerror(errno));
        close(ctx->listen_fd);
        return -1;
    }

    /* 监听 */
    if (listen(ctx->listen_fd, IPC_MAX_CLIENTS) < 0) {
        LOG_ERROR("listen failed: %s", strerror(errno));
        close(ctx->listen_fd);
        unlink(IPC_SOCK_PATH);
        return -1;
    }

    /* 创建epoll实例 */
    ctx->epoll_fd = epoll_create1(0);
    if (ctx->epoll_fd < 0) {
        LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        close(ctx->listen_fd);
        unlink(IPC_SOCK_PATH);
        return -1;
    }

    /* 将监听fd加入epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = ctx->listen_fd;
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl(listen_fd) failed: %s", strerror(errno));
        close(ctx->epoll_fd);
        close(ctx->listen_fd);
        unlink(IPC_SOCK_PATH);
        return -1;
    }

    LOG_INFO("IPC socket listening on %s", IPC_SOCK_PATH);
    return 0;
}

/*
 * accept_client() - 接受新的客户端连接
 * @ctx: IPC上下文
 * 返回值：成功返回客户端索引，失败返回-1
 */
static int accept_client(IpcContext *ctx)
{
    if (ctx->client_count >= IPC_MAX_CLIENTS) {
        /* 达到最大连接数，拒绝新的连接 */
        int fd = accept(ctx->listen_fd, NULL, NULL);
        if (fd >= 0) close(fd);
        LOG_WARN("max clients reached (%d), rejecting new connection",
                 IPC_MAX_CLIENTS);
        return -1;
    }

    int fd = accept(ctx->listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno != EINTR && errno != EAGAIN) {
            LOG_ERROR("accept failed: %s", strerror(errno));
        }
        return -1;
    }

    /* 加入epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl(client) failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* 记录客户端fd */
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        if (ctx->client_fds[i] < 0) {
            ctx->client_fds[i] = fd;
            ctx->client_count++;
            LOG_INFO("client connected, fd=%d", fd);
            return i;
        }
    }
    /* 不应到达这里 */
    close(fd);
    return -1;
}

/*
 * remove_client() - 断开并清理一个客户端连接
 * @ctx:     IPC上下文
 * @fd:      客户端fd
 */
static void remove_client(IpcContext *ctx, int fd)
{
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        if (ctx->client_fds[i] == fd) {
            ctx->client_fds[i] = -1;
            ctx->client_count--;
            LOG_INFO("client disconnected, fd=%d", fd);
            return;
        }
    }
}

/*
 * read_request() - 从客户端读取一个完整请求
 * @fd:  客户端fd
 * @req: 输出请求结构体
 * 返回值：成功返回0，对端关闭返回1，错误返回-1
 */
static int read_request(int fd, IpcRequest *req)
{
    char buf[IPC_REQ_LEN];
    ssize_t total = 0;
    while (total < IPC_REQ_LEN) {
        ssize_t n = read(fd, buf + total, IPC_REQ_LEN - total);
        if (n == 0) {
            return 1; /* 对端关闭 */
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("read(fd=%d) failed: %s", fd, strerror(errno));
            return -1;
        }
        total += n;
    }
    req->cmd = (unsigned char)buf[0];
    memcpy(req->name, buf + 1, IPC_NAME_MAX);
    req->name[IPC_NAME_MAX - 1] = '\0';
    return 0;
}

/*
 * send_response() - 向客户端发送响应
 * @fd:  客户端fd
 * @rsp: 响应结构体
 * 返回值：成功返回0，失败返回-1
 */
static int send_response(int fd, const IpcResponse *rsp)
{
    char buf[IPC_RSP_LEN];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &rsp->ret_code, 4);
    memcpy(buf + 4, rsp->msg, sizeof(rsp->msg));
    ssize_t total = 0;
    while (total < IPC_RSP_LEN) {
        ssize_t n = write(fd, buf + total, IPC_RSP_LEN - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("write(fd=%d) failed: %s", fd, strerror(errno));
            return -1;
        }
        total += n;
    }
    return 0;
}

/*
 * set_response() - 填充响应结构体
 */
static void set_response(IpcResponse *rsp, int code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_response(IpcResponse *rsp, int code, const char *fmt, ...)
{
    memset(rsp, 0, sizeof(*rsp));
    rsp->ret_code = code;
    va_list args;
    va_start(args, fmt);
    vsnprintf(rsp->msg, sizeof(rsp->msg), fmt, args);
    va_end(args);
}

/*
 * handle_command() - 处理一条命令并发送响应
 * @fd:  客户端fd
 * @req: 请求
 * @pm:  进程管理器
 */
static void handle_command(int fd, const IpcRequest *req, ProcManager *pm)
{
    IpcResponse rsp;

    /* CMD_LIST 不需要进程名 */
    if (req->cmd != CMD_LIST && strlen(req->name) == 0) {
        set_response(&rsp, -1, "process name required");
        send_response(fd, &rsp);
        return;
    }

    pm_lock(pm);

    switch (req->cmd) {
    case CMD_START: {
        int idx = pm_find(pm, req->name);
        if (idx < 0) {
            set_response(&rsp, -1, "process '%s' not found", req->name);
        } else if (pm->procs[idx].pid > 0) {
            set_response(&rsp, 0, "process '%s' is already running, pid=%d",
                         req->name, pm->procs[idx].pid);
        } else if (start_process(&pm->procs[idx]) == 0) {
            set_response(&rsp, 0, "process '%s' started, pid=%d",
                         req->name, pm->procs[idx].pid);
        } else {
            set_response(&rsp, -1, "failed to start '%s'", req->name);
        }
        break;
    }
    case CMD_STOP: {
        int idx = pm_find(pm, req->name);
        if (idx < 0) {
            set_response(&rsp, -1, "process '%s' not found", req->name);
        } else if (stop_process(&pm->procs[idx]) == 0) {
            set_response(&rsp, 0, "process '%s' stopped", req->name);
        } else {
            set_response(&rsp, -1, "failed to stop '%s'", req->name);
        }
        break;
    }
    case CMD_RESTART: {
        int idx = pm_find(pm, req->name);
        if (idx < 0) {
            set_response(&rsp, -1, "process '%s' not found", req->name);
        } else if (restart_process(&pm->procs[idx]) == 0) {
            set_response(&rsp, 0, "process '%s' restarted, pid=%d",
                         req->name, pm->procs[idx].pid);
        } else {
            set_response(&rsp, -1, "failed to restart '%s'", req->name);
        }
        break;
    }
    case CMD_STATUS: {
        int idx = pm_find(pm, req->name);
        if (idx < 0) {
            set_response(&rsp, -1, "process '%s' not found", req->name);
        } else {
            ProcInfo *p = &pm->procs[idx];
            set_response(&rsp, 0, "name=%s status=%s pid=%d restarts=%d",
                         p->config.name, pm_status_str(p->status),
                         p->pid, p->restart_count);
        }
        break;
    }
    case CMD_LIST: {
        char list_buf[120];
        int pos = 0;
        for (int i = 0; i < pm->count && pos < (int)sizeof(list_buf) - 1; i++) {
            ProcInfo *p = &pm->procs[i];
            pos += snprintf(list_buf + pos, sizeof(list_buf) - pos,
                            "%s(%s,%d) ", p->config.name,
                            pm_status_str(p->status), p->pid);
        }
        list_buf[pos > 0 ? pos - 1 : 0] = '\0';
        if (pm->count == 0) {
            set_response(&rsp, 0, "(no processes configured)");
        } else {
            set_response(&rsp, 0, "%s", list_buf);
        }
        break;
    }
    default:
        set_response(&rsp, -1, "unknown command %d", req->cmd);
        break;
    }

    pm_unlock(pm);

    send_response(fd, &rsp);
}

/*
 * ipc_event_loop() - epoll事件循环
 * 阻塞运行直到收到退出信号
 */
int ipc_event_loop(IpcContext *ctx, ProcManager *pm)
{
    struct epoll_event events[IPC_MAX_EVENTS];
    LOG_INFO("IPC event loop started");

    while (pm->running) {
        int nfds = epoll_wait(ctx->epoll_fd, events, IPC_MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait failed: %s", strerror(errno));
            return -1;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == ctx->listen_fd) {
                /* 新的客户端连接 */
                accept_client(ctx);
                continue;
            }

            /* 客户端数据 */
            IpcRequest req;
            int ret = read_request(fd, &req);
            if (ret == 0) {
                handle_command(fd, &req, pm);
            } else {
                /* 出错或对端关闭 */
                remove_client(ctx, fd);
            }
        }
    }

    LOG_INFO("IPC event loop exiting");
    return 0;
}

/*
 * ipc_cleanup() - 关闭IPC并清理资源
 */
void ipc_cleanup(IpcContext *ctx)
{
    /* 关闭所有客户端连接 */
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        if (ctx->client_fds[i] >= 0) {
            close(ctx->client_fds[i]);
            ctx->client_fds[i] = -1;
        }
    }
    /* 关闭监听fd和epoll fd */
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
    }
    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
    }
    /* 删除socket文件 */
    unlink(IPC_SOCK_PATH);
    LOG_INFO("IPC cleaned up");
}
