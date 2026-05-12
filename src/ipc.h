#ifndef IPC_H
#define IPC_H

#include <sys/epoll.h>

/*
 * ipc.h - IPC模块头文件
 *
 * 通过Unix Domain Socket提供控制接口。
 * 使用epoll管理多个客户端连接，二进制协议通信。
 */

/* Unix Domain Socket 路径 */
#define IPC_SOCK_PATH "/tmp/rkd.sock"

/* 最大同时连接的客户端数 */
#define IPC_MAX_CLIENTS 16

/* 最大epoll事件数 */
#define IPC_MAX_EVENTS (IPC_MAX_CLIENTS + 2)

/* 协议：请求 header 大小 */
#define IPC_REQ_LEN 64

/* 协议：响应 header 大小 */
#define IPC_RSP_LEN 128

/* 协议：进程名最大长度 */
#define IPC_NAME_MAX 63

/*
 * 命令定义
 */
enum IpcCommand {
    CMD_START   = 1,  /* 启动进程 */
    CMD_STOP    = 2,  /* 停止进程 */
    CMD_RESTART = 3,  /* 重启进程 */
    CMD_STATUS  = 4,  /* 查询单个进程状态 */
    CMD_LIST    = 5,  /* 列出所有进程 */
};

/*
 * 请求格式（64字节）：
 *   [1 byte: 命令] [63 bytes: 进程名（\0结尾）]
 */
typedef struct {
    unsigned char cmd;
    char          name[IPC_NAME_MAX];
} IpcRequest;

/*
 * 响应格式（128字节）：
 *   [4 bytes: 返回码] [124 bytes: 消息文本（\0结尾）]
 */
typedef struct {
    int  ret_code;
    char msg[124];
} IpcResponse;

/*
 * IPC上下文
 */
typedef struct {
    int         listen_fd;                     /* 监听socket */
    int         epoll_fd;                      /* epoll实例 */
    int         client_fds[IPC_MAX_CLIENTS];   /* 客户端fd列表 */
    int         client_count;                  /* 当前连接数 */
} IpcContext;

#include "process_mgr.h"

/*
 * ipc_init() - 创建Unix Socket并开始监听
 * @ctx: IPC上下文指针
 * 返回值：成功返回0，失败返回-1
 */
int ipc_init(IpcContext *ctx);

/*
 * ipc_event_loop() - epoll事件循环
 * @ctx: IPC上下文指针
 * @pm:  进程管理器指针，用于处理命令
 * 返回值：成功返回0，失败返回-1
 * 该函数会阻塞，直到收到退出信号
 */
int ipc_event_loop(IpcContext *ctx, ProcManager *pm);

/*
 * ipc_cleanup() - 关闭所有连接并删除socket文件
 * @ctx: IPC上下文指针
 */
void ipc_cleanup(IpcContext *ctx);

#endif /* IPC_H */
