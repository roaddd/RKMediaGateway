#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>

#define LOG_COLOR_DEBUG "\033[36m"
#define LOG_COLOR_INFO  "\033[32m"
#define LOG_COLOR_WARN  "\033[33m"
#define LOG_COLOR_ERROR "\033[31m"
#define LOG_COLOR_RESET "\033[0m"

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * 从完整路径里取文件名。
 * __FILE__ 在不同编译环境下可能是 foo.c、dir/foo.c 或 dir\foo.c，
 * 这里同时兼容 Linux 和 Windows 风格分隔符。
 */
static const char *log_basename(const char *path) {
    const char *slash;
    const char *backslash;

    if (!path || path[0] == '\0') return "unknown";
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if (slash && backslash) return (slash > backslash) ? slash + 1 : backslash + 1;
    if (slash) return slash + 1;
    if (backslash) return backslash + 1;
    return path;
}

static const char *log_level_name(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO: return "INFO";
        case LOG_LEVEL_WARN: return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static const char *log_level_color(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return LOG_COLOR_DEBUG;
        case LOG_LEVEL_INFO: return LOG_COLOR_INFO;
        case LOG_LEVEL_WARN: return LOG_COLOR_WARN;
        case LOG_LEVEL_ERROR: return LOG_COLOR_ERROR;
        default: return LOG_COLOR_RESET;
    }
}

/*
 * 格式化本地时间，精确到毫秒。
 * 输出示例：2026-04-26 20:30:12.123
 */
static void log_format_time(char *buf, size_t buf_size) {
    struct timeval tv;
    struct tm tm_value;

    if (!buf || buf_size == 0) return;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_value);
    snprintf(buf,
             buf_size,
             "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_value.tm_year + 1900,
             tm_value.tm_mon + 1,
             tm_value.tm_mday,
             tm_value.tm_hour,
             tm_value.tm_min,
             tm_value.tm_sec,
             tv.tv_usec / 1000);
}

void log_write(LogLevel level, const char *file, int line, const char *fmt, ...) {
    char time_buf[32];
    FILE *out = (level >= LOG_LEVEL_WARN) ? stderr : stdout;
    int enable_color = isatty(fileno(out));
    va_list args;

    if (!fmt) return;
    log_format_time(time_buf, sizeof(time_buf));

    pthread_mutex_lock(&g_log_lock);
    if (enable_color) {
        fprintf(out,
                "%s[%s] [%s] [%s:%d] ",
                log_level_color(level),
                time_buf,
                log_level_name(level),
                log_basename(file),
                line);
    } else {
        fprintf(out,
                "[%s] [%s] [%s:%d] ",
                time_buf,
                log_level_name(level),
                log_basename(file),
                line);
    }

    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    if (enable_color) fprintf(out, "%s", LOG_COLOR_RESET);
    fputc('\n', out);
    fflush(out);
    pthread_mutex_unlock(&g_log_lock);
}
