#ifndef DAEMON_H
#define DAEMON_H

/*
 * daemon.h - 守护进程模块头文件
 *
 * 提供将当前进程转变为守护进程的功能。
 * 使用标准的两次fork + setsid方案。
 */

/* PID文件路径 */
#define PID_FILE "/tmp/rkd.pid"

/*
 * daemonize() - 将当前进程转变为守护进程
 * 返回值：成功返回0，失败返回-1
 *
 * 执行步骤：
 *   1. 第一次fork，父进程退出
 *   2. 子进程调用setsid()创建新会话
 *   3. 第二次fork，第一代子进程退出
 *   4. 关闭标准输入/输出/错误
 *   5. 切换工作目录到 /tmp
 *   6. 写入PID文件
 */
int daemonize(void);

/*
 * write_pid_file() - 将当前进程PID写入PID_FILE
 * 返回值：成功返回0，失败返回-1
 */
int write_pid_file(void);

/*
 * remove_pid_file() - 删除PID文件
 */
void remove_pid_file(void);

#endif /* DAEMON_H */
