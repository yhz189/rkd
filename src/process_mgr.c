#include "process_mgr.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

/* stop_process 等待SIGTERM生效的超时时间（秒） */
#define STOP_TIMEOUT 3

/*
 * pm_init() - 初始化进程管理器，包括互斥锁
 */
void pm_init(ProcManager *pm)
{
    memset(pm, 0, sizeof(*pm));
    pthread_mutex_init(&pm->lock, NULL);
    pm->running = 1;
}

/*
 * pm_lock() / pm_unlock() - 供外部模块使用的锁接口
 */
void pm_lock(ProcManager *pm)
{
    pthread_mutex_lock(&pm->lock);
}

void pm_unlock(ProcManager *pm)
{
    pthread_mutex_unlock(&pm->lock);
}

/*
 * pm_add() - 向管理器添加一个进程配置
 */
int pm_add(ProcManager *pm, const ProcConfig *cfg)
{
    if (pm->count >= MAX_PROCS) {
        LOG_ERROR("too many processes (max=%d)", MAX_PROCS);
        return -1;
    }
    ProcInfo *info = &pm->procs[pm->count];
    memcpy(&info->config, cfg, sizeof(ProcConfig));
    info->pid = -1;
    info->status = PROC_STOPPED;
    info->restart_count = 0;
    info->start_time = 0;
    info->last_crash_time = 0;
    pm->count++;
    LOG_INFO("added process: %s cmd=%s max_restart=%d delay=%d",
             cfg->name, cfg->cmd, cfg->max_restart, cfg->restart_delay);
    return 0;
}

/*
 * pm_find() - 按名称查找进程，返回索引或-1
 */
