/*
 * main.c - rkd (RK3568 Process Monitor Daemon) 程序入口
 *
 * 解析命令行参数，加载配置文件，初始化各模块，
 * 启动守护进程、监控线程和IPC服务。
 */

#include "src/log.h"
#include "src/daemon.h"
#include "src/process_mgr.h"
#include "src/ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <getopt.h>

/* 默认配置文件路径 */
#define DEFAULT_CONF "/etc/rkd.conf"

/* 进程管理器实例（静态分配，供信号处理函数访问） */
static ProcManager g_pm_instance;
static ProcManager *g_pm = NULL;

/* 监控线程ID */
static pthread_t g_monitor_tid;

/*
 * monitor_thread() - 监控线程入口
 * @arg: 进程管理器指针
 */
static void *monitor_thread(void *arg)
{
    ProcManager *pm = (ProcManager *)arg;
    monitor_loop(pm);
    return NULL;
}

/*
 * load_config() - 解析配置文件
 * @path: 配置文件路径
 * @pm:   进程管理器指针
 * 返回值：成功加载的进程数，失败返回-1
 *
 * 配置文件格式（每行）：
 *   # 注释行
 *   进程名=启动命令,最大重启次数,重启延迟秒数
 */
static int load_config(const char *path, ProcManager *pm)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("fopen(%s) failed: %s", path, strerror(errno));
        return -1;
    }

    char line[512];
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* 去除行尾换行 */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }

        /* 跳过空行和注释 */
        if (len == 0 || line[0] == '#') {
            continue;
        }

        /* 解析：name=cmd,max_restart,restart_delay */
        char *saveptr;
        char *name = strtok_r(line, "=", &saveptr);
        if (!name) continue;

        ProcConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        strncpy(cfg.name, name, sizeof(cfg.name) - 1);

        /* 解析剩余部分 */
        char *cmd_part = strtok_r(NULL, ",", &saveptr);
        if (!cmd_part) {
            LOG_WARN("invalid config line (missing cmd): %s", line);
            continue;
        }
        strncpy(cfg.cmd, cmd_part, sizeof(cfg.cmd) - 1);

        char *max_str = strtok_r(NULL, ",", &saveptr);
        cfg.max_restart = max_str ? atoi(max_str) : -1;

        char *delay_str = strtok_r(NULL, ",", &saveptr);
        cfg.restart_delay = delay_str ? atoi(delay_str) : 3;

        if (pm_add(pm, &cfg) != 0) {
            LOG_ERROR("failed to add process: %s", cfg.name);
            fclose(fp);
            return -1;
        }
        count++;
    }

    fclose(fp);
    LOG_INFO("loaded %d processes from %s", count, path);
    return count;
}

/*
 * signal_handler() - 处理SIGTERM/SIGINT，触发优雅退出
 */
static void signal_handler(int sig)
{
    LOG_INFO("received signal %d (%s), shutting down...", sig, strsignal(sig));
    if (g_pm) {
        g_pm->running = 0;
    }
}

/*
 * setup_signals() - 安装信号处理器
 * 返回值：成功返回0，失败返回-1
 */
static int setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    /* 不设置SA_RESTART，使得epoll_wait能被信号中断并返回EINTR */
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        LOG_ERROR("sigaction(SIGTERM) failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        LOG_ERROR("sigaction(SIGINT) failed: %s", strerror(errno));
        return -1;
    }
    /* 忽略SIGCHLD，由monitor线程中的waitpid统一处理 */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL);

    return 0;
}

/*
 * print_usage() - 打印使用说明
 */
static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [OPTIONS]\n", prog);
    fprintf(stderr, "RK3568 Process Monitor Daemon\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c, --config FILE   Config file path (default: %s)\n",
            DEFAULT_CONF);
    fprintf(stderr, "  -h, --help          Show this help\n");
    fprintf(stderr, "  -v, --version       Show version\n");
}

/*
 * main() - 程序入口
 */
int main(int argc, char *argv[])
{
    const char *conf_path = DEFAULT_CONF;

    /* 解析命令行参数 */
    static struct option long_opts[] = {
        {"config",  required_argument, 0, 'c'},
        {"help",    no_argument,       0, 'h'},
        {"version", no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:hv", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            conf_path = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'v':
            printf("rkd v1.0.0\n");
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* 初始化日志（在daemonize之前，方便调试） */
    if (log_init() != 0) {
        fprintf(stderr, "failed to initialize log\n");
        return 1;
    }

    LOG_INFO("rkd v1.0.0 starting...");

    /* 将配置文件路径转为绝对路径（daemonize会切换工作目录到/tmp） */
    char abs_conf[512];
    if (realpath(conf_path, abs_conf) == NULL) {
        fprintf(stderr, "config file not found: %s\n", conf_path);
        log_close();
        return 1;
    }

    /* 转变为守护进程 */
    if (daemonize() != 0) {
        LOG_ERROR("daemonize failed");
        log_close();
        return 1;
    }

    /* 设置信号处理 */
    if (setup_signals() != 0) {
        remove_pid_file();
        log_close();
        return 1;
    }

    /* 初始化进程管理器 */
    ProcManager *pm = &g_pm_instance;
    pm_init(pm);
    g_pm = pm;

    /* 加载配置文件（使用绝对路径） */
    if (load_config(abs_conf, pm) < 0) {
        LOG_ERROR("failed to load config from %s", conf_path);
        pm_cleanup(pm);
        remove_pid_file();
        log_close();
        return 1;
    }

    if (pm->count == 0) {
        LOG_WARN("no processes configured, exiting");
        pm_cleanup(pm);
        remove_pid_file();
        log_close();
        return 0;
    }

    /* 启动所有配置的进程 */
    pm_lock(pm);
    for (int i = 0; i < pm->count; i++) {
        start_process(&pm->procs[i]);
    }
    pm_unlock(pm);

    /* 创建监控线程 */
    if (pthread_create(&g_monitor_tid, NULL, monitor_thread, pm) != 0) {
        LOG_ERROR("pthread_create failed: %s", strerror(errno));
        pm_cleanup(pm);
        remove_pid_file();
        log_close();
        return 1;
    }

    /* 初始化IPC */
    IpcContext ipc_ctx;
    if (ipc_init(&ipc_ctx) != 0) {
        LOG_ERROR("ipc_init failed");
        pm->running = 0;
        pthread_join(g_monitor_tid, NULL);
        pm_cleanup(pm);
        remove_pid_file();
        log_close();
        return 1;
    }

    /* 运行IPC事件循环（主线程） */
    LOG_INFO("rkd started successfully, entering event loop");
    ipc_event_loop(&ipc_ctx, pm);

    /* 收到退出信号，开始清理 */
    LOG_INFO("rkd shutting down...");

    /* 等待监控线程退出 */
    pthread_join(g_monitor_tid, NULL);

    /* 清理资源 */
    ipc_cleanup(&ipc_ctx);
    pm_cleanup(pm);
    remove_pid_file();
    log_close();

    LOG_INFO("rkd stopped");
    return 0;
}
