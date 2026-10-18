/**
 * @file webrtcSignalingParser.cpp
 * @brief 实现 PeerConnection 信令字段解析。
 */
#include "webrtcSignalingParser.h"

#include "cloudSignalingProtocol.h"

namespace rkmedia {
namespace webrtc {

bool signaling_parse_message(const std::string &json, SignalingMessage &message)
{
    CloudSignalingMessage cloudMessage;

    message = SignalingMessage();
    /*
     * 云端协议解析器逐个读取 JSON 键值，不通过字符串查找猜测字段位置。
     * 因此 type 的值即使也是 "candidate"，也不会被误认为 candidate 键。
     */
    if (!cloud_signaling_parse_message(json, cloudMessage)) {
        return false;
    }

    message.type = cloudMessage.get("type");
    message.sdp = cloudMessage.get("sdp");
    if (message.sdp.empty()) {
        message.sdp = cloudMessage.get("description");
    }
    message.candidate = cloudMessage.get("candidate");
    message.mid = cloudMessage.get("mid");
    if (message.mid.empty()) {
        message.mid = cloudMessage.get("sdpMid");
    }
    return !message.type.empty();
}

} // namespace webrtc
} // namespace rkmedia
