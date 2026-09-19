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
};

/**
 * @brief 转义需要写入 JSON 字符串字段的内容。
 *
 * 函数处理双引号、反斜杠、常用控制字符以及小于 0x20 的字符，但不负责添加
 * JSON 字段外围的双引号。
 *
 * @param[in] value 原始字符串。
 * @return 可安全放入 JSON 字符串字段的转义结果。
 */
std::string signaling_json_escape(const std::string &value);

/**
 * @brief 解析浏览器发送的 WebRTC 信令 JSON。
 *
 * 仅提取当前信令协议使用的字符串字段；调用时会先清空 @p message。
 * `sdp`/`description` 和 `mid`/`sdpMid` 分别作为兼容字段写入同一个成员。
 *
 * @param[in] json 待解析的 JSON 文本。
 * @param[out] message 解析后的信令字段。
 * @retval true 成功取得非空的 `type` 字段。
 * @retval false 缺少有效的 `type` 字段。
 */
bool signaling_parse_message(const std::string &json, SignalingMessage &message);

/**
 * @brief 解析 DataChannel IPC 消息中的命令字段。
 *
 * 该入口与 WebSocket 信令解析相互独立，不要求 IPC 消息携带信令专用的 `type`
 * 字段。调用时会先清空 @p command。
 *
 * @param[in] json 待解析的 DataChannel JSON 文本。
 * @param[out] command 解析得到的非空 `cmd` 字段。
 * @retval true 成功取得非空的 `cmd` 字段。
 * @retval false 缺少有效的 `cmd` 字段。
 */
bool signaling_parse_ipc_command(const std::string &json, std::string &command);

/**
 * @brief 生成携带 SDP 的 offer 或 answer 信令 JSON。
 *
 * @param[in] type SDP 描述类型，通常为 `offer` 或 `answer`。
 * @param[in] sdp 需要发送的 SDP 文本。
 * @return 已完成 JSON 转义的信令消息。
 */
std::string signaling_make_description(const std::string &type, const std::string &sdp);

/**
 * @brief 生成发送给浏览器的 trickle ICE candidate 信令 JSON。
 *
 * @param[in] candidate libdatachannel 产生的 ICE candidate，函数读取其中的
 * candidate 文本和所属 `mid`。
 * @return 已完成 JSON 转义的 candidate 信令消息。
 */
std::string signaling_make_candidate(const rtc::Candidate &candidate);

/**
 * @brief 从 SDP 的 video 媒体段提取 `a=mid`。
 *
 * @param[in] sdp 浏览器 Offer 的完整 SDP。
 * @param[out] mid video m-line 对应的媒体标识；失败时为空字符串。
 * @retval true 成功取得非空的 video mid。
 * @retval false video 媒体段或 `a=mid` 缺失。
 */
bool signaling_get_video_mid(const std::string &sdp, std::string &mid);

/**
 * @brief 从浏览器 Offer 中选择当前发送端支持的 H.264 RTP Payload Type。
 *
 * 优先选择 `packetization-mode=1` 且 profile-level-id 为 baseline 的 H.264
 * 能力；没有精确 profile 匹配时退化为任意 `packetization-mode=1` 的 H.264 能力。
 *
 * @param[in] sdp 浏览器 Offer 的完整 SDP。
 * @param[out] payloadType 选中的 H.264 RTP PT；失败时为 0。
 * @retval true 找到兼容的 H.264 RTP PT。
 * @retval false 不存在当前发送端支持的 H.264 能力。
 */
bool signaling_select_h264_payload_type(const std::string &sdp, uint8_t &payloadType);

/**
 * @brief 判断 SDP 是否包含 video m-line。
 *
 * @param[in] sdp 待检查的完整 SDP。
 * @retval true SDP 包含 video 媒体段。
 * @retval false SDP 不包含 video 媒体段。
 */
bool signaling_offer_has_video(const std::string &sdp);

/**
 * @brief 从 SDP 的 audio 媒体段提取 `a=mid`。
 *
 * @param[in] sdp 浏览器 Offer 的完整 SDP。
 * @param[out] mid audio m-line 对应的媒体标识；失败时为空字符串。
 * @retval true 成功取得非空的 audio mid。
 * @retval false audio 媒体段或 `a=mid` 缺失。
 */
bool signaling_get_audio_mid(const std::string &sdp, std::string &mid);

/**
 * @brief 从浏览器 Offer 中选择指定音频编码的 RTP Payload Type。
 *
 * PCMA、PCMU 和 Opus 分别匹配 `PCMA/8000`、`PCMU/8000` 和
 * `opus/48000/2`，不使用固定 PT 兜底。
 *
 * @param[in] sdp 浏览器 Offer 的完整 SDP。
 * @param[in] codec 设备当前需要协商的音频编码格式。
 * @param[out] payloadType 选中的音频 RTP PT；失败时为 0。
 * @retval true 找到与 @p codec 匹配的 RTP PT。
 * @retval false audio 媒体段缺失、编码不受支持或未找到匹配能力。
 */
bool signaling_select_audio_payload_type(const std::string &sdp,
                                         WebRtcAudioCodec codec,
                                         uint8_t &payloadType);

/**
 * @brief 判断 SDP 是否包含 audio m-line。
 *
 * @param[in] sdp 待检查的完整 SDP。
 * @retval true SDP 包含 audio 媒体段。
 * @retval false SDP 不包含 audio 媒体段。
 */
bool signaling_offer_has_audio(const std::string &sdp);

/**
 * @brief 判断浏览器是否允许通过 audio m-line 向设备发送 RTP。
 *
 * `a=recvonly` 和 `a=inactive` 表示浏览器不发送音频；`a=sendonly`、
 * `a=sendrecv` 以及未声明方向时均表示浏览器允许发送。SDP 未声明方向时按照
 * RFC 3264 的 sendrecv 缺省语义处理。
 *
 * @param[in] sdp 浏览器 Offer 的完整 SDP。
 * @retval true audio 媒体段允许浏览器向设备发送 RTP。
 * @retval false audio 媒体段缺失，或方向为 recvonly/inactive。
 */
bool signaling_audio_offer_can_send(const std::string &sdp);

} // namespace webrtc
} // namespace rkmedia

#endif
