/**
 * @file webrtcSignalingParser.h
 * @brief 定义 PeerConnection 信令字段的解析接口。
 *
 * 本文件只负责从云端信令消息中提取 SDP 和 ICE 字段，不依赖 libdatachannel，
 * 便于协议解析逻辑单独测试。
 */
#ifndef __WEBRTC_SIGNALING_PARSER_H__
#define __WEBRTC_SIGNALING_PARSER_H__

#include <string>

namespace rkmedia {
namespace webrtc {

/** @brief PeerConnection 当前使用的信令字段。 */
struct SignalingMessage {
    std::string type;      /* 信令类型，如 offer、answer、candidate 或 close。 */
    std::string sdp;       /* offer/answer 携带的 SDP 内容。 */
    std::string candidate; /* trickle ICE candidate 的完整文本描述。 */
    std::string mid;       /* candidate 所属 SDP m-line 的 a=mid 值。 */
};

/**
 * @brief 解析云端转发的 PeerConnection 信令消息。
 * @param[in] json 云端信令 v1 的完整 JSON 文本。
 * @param[out] message 解析后的 PeerConnection 信令字段，失败时被清空。
 * @retval true JSON 合法并且包含非空 type 字段。
 * @retval false JSON 格式、协议版本或 type 字段不合法。
 */
bool signaling_parse_message(const std::string &json, SignalingMessage &message);

} // namespace webrtc
} // namespace rkmedia

#endif
