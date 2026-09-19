#include "logger.h"

#include <cstdarg>

/*
 * RTP 解析单元测试不验证日志后端。提供最小空实现，使测试可以在编译服务器原生运行，
 * 而产品目标仍然链接 thirdparty/logger 中的真实实现。
 */
extern "C" void log_set_level(LogLevel level)
{
    (void)level;
}

extern "C" LogLevel log_get_level(void)
{
    return LOG_LEVEL_DEBUG;
}

extern "C" void log_set_immediate_flush(int enabled)
{
    (void)enabled;
}

extern "C" void log_write(LogLevel level,
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
