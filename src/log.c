#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

/* 日志文件路径 */
#define LOG_FILE_PATH "/tmp/rkd.log"

/* 日志级别对应的字符串标签 */
static const char *level_str(LogLevel level)
{
    switch (level) {
    case LOG_LEVEL_INFO:  return "INFO";
    case LOG_LEVEL_WARN:  return "WARN";
    case LOG_LEVEL_ERROR: return "ERROR";
    default:              return "UNKNOWN";
    }
}

static FILE *log_fp = NULL;

/*
 * log_init() - 打开日志文件
 * 以追加模式打开，行缓冲以便实时查看
 */
int log_init(void)
{
    log_fp = fopen(LOG_FILE_PATH, "a");
    if (!log_fp) {
        fprintf(stderr, "log_init: fopen(%s) failed: %s\n",
                LOG_FILE_PATH, strerror(errno));
        return -1;
    }
    /* 行缓冲模式，每条日志即时写入文件 */
    setvbuf(log_fp, NULL, _IOLBF, 0);
    return 0;
}

/*
 * log_write() - 格式化并写入一条日志
 * 格式：[YYYY-MM-DD HH:MM:SS][LEVEL] message
 * 同时写入文件和标准错误输出
 */
int log_write(LogLevel level, const char *fmt, ...)
{
    /* 生成时间戳 */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    /* 写入时间戳和级别前缀 */
    int total = 0;
    int ret = fprintf(log_fp ? log_fp : stderr,
                      "[%s][%s] ", time_str, level_str(level));
    if (ret < 0) return -1;
    total += ret;

    /* 写入用户消息 */
    va_list args;
    va_start(args, fmt);
    ret = vfprintf(log_fp ? log_fp : stderr, fmt, args);
    va_end(args);
    if (ret < 0) return -1;
    total += ret;

    /* 写入换行 */
    ret = fputc('\n', log_fp ? log_fp : stderr);
    if (ret == EOF) return -1;
    total += 1;

    fflush(log_fp ? log_fp : stderr);

    /* 同步输出到标准错误，方便调试 */
    if (log_fp) {
        va_start(args, fmt);
        fprintf(stderr, "[%s][%s] ", time_str, level_str(level));
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
    }

    return total;
}

/*
 * log_close() - 关闭日志文件
 */
void log_close(void)
{
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}
