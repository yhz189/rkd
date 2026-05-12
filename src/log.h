#ifndef LOG_H
#define LOG_H

/*
 * log.h - 日志模块头文件
 *
 * 提供日志初始化、写入和关闭功能。
 * 日志同时输出到文件 /tmp/rkd.log 和标准错误输出。
 * 使用三个宏简化调用：LOG_INFO、LOG_WARN、LOG_ERROR。
 */

/* 日志级别 */
typedef enum {
    LOG_LEVEL_INFO  = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_ERROR = 2
} LogLevel;

/*
 * log_init() - 打开日志文件，初始化日志模块
 * 返回值：成功返回0，失败返回-1并设置errno
 * 注意：必须在其他日志函数之前调用
 */
int log_init(void);

/*
 * log_write() - 写入一条日志记录
 * @level: 日志级别
 * @fmt:   格式化字符串（printf风格）
 * @...:   可变参数
 * 返回值：成功返回写入字节数，失败返回-1
 */
int log_write(LogLevel level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * log_close() - 关闭日志文件，释放资源
 */
void log_close(void);

/* 日志宏，自动传入级别 */
#define LOG_INFO(fmt, ...)  log_write(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_write(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_write(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
