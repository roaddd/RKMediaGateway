#ifndef __WEBRTC_SIGNALING_H__
#define __WEBRTC_SIGNALING_H__

#include <rtc/candidate.hpp>
#include <string>

#include "webrtcTypes.h"

namespace rkmedia {
namespace webrtc {

/*
 * 简单信令消息结构。
 * 当前只覆盖浏览器测试页需要的 offer/answer/candidate/close 字段，不引入额外 JSON 库。
 */
struct SignalingMessage {
    std::string type;      /* 信令类型，如 offer、answer、candidate 或 close。 */
    std::string sdp;       /* offer/answer 携带的 SDP 内容。 */
    std::string candidate; /* trickle ICE candidate 的文本描述。 */
    std::string mid;       /* candidate 所属 SDP m-line 的 a=mid 值。 */
    std::string cmd;       /* DataChannel IPC 命令，如 ping、get_status。 */
};

std::string signaling_json_escape(const std::string &value);
bool signaling_parse_message(const std::string &json, SignalingMessage &message);
std::string signaling_make_description(const std::string &type, const std::string &sdp);
std::string signaling_make_candidate(const rtc::Candidate &candidate);

bool signaling_get_video_mid(const std::string &sdp, std::string &mid);
bool signaling_select_h264_payload_type(const std::string &sdp, uint8_t &payloadType);
bool signaling_offer_has_video(const std::string &sdp);
bool signaling_get_audio_mid(const std::string &sdp, std::string &mid);
bool signaling_select_g711_payload_type(const std::string &sdp,
                                        WebRtcAudioCodec codec,
                                        uint8_t &payloadType);
bool signaling_offer_has_audio(const std::string &sdp);

} // namespace webrtc
} // namespace rkmedia

#endif
