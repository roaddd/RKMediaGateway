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
    std::string type;
    std::string sdp;
    std::string candidate;
    std::string mid;
    std::string cmd;
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
