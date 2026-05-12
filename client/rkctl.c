/*
 * rkctl.c - rkd 命令行控制客户端
 *
 * 通过Unix Domain Socket向rkd守护进程发送控制命令。
 *
 * 用法：
 *   rkctl start   <进程名>    启动进程
 *   rkctl stop    <进程名>    停止进程
 *   rkctl restart <进程名>    重启进程
 *   rkctl status  <进程名>    查询进程状态
 *   rkctl list                列出所有进程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

/* IPC协议常量（与ipc.h保持一致） */
#define IPC_SOCK_PATH "/tmp/rkd.sock"
#define IPC_REQ_LEN    64
#define IPC_RSP_LEN    128
#define IPC_NAME_MAX   63

enum {
    CMD_START   = 1,
    CMD_STOP    = 2,
    CMD_RESTART = 3,
    CMD_STATUS  = 4,
    CMD_LIST    = 5,
};

/*
 * parse_cmd() - 将命令字符串转换为命令编号
 * @str: 命令字符串
 * 返回值：命令编号，未知命令返回-1
 */
static int parse_cmd(const char *str)
{
    if (strcmp(str, "start")   == 0) return CMD_START;
    if (strcmp(str, "stop")    == 0) return CMD_STOP;
    if (strcmp(str, "restart") == 0) return CMD_RESTART;
    if (strcmp(str, "status")  == 0) return CMD_STATUS;
    if (strcmp(str, "list")    == 0) return CMD_LIST;
    return -1;
}

/*
 * connect_to_daemon() - 连接到rkd守护进程
 * 返回值：成功返回socket fd，失败返回-1
 */
static int connect_to_daemon(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket() failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect(%s) failed: %s\n",
                IPC_SOCK_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * send_request() - 发送请求到守护进程
 * @fd:  socket文件描述符
 * @cmd: 命令编号
 * @name: 进程名称（CMD_LIST时可为NULL）
 * 返回值：成功返回0，失败返回-1
 */
static int send_request(int fd, int cmd, const char *name)
{
    char buf[IPC_REQ_LEN];
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)cmd;
    if (name) {
        strncpy(buf + 1, name, IPC_NAME_MAX);
    }

    ssize_t total = 0;
    while (total < IPC_REQ_LEN) {
        ssize_t n = write(fd, buf + total, IPC_REQ_LEN - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "write() failed: %s\n", strerror(errno));
            return -1;
        }
        total += n;
    }
    return 0;
}

/*
 * recv_response() - 接收守护进程的响应并显示
 * @fd: socket文件描述符
 * 返回值：成功返回0，失败返回-1
 */
static int recv_response(int fd)
{
    char buf[IPC_RSP_LEN];
    ssize_t total = 0;
    while (total < IPC_RSP_LEN) {
        ssize_t n = read(fd, buf + total, IPC_RSP_LEN - total);
        if (n == 0) {
            fprintf(stderr, "daemon closed connection\n");
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "read() failed: %s\n", strerror(errno));
            return -1;
        }
        total += n;
    }

    /* 解析响应：前4字节为返回码，后124字节为消息 */
    int ret_code;
    memcpy(&ret_code, buf, 4);

    char msg[125];
    memcpy(msg, buf + 4, 124);
    msg[124] = '\0';

    if (ret_code == 0) {
        printf("%s\n", msg);
    } else {
        fprintf(stderr, "error: %s\n", msg);
    }
    return ret_code;
}

/*
 * print_usage() - 打印使用说明
 */
static void print_usage(void)
{
    printf("Usage: rkctl <command> [process_name]\n\n");
    printf("Commands:\n");
    printf("  start   <name>   Start a process\n");
    printf("  stop    <name>   Stop a process\n");
    printf("  restart <name>   Restart a process\n");
    printf("  status  <name>   Show process status\n");
    printf("  list             List all processes\n");
}

/*
 * main() - 客户端入口
 */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    /* 解析命令 */
    int cmd = parse_cmd(argv[1]);
    if (cmd < 0) {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        print_usage();
        return 1;
    }

    /* list 不需要进程名 */
    const char *name = NULL;
    if (cmd != CMD_LIST) {
        if (argc < 3) {
            fprintf(stderr, "process name required for '%s'\n", argv[1]);
            return 1;
        }
        name = argv[2];
    }

    /* 连接到守护进程 */
    int fd = connect_to_daemon();
    if (fd < 0) {
        return 1;
    }

    /* 发送请求 */
    if (send_request(fd, cmd, name) != 0) {
        close(fd);
        return 1;
    }

    /* 接收响应 */
    int ret = recv_response(fd);

    close(fd);
    return ret != 0 ? 1 : 0;
}
