/**
 * @file debugCommandServer.h
 * @brief 进程内 shell 命令注册和 Unix Domain Socket 命令服务接口。
 *
 * 业务模块通过 regDebugCmd 注册命令处理函数，外部 rkmgw 客户端通过本地
 * Unix Domain Socket 发送命令并获取文本结果。该模块只负责命令分发，
 * 不关心业务状态含义。
 */

#ifndef __debug_command_SERVER_H__
#define __debug_command_SERVER_H__

#include <stddef.h>

#include "commonDef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define debug_command_DEFAULT_SOCKET_PATH "/tmp/rkmedia_gateway_shell.sock"
#define debug_command_MAX_REPLY_SIZE 32768

/**
 * @brief shell 命令处理函数。
 *
 * user_data 为注册命令时传入的业务上下文，命令实现函数内部自行转换类型。
 * input 为命令名后面的参数字符串。例如客户端输入 "getStatus brief" 时，
 * getStatus 处理函数收到的 input 为 "brief"；如果没有额外参数则为空字符串。
 * output 为服务端提供的文本输出缓冲区，容量固定为 debug_command_MAX_REPLY_SIZE。
 */
typedef int (*DebugCommandHandler)(void *user_data, const char *input, char *output);

/**
 * @brief 向 shell 命令回复缓冲区追加格式化文本。
 *
 * output 为 DebugCommandHandler 收到的输出缓冲区，容量固定为
 * debug_command_MAX_REPLY_SIZE。offset 由调用方维护，用于连续追加多段文本。
 */
void debug_command_reply_append(char *output,
                                size_t *offset,
                                const char *fmt,
                                ...);

/**
 * @brief 初始化 shell 命令服务。
 *
 * @param socket_path Unix Domain Socket 路径；为空时使用默认路径。
 * @return MEDIA_OK 成功，错误码表示失败原因。
 */
/* 返回 MEDIA_OK 表示成功，错误码表示 socket、bind、listen 或同步资源初始化失败。 */
int debug_command_server_init(const char *socket_path);

/**
 * @brief 注册一条 shell 命令。
 *
 * @param name 命令名称，例如 getStatus。
 * @param handler 命令处理函数。
 * @param user_data 命令处理函数上下文，可为空。
 * @return MEDIA_OK 成功，错误码表示失败原因。
 */
/* 返回 MEDIA_OK 表示成功，MEDIA_ERR_FULL 表示命令表已满，其他错误码表示失败原因。 */
int regDebugCmd(const char *name,
                DebugCommandHandler handler,
                void *user_data);

/**
 * @brief 启动 shell 命令服务线程。
 *
 * @return MEDIA_OK 成功，错误码表示失败原因。
 */
/* 返回 MEDIA_OK 表示成功，错误码表示服务未初始化或线程启动失败。 */
int debug_command_server_start(void);

/**
 * @brief 停止 shell 命令服务线程。
 */
void debug_command_server_stop(void);

/**
 * @brief 释放 shell 命令服务资源。
 */
void debug_command_server_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
