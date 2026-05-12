#ifndef PROCESS_MGR_H
#define PROCESS_MGR_H

#include <pthread.h>
#include <sys/types.h>
#include <time.h>

/*
 * process_mgr.h - 进程管理模块头文件
 *
 * 管理被监控进程的生命周期：启动、停止、重启和状态监控。
 * 支持崩溃自动重启，可配置最大重启次数和重启延迟。
 */

/* 最大可管理的进程数 */
#define MAX_PROCS 32

/* 进程状态 */
typedef enum {
    PROC_RUNNING  = 0,
    PROC_STOPPED  = 1,
    PROC_CRASHED  = 2,
} ProcStatus;

/* 进程配置（对应配置文件中的一行） */
typedef struct {
    char name[64];          /* 进程名称 */
    char cmd[256];          /* 启动命令 */
    int  max_restart;       /* 最大重启次数，-1表示无限 */
    int  restart_delay;     /* 重启延迟（秒） */
} ProcConfig;

/* 进程运行时信息 */
typedef struct {
    ProcConfig config;       /* 关联配置 */
    pid_t      pid;          /* 当前PID，-1表示未运行 */
    ProcStatus status;       /* 当前状态 */
    int        restart_count;/* 累计重启次数 */
    time_t     start_time;   /* 最近一次启动时间 */
    time_t     last_crash_time; /* 最近一次崩溃时间 */
} ProcInfo;

/* 全局进程管理状态 */
typedef struct {
    ProcInfo        procs[MAX_PROCS]; /* 进程数组 */
    int             count;            /* 当前管理的进程数量 */
    pthread_mutex_t lock;             /* 互斥锁，保护并发访问 */
    volatile int    running;          /* 运行标志，0表示退出 */
} ProcManager;

/*
 * pm_init() - 初始化进程管理器
 * @pm: 进程管理器指针
 */
void pm_init(ProcManager *pm);

/*
 * pm_add() - 添加一个进程配置
 * @pm:  进程管理器指针
 * @cfg: 进程配置
 * 返回值：成功返回0，超出最大数量返回-1
 */
int pm_add(ProcManager *pm, const ProcConfig *cfg);

/*
 * pm_find() - 按名称查找进程
 * @pm:   进程管理器指针
 * @name: 进程名称
 * 返回值：找到返回索引，未找到返回-1
 */
int pm_find(ProcManager *pm, const char *name);

/*
 * start_process() - 启动一个进程
 * @proc: 进程信息指针
 * 返回值：成功返回0，失败返回-1
 * 使用fork+exec启动子进程，更新proc中的pid/status/start_time
 */
int start_process(ProcInfo *proc);

/*
 * stop_process() - 停止一个进程
 * @proc: 进程信息指针
 * 返回值：成功返回0，失败返回-1
 * 先发SIGTERM，等待3秒，若未退出则发SIGKILL
 */
int stop_process(ProcInfo *proc);

/*
 * restart_process() - 重启一个进程
 * @proc: 进程信息指针
 * 返回值：成功返回0，失败返回-1
 * 先stop，延迟restart_delay秒后start
 */
int restart_process(ProcInfo *proc);

/*
 * pm_get_status() - 获取进程状态字符串
 * @status: 进程状态枚举值
 * 返回值：状态字符串
 */
const char *pm_status_str(ProcStatus status);

/*
 * monitor_loop() - 监控循环
 * @pm: 进程管理器指针
 * 返回值：正常退出返回0，严重错误返回-1
 * 用waitpid(WNOHANG)轮询所有子进程，检测到崩溃则自动重启
 */
int monitor_loop(ProcManager *pm);

/*
 * pm_cleanup() - 清理所有子进程
 * @pm: 进程管理器指针
 * 向所有运行中的子进程发送SIGTERM
 */
void pm_cleanup(ProcManager *pm);

/*
 * pm_lock() / pm_unlock() - 加锁/解锁进程管理器
 * @pm: 进程管理器指针
 * 用于跨模块的线程安全访问
 */
void pm_lock(ProcManager *pm);
void pm_unlock(ProcManager *pm);

#endif /* PROCESS_MGR_H */
