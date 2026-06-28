/**
 * @file systemDebug.h
 * @brief 系统级调试模块接口。
 *
 * 该模块只放和具体业务模块无关的运行期调试能力，例如日志级别调整。
 */

#ifndef __SYSTEM_DEBUG_H__
#define __SYSTEM_DEBUG_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化系统级调试模块。
 *
 * 当前初始化动作：
 * - 注册 setLogLevel shell 命令。
 *
 * setLogLevel 命令用法：
 * - setLogLevel 0：设置为 DEBUG；
 * - setLogLevel 1：设置为 INFO；
 * - setLogLevel 2：设置为 WARN；
 * - setLogLevel 3：设置为 ERROR。
 *
 * @return 0 成功，-1 失败。
 */
int system_debug_init(void);

#ifdef __cplusplus
}
#endif

#endif