int pm_find(ProcManager *pm, const char *name)
{
    for (int i = 0; i < pm->count; i++) {
        if (strcmp(pm->procs[i].config.name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * pm_status_str() - 获取状态的可读字符串
 */
const char *pm_status_str(ProcStatus status)
{
    switch (status) {
    case PROC_RUNNING: return "running";
    case PROC_STOPPED: return "stopped";
    case PROC_CRASHED: return "crashed";
    default:           return "unknown";
    }
}

/*
 * build_argv() - 将命令字符串拆分为argv数组
 * @cmd:   命令字符串（空格分隔）
 * @argv:  输出argv数组，结尾置NULL
 * @max:   argv数组最大长度
 * 返回值：参数个数
 */
static int build_argv(char *cmd, char *argv[], int max)
{
    int argc = 0;
    char *saveptr;
    char *token = strtok_r(cmd, " ", &saveptr);
    while (token && argc < max - 1) {
        argv[argc++] = token;
        token = strtok_r(NULL, " ", &saveptr);
    }
    argv[argc] = NULL;
    return argc;
}

/*
 * start_process() - fork+exec启动子进程
 * 注：调用者应在外部持有pm->lock
 */
int start_process(ProcInfo *proc)
{
    if (proc->pid > 0) {
        LOG_WARN("process %s is already running, pid=%d",
                 proc->config.name, proc->pid);
        return 0;
    }

    /* 制作命令副本，strtok_r会修改字符串 */
    char cmd_copy[256];
    strncpy(cmd_copy, proc->config.cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    char *argv[64];
    build_argv(cmd_copy, argv, 64);

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed for %s: %s", proc->config.name, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* 子进程：执行目标程序 */
        execvp(argv[0], argv);
        /* execvp只有失败才会返回 */
        LOG_ERROR("execvp(%s) failed: %s", argv[0], strerror(errno));
        _exit(127);
    }

    /* 父进程：记录子进程信息 */
    proc->pid = pid;
    proc->status = PROC_RUNNING;
    proc->start_time = time(NULL);
    LOG_INFO("process %s started, pid=%d, cmd=%s",
             proc->config.name, pid, proc->config.cmd);
    return 0;
}

/*
 * stop_process() - 停止子进程，先SIGTERM再SIGKILL
 * 注：调用者应在外部持有pm->lock，但wait循环期间会临时释放
 */
int stop_process(ProcInfo *proc)
{
    if (proc->pid <= 0) {
        proc->status = PROC_STOPPED;
        proc->pid = -1;
        return 0;
    }

    pid_t target_pid = proc->pid;
    LOG_INFO("stopping process %s (pid=%d)...", proc->config.name, target_pid);

    /* 发送SIGTERM */
    if (kill(target_pid, SIGTERM) != 0) {
        if (errno == ESRCH) {
            /* 进程已经不存在 */
            LOG_WARN("process %s pid=%d already gone",
                     proc->config.name, target_pid);
            proc->pid = -1;
            proc->status = PROC_STOPPED;
            return 0;
        }
        LOG_ERROR("kill(SIGTERM, %d) failed: %s", target_pid, strerror(errno));
        return -1;
    }

    /* 等待STOP_TIMEOUT秒，轮询检查进程是否退出 */
    int waited = 0;
    while (waited < STOP_TIMEOUT) {
        int status;
        pid_t result = waitpid(target_pid, &status, WNOHANG);
        if (result == target_pid) {
            LOG_INFO("process %s stopped, pid=%d",
                     proc->config.name, target_pid);
            goto done;
        }
        if (result < 0 && errno != EINTR) {
            break;
        }
        sleep(1);
        waited++;
    }

    /* SIGTERM超时，发送SIGKILL强制终止 */
    LOG_WARN("SIGTERM timeout for %s, sending SIGKILL", proc->config.name);
    if (kill(target_pid, SIGKILL) != 0) {
        LOG_ERROR("kill(SIGKILL, %d) failed: %s", target_pid, strerror(errno));
    }
    /* 等待SIGKILL生效 */
    {
        int status;
        waitpid(target_pid, &status, 0);
    }

done:
    proc->pid = -1;
    proc->status = PROC_STOPPED;
    return 0;
}

/*
 * restart_process() - 先停后启，延迟restart_delay秒
 * 注：调用者应在外部持有pm->lock，sleep期间会临时释放
 */
int restart_process(ProcInfo *proc)
{
    LOG_INFO("restarting process %s...", proc->config.name);

    if (stop_process(proc) != 0) {
        return -1;
    }

    if (proc->config.restart_delay > 0) {
        sleep(proc->config.restart_delay);
    }

    /* 重置重启计数（手动重启时清零） */
    proc->restart_count = 0;
    return start_process(proc);
}

/*
 * handle_crash() - 处理进程崩溃，判断是否应自动重启
 * @proc: 崩溃的进程
 * @pm:   进程管理器（用于临时解锁，避免sleep期间阻塞其他线程）
 * 返回值：如果触发了自动重启返回1，否则返回0
 * 注：调用者应在外部持有pm->lock
 */
static int handle_crash(ProcInfo *proc, ProcManager *pm)
{
    proc->status = PROC_CRASHED;
    proc->last_crash_time = time(NULL);
    proc->pid = -1;
    proc->restart_count++;

    LOG_ERROR("process %s crashed (restart #%d)",
              proc->config.name, proc->restart_count);

    /* 检查是否超过最大重启次数（-1表示无限） */
    if (proc->config.max_restart >= 0 &&
        proc->restart_count > proc->config.max_restart) {
        LOG_ERROR("process %s exceeded max restart count (%d), giving up",
                   proc->config.name, proc->config.max_restart);
        proc->status = PROC_STOPPED;
        return 0;
    }

    /* 延迟后重启：临时释放锁避免长时间阻塞IPC线程 */
    int delay = proc->config.restart_delay;
    if (delay > 0) {
        pm_unlock(pm);
        sleep(delay);
        pm_lock(pm);
    }
    LOG_INFO("auto-restarting process %s...", proc->config.name);
    return start_process(proc);
}

/*
 * monitor_loop() - 主监控循环（运行在独立线程中）
 * 每次迭代用waitpid(WNOHANG)检查所有运行中的子进程
 */
int monitor_loop(ProcManager *pm)
{
    LOG_INFO("monitor loop started, watching %d processes", pm->count);

    while (pm->running) {
        pm_lock(pm);

        for (int i = 0; i < pm->count; i++) {
            ProcInfo *proc = &pm->procs[i];

            if (proc->pid <= 0) {
                continue;
            }

            int status;
            pid_t result = waitpid(proc->pid, &status, WNOHANG);

            if (result == 0) {
                /* 子进程仍在运行 */
                continue;
            }

            if (result < 0) {
                if (errno == ECHILD) {
                    proc->pid = -1;
                    proc->status = PROC_STOPPED;
                    LOG_WARN("process %s disappeared (no children)",
                             proc->config.name);
                } else if (errno != EINTR) {
                    LOG_ERROR("waitpid(%d) error: %s",
                              proc->pid, strerror(errno));
                }
                continue;
            }

            /* 子进程状态改变 */
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                LOG_INFO("process %s exited with code %d",
                         proc->config.name, exit_code);
                /* 不论退出码是否为0，都尝试自动重启 */
                proc->pid = -1;
                proc->restart_count++;

                if (proc->config.max_restart == -1 ||
                    proc->restart_count <= proc->config.max_restart) {
                    if (proc->config.restart_delay > 0) {
                        pm_unlock(pm);
                        sleep(proc->config.restart_delay);
                        pm_lock(pm);
                    }
                    start_process(proc);
                    LOG_INFO("process %s restarted (count=%d, exit_code=%d)",
                             proc->config.name, proc->restart_count, exit_code);
                } else {
                    proc->pid = -1;
                    proc->status = PROC_STOPPED;
                    LOG_WARN("process %s reached max restarts (%d), giving up",
                             proc->config.name, proc->config.max_restart);
                }
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                LOG_ERROR("process %s killed by signal %d (%s)",
                          proc->config.name, sig, strsignal(sig));
                handle_crash(proc, pm);
            }
        }

        pm_unlock(pm);

        /* 短暂休眠，避免CPU空转 */
        sleep(1);
    }

    LOG_INFO("monitor loop exiting");
    return 0;
}

/*
 * pm_cleanup() - 清理所有子进程并销毁锁
 */
void pm_cleanup(ProcManager *pm)
{
    LOG_INFO("cleaning up all child processes...");
    pm_lock(pm);
    for (int i = 0; i < pm->count; i++) {
        ProcInfo *proc = &pm->procs[i];
        if (proc->pid > 0) {
            stop_process(proc);
        }
    }
    pm_unlock(pm);
    pthread_mutex_destroy(&pm->lock);
}
