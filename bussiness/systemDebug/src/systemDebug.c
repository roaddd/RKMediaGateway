/**
 * @file systemDebug.c
 * @brief 系统级 shell 调试命令实现。
 *
 * 本文件用于注册和处理不依赖具体业务上下文的调试命令。
 */

#include "systemDebug.h"

#include "logger.h"
#include "shellCommandServer.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

/**
 * @brief 获取日志级别名称。
 *
 * @param level 日志级别枚举值。
 * @return 日志级别对应的文本名称。
 */
static const char *system_debug_log_level_name(LogLevel level)
{
    const char *name = "UNKNOWN";

    switch (level)
    {
        case LOG_LEVEL_DEBUG:
            name = "DEBUG";
            break;
        case LOG_LEVEL_INFO:
            name = "INFO";
            break;
        case LOG_LEVEL_WARN:
            name = "WARN";
            break;
        case LOG_LEVEL_ERROR:
            name = "ERROR";
            break;
        default:
            name = "UNKNOWN";
            break;
    }

    return name;
}

/**
 * @brief 检查字符串剩余部分是否只有空白字符。
 *
 * strtol 解析数字后，允许后面带空格、制表符或换行；如果还有其它字符，
 * 说明用户输入不是纯数字参数，需要拒绝。
 *
 * @param text 待检查字符串。
 * @return 1 表示只有空白字符，0 表示包含非空白字符。
 */
static int system_debug_is_blank_tail(const char *text)
{
    const char *pos = text;
    int blank = 1;

    while (pos != NULL && *pos != '\0')
    {
        if (!isspace((unsigned char)*pos))
        {
            blank = 0;
            break;
        }
        pos++;
    }

    return blank;
}

/**
 * @brief 处理 setLogLevel 命令。
 *
 * input 为客户端命令名之后的原始参数，例如：
 * - setLogLevel 0：设置为 DEBUG；
 * - setLogLevel 1：设置为 INFO；
 * - setLogLevel 2：设置为 WARN；
 * - setLogLevel 3：设置为 ERROR。
 *
 * @param user_data 注册命令时传入的用户数据，本命令不需要使用。
 * @param input 命令参数字符串。
 * @param output shell 命令回复缓冲区。
 * @return 0 成功，-1 失败。
 */
static int system_debug_shell_set_log_level(void *user_data,
                                            const char *input,
                                            char *output)
{
    size_t offset = 0;
    char *end = NULL;
    long level_value = 0;
    LogLevel level = LOG_LEVEL_INFO;
    LogLevel current_level = LOG_LEVEL_INFO;

    (void)user_data;

    if (input == NULL || output == NULL)
    {
        LOG_ERROR("system_debug_shell_set_log_level failed: input=%p output=%p",
                  input,
                  output);
        return -1;
    }

    errno = 0;
    level_value = strtol(input, &end, 10);
    if (input == end)
    {
        LOG_ERROR("system_debug_shell_set_log_level failed: missing level input=%s", input);
        shell_command_reply_append(output,
                                   &offset,
                                   "invalid input: missing log level\nusage: setLogLevel <0|1|2|3>\n");
        return -1;
    }

    if (errno != 0)
    {
        LOG_ERROR("system_debug_shell_set_log_level failed: strtol errno=%d input=%s",
                  errno,
                  input);
        shell_command_reply_append(output,
                                   &offset,
                                   "invalid input: parse log level failed\nusage: setLogLevel <0|1|2|3>\n");
        return -1;
    }

    if (!system_debug_is_blank_tail(end))
    {
        LOG_ERROR("system_debug_shell_set_log_level failed: invalid tail input=%s", input);
        shell_command_reply_append(output,
                                   &offset,
                                   "invalid input: log level must be a number\nusage: setLogLevel <0|1|2|3>\n");
        return -1;
    }

    if (level_value < LOG_LEVEL_DEBUG || level_value > LOG_LEVEL_ERROR)
    {
        LOG_ERROR("system_debug_shell_set_log_level failed: invalid level=%ld", level_value);
        shell_command_reply_append(output,
                                   &offset,
                                   "invalid log level: %ld\nvalid range: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR\n",
                                   level_value);
        return -1;
    }

    level = (LogLevel)level_value;
    log_set_level(level);
    current_level = log_get_level();

    shell_command_reply_append(output,
                               &offset,
                               "log level set to %d(%s)\n",
                               (int)current_level,
                               system_debug_log_level_name(current_level));
    return 0;
}

/**
 * @brief 注册系统级 shell 调试命令。
 *
 * 该函数应在 shell_command_server_init 成功后调用。
 *
 * @return 0 成功，-1 失败。
 */
static int system_debug_register_shell_commands(void)
{
    if (regShellCmd("setLogLevel", system_debug_shell_set_log_level, NULL) != 0)
    {
        LOG_ERROR("system_debug_register_shell_commands failed: regShellCmd setLogLevel");
        return -1;
    }

    return 0;
}

/**
 * @brief 初始化系统调试模块。
 *
 * 该函数应在 shell_command_server_init 成功后调用。当前初始化动作是注册
 * 系统级 shell 调试命令，后续如果增加系统调试资源，也统一放在这里初始化。
 *
 * @return 0 成功，-1 失败。
 */
int system_debug_init(void)
{
    if (system_debug_register_shell_commands() != 0)
    {
        LOG_ERROR("system_debug_init failed: register shell commands");
        return -1;
    }

    return 0;
}
