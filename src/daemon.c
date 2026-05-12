#include "daemon.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

/*
 * write_pid_file() - 将当前进程PID写入文件
 * 写入前检查是否已有另一个rkd实例在运行
 */
int write_pid_file(void)
{
    /* 检查已有PID文件 */
    FILE *fp = fopen(PID_FILE, "r");
    if (fp) {
        int old_pid;
        if (fscanf(fp, "%d", &old_pid) == 1) {
            /* 检查旧进程是否仍在运行 */
            if (kill(old_pid, 0) == 0) {
                LOG_ERROR("another rkd instance is running, pid=%d", old_pid);
                fclose(fp);
                return -1;
            }
        }
        fclose(fp);
    }

    /* 写入新PID */
    fp = fopen(PID_FILE, "w");
    if (!fp) {
        LOG_ERROR("fopen(%s) failed: %s", PID_FILE, strerror(errno));
        return -1;
    }
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    LOG_INFO("PID file written: %s (pid=%d)", PID_FILE, getpid());
    return 0;
}

/*
 * remove_pid_file() - 删除PID文件
 */
void remove_pid_file(void)
{
    if (unlink(PID_FILE) == 0) {
        LOG_INFO("PID file removed: %s", PID_FILE);
    }
}

/*
 * daemonize() - 标准两次fork实现守护进程化
 */
int daemonize(void)
{
    pid_t pid;

    /* 第一次fork */
    pid = fork();
    if (pid < 0) {
        LOG_ERROR("first fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        /* 父进程退出 */
        _exit(0);
    }

    /* 第一代子进程：创建新会话，脱离控制终端 */
    if (setsid() < 0) {
        LOG_ERROR("setsid failed: %s", strerror(errno));
        return -1;
    }

    /* 第二次fork，确保进程不再是会话组长，无法重新获取终端 */
    pid = fork();
    if (pid < 0) {
        LOG_ERROR("second fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        /* 第一代子进程退出 */
        _exit(0);
    }

    /* 第二代子进程（最终守护进程） */
    /* 设置文件创建掩码 */
    umask(0);

    /* 切换工作目录到 /tmp */
    if (chdir("/tmp") < 0) {
        LOG_ERROR("chdir(/tmp) failed: %s", strerror(errno));
        return -1;
    }

    /* 关闭标准输入/输出/错误，重定向到 /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) {
        LOG_ERROR("open(/dev/null) failed: %s", strerror(errno));
        return -1;
    }
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }

    /* 写入PID文件 */
    if (write_pid_file() != 0) {
        return -1;
    }

    LOG_INFO("daemon started successfully, pid=%d", getpid());
    return 0;
}
