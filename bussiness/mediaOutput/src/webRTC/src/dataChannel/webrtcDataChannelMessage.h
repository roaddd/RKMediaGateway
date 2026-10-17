/**
 * @file webrtcDataChannelMessage.h
 * @brief 定义 WebRTC DataChannel 设备控制消息的解析接口。
 *
 * DataChannel 消息属于 PeerConnection 建立后的业务控制面，与建立
 * PeerConnection 使用的 SDP/ICE 信令协议相互独立。
 */
#ifndef __WEBRTC_DATA_CHANNEL_MESSAGE_H__
#define __WEBRTC_DATA_CHANNEL_MESSAGE_H__

#include <string>

namespace rkmedia {
namespace webrtc {

/**
 * @brief 从 DataChannel JSON 消息中解析非空的 `cmd` 字段。
 *
 * @param[in] json 待解析的 DataChannel 文本消息。
 * @param[out] command 解析成功后的命令名称；失败时清空。
 * @retval true 成功取得非空的 `cmd` 字段。
 * @retval false JSON 格式错误或缺少有效的 `cmd` 字段。
 */
bool data_channel_parse_command(const std::string &json, std::string &command);

} // namespace webrtc
} // namespace rkmedia

#endif
