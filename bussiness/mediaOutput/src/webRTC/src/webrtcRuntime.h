/**
 * @file webrtcRuntime.h
 * @brief 定义网关内所有 WebRTC 输出共享的 ICE 与云端信令运行时。
 *
 * 运行时由 mediaGateway 创建和销毁。各输出只借用其中的 ICE 配置快照以及
 * 唯一的云端设备连接，不拥有这些共享资源。
 */
#ifndef __WEBRTC_RUNTIME_H__
#define __WEBRTC_RUNTIME_H__

#include "webrtcConfig.h"

#include <memory>
#include <string>

namespace rkmedia {
namespace webrtc {

class WebRtcCloudSignalingClient;

} // namespace webrtc
} // namespace rkmedia

struct MediaOutputWebRtcRuntime {
    std::shared_ptr<rkmedia::webrtc::WebRtcCloudSignalingClient> cloudClient;
    rkmedia::webrtc::WebRtcIceConfig ice;
    std::string cloudEndpoint;
};

#endif
