#ifndef __WEBRTC_DEBUG_H__
#define __WEBRTC_DEBUG_H__

#include "webrtcServer.h"

namespace rkmedia {
namespace webrtc {

/*
 * 注册一个 WebRTC server 到 shell 调试命令模块。
 * 调用时机：WebRtcServer 启动成功后调用；函数内部会按需注册 getWebRTC 命令。
 */
int web_rtc_debug_register_server(WebRtcServer *server);

/*
 * 从 shell 调试命令模块注销一个 WebRTC server。
 * 调用时机：WebRtcServer 停止并释放前调用，避免 shell 命令访问已经销毁的对象。
 */
void web_rtc_debug_unregister_server(WebRtcServer *server);

} // namespace webrtc
} // namespace rkmedia

#endif
