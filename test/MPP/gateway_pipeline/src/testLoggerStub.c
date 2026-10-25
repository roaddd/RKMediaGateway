/**
 * @file testLoggerStub.c
 * @brief 正式链路原生单元测试使用的最小日志后端。
 */

#include "logger.h"

#include <stdarg.h>

void log_set_level(LogLevel level)
{
    (void)level;
}

LogLevel log_get_level(void)
{
    return LOG_LEVEL_DEBUG;
}

void log_write(LogLevel level,
               const char *file,
               const char *func,
               int line,
               const char *fmt,
               ...)
{
    (void)level;
    (void)file;
    (void)func;
    (void)line;
    (void)fmt;
}
