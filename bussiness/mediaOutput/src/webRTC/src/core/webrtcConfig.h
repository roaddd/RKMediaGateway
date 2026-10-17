/**
 * @file webrtcConfig.h
 * @brief 定义 WebRTC 媒体端点和 ICE 的内部配置快照。
 */
#ifndef __WEBRTC_CONFIG_H__
#define __WEBRTC_CONFIG_H__

#include "webrtcMediaTypes.h"

#include <cstdint>
#include <functional>
#include <string>

namespace rkmedia {
namespace webrtc {

struct WebRtcIceConfig {
    std::string stunHost; /* STUN 主机；为空时不使用。 */
    uint16_t stunPort = 0; /* STUN UDP 端口。 */
    std::string turnHost; /* TURN 主机；为空时不使用。 */
    uint16_t turnPort = 0; /* TURN UDP 端口。 */
    std::string turnUsername; /* TURN 用户名。 */
    std::string turnPassword; /* 从私有文件读取的 TURN 密码。 */
    bool relayOnly = false; /* 是否只允许 TURN relay candidate。 */
};

/** @brief 单路 WebRTC 媒体端点配置，不包含具体信令实现。 */
struct WebRtcStreamEndpointConfig {
    std::string streamId; /* 云端信令使用的码流标识。 */
    std::string signalingEndpoint; /* 云端信令服务器地址，仅用于调试快照。 */
    std::function<bool()> signalingReadyCallback; /* 查询共享云端连接是否就绪。 */
    std::string name; /* 输出通道名称。 */
    uint32_t videoFps; /* 视频帧率，用于 RTP 媒体时间递增。 */
    WebRtcAudioCodec audioCodec; /* 音频 Track 编码类型。 */
    uint32_t audioSampleRate; /* G711=8000，Opus=48000。 */
    uint32_t audioChannels; /* G711=1，Opus=1 或 2。 */
    std::function<void(const WebRtcIncomingAudioPacket &)> incomingAudioCallback;
    WebRtcIceConfig ice; /* 每个 PeerConnection 使用的 ICE 参数快照。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
