/**
 * @file mediaGatewayDebug.h
 * @brief MediaGateway 运行期调试命令注册接口。
 */

#ifndef __MEDIA_GATEWAY_DEBUG_H__
#define __MEDIA_GATEWAY_DEBUG_H__

#include "mediaGateway.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @description: 注册 MediaGateway shell 调试命令。
 *
 * shell 命令服务的初始化、启动和释放由 main 程序负责，本接口只注册
 * MediaGateway 相关命令。
 */
int media_gateway_debug_register_shell_commands(MediaGatewayCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
