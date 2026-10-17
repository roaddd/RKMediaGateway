/**
 * @file webRTCDebug.h
 * @brief 声明 WebRTC 输出调试登记、注销和统计命令接口。
 */
#ifndef __WEBRTC_DEBUG_H__
#define __WEBRTC_DEBUG_H__

#include "webrtcStreamEndpoint.h"

namespace rkmedia {
namespace webrtc {

/**
 * @brief 把单路媒体端点登记到 shell 调试命令模块。
 * @param[in] endpoint 已成功启动的单路媒体端点。
 * @return MEDIA_OK 表示登记成功，其它 MediaResult 表示失败。
 */
int web_rtc_debug_register_output(WebRtcStreamEndpoint *endpoint);

/**
 * @brief 从 shell 调试命令模块注销一个 WebRTC 输出。
 * @param[in] endpoint 登记时使用的媒体端点；停止和释放前调用。
 */
void web_rtc_debug_unregister_output(WebRtcStreamEndpoint *endpoint);

} // namespace webrtc
} // namespace rkmedia

#endif
