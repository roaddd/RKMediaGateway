#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
#define DEFAULT_LOG_FILE_PATH "/tmp/rkmedia_gateway.log"

/**
 * TODO:串口波特率115200时，实际吞吐大约只有11 KB/s，如果每帧打印 [BENCH_FRAME]、[PATH_LATENCY]、编码耗时、队列信息等，一行几百字节，30fps 很容易超过串口吞吐。
 * 超过后通常有两种结果：
 * （1）printf/LOG 阻塞：写串口时内核 tty 缓冲满了，业务线程会卡在 write()。如果这个线程在采集、编码、输出路径上，就会直接导致 DQBUF 延迟、编码提交延迟、RTSP 发送延迟。
 * （2）日志锁竞争：多线程同时打日志时，logger 往往有全局锁。一个线程被串口 write 卡住，其他线程也可能排队等日志锁，最后变成全链路抖动。
 */

/**
 * 如果还要保留详细日志，优先写内存 ring buffer 或文件，异步低优先级落盘；不要在采集/编码/RTSP 发送线程里同步打串口
 */

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static LogLevel g_log_level = LOG_LEVEL_INFO;
static FILE *g_log_file = NULL;
static int g_log_file_open_failed = 0;

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
        case LOG_LEVEL_DEBUG: return "D";
        case LOG_LEVEL_INFO: return "I";
        case LOG_LEVEL_WARN: return "W";
        case LOG_LEVEL_ERROR: return "E";
        default: return "?";
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
 * 默认把日志写入文件，避免大量日志直接打到串口导致业务线程阻塞。
 * 可通过 RKMGW_LOG_FILE 指定路径；未指定时写入 /tmp/rkmedia_gateway.log。
 */
static FILE *log_output_stream_locked(void) {
    const char *path;
    const char *redirect_stdio;

    if (g_log_file) {
        return g_log_file;
    }
    if (g_log_file_open_failed) {
        return stderr;
    }

    path = getenv("RKMGW_LOG_FILE");
    if (!path || path[0] == '\0') {
        path = DEFAULT_LOG_FILE_PATH;
    }

    g_log_file = fopen(path, "a");
    if (!g_log_file) {
        g_log_file_open_failed = 1;
        return stderr;
    }

    /*
     * 文件使用全缓冲，减少 write 调用频率；进程正常退出或缓冲区满时自动落盘。
     * 调试需要实时看日志时，可以 tail -f 文件。
     */
    setvbuf(g_log_file, NULL, _IOFBF, 64 * 1024);

    /*
     * 进程里仍有 printf、fprintf(stderr, ...) 以及其它模块的日志库会直接写标准输出/错误。
     * 默认一并重定向到日志文件，避免这些输出继续打到串口。
     * 如需临时保留终端输出，可设置 RKMGW_LOG_REDIRECT_STDIO=0。
     */
    redirect_stdio = getenv("RKMGW_LOG_REDIRECT_STDIO");
    if (!redirect_stdio || strcmp(redirect_stdio, "0") != 0) {
        fflush(stdout);
        fflush(stderr);
        if (dup2(fileno(g_log_file), STDOUT_FILENO) >= 0 &&
            dup2(fileno(g_log_file), STDERR_FILENO) >= 0) {
            /* stdout/stderr 已经指向日志文件。 */
        }
    }
    return g_log_file;
}

void log_set_level(LogLevel level) {
    if (level < LOG_LEVEL_DEBUG) level = LOG_LEVEL_DEBUG;
    if (level > LOG_LEVEL_ERROR) level = LOG_LEVEL_ERROR;

    pthread_mutex_lock(&g_log_lock);
    g_log_level = level;
    pthread_mutex_unlock(&g_log_lock);
}

LogLevel log_get_level(void) {
    LogLevel level;

    pthread_mutex_lock(&g_log_lock);
    level = g_log_level;
    pthread_mutex_unlock(&g_log_lock);
    return level;
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
#if defined(_WIN32)
    {
        time_t sec = (time_t)tv.tv_sec;
        localtime_s(&tm_value, &sec);
    }
#else
    localtime_r(&tv.tv_sec, &tm_value);
#endif
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

void log_write(LogLevel level, const char *file, const char *func, int line, const char *fmt, ...) {
    char time_buf[32];
    FILE *out;
    int enable_color;
    va_list args;
    const char *func_name;

    if (!fmt) return;
    if (level < log_get_level()) return;
    func_name = (func && func[0] != '\0') ? func : "unknown";
    log_format_time(time_buf, sizeof(time_buf));

    pthread_mutex_lock(&g_log_lock);
    out = log_output_stream_locked();
    enable_color = (out == stdout || out == stderr) && isatty(fileno(out));
    if (enable_color) {
        fprintf(out,
                "%s[%s] [%s] [%s:%s:%d] ",
                log_level_color(level),
                time_buf,
                log_level_name(level),
                log_basename(file),
                func_name,
                line);
    } else {
        fprintf(out,
                "[%s] [%s] [%s:%s:%d] ",
                time_buf,
                log_level_name(level),
                log_basename(file),
                func_name,
                line);
    }

    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    if (enable_color) fprintf(out, "%s", LOG_COLOR_RESET);
    fputc('\n', out);
    if (out == stdout || out == stderr) {
        fflush(out);
    }
    pthread_mutex_unlock(&g_log_lock);
}
