/**
 * @file cloudSignalingProtocol.h
 * @brief 定义云端 WebRTC 信令 JSON 的纯解析和构造函数。
 *
 * 本模块不建立网络连接、不管理重连，也不修改媒体会话状态；调用方可以仅通过
 * 输入和返回值测试协议格式。
 */
#ifndef __CLOUD_SIGNALING_PROTOCOL_H__
#define __CLOUD_SIGNALING_PROTOCOL_H__

#include <map>
#include <string>
#include <vector>

namespace rkmedia {
namespace webrtc {

/** @brief 云端 v1 扁平 JSON 消息解析后的字符串字段集合。 */
struct CloudSignalingMessage {
    std::map<std::string, std::string> fields; /* 已校验且无重复键的协议字段。 */

    /** @brief 返回指定字段；字段不存在时返回空字符串。 */
    std::string get(const std::string &name) const;
};

/**
 * @brief 严格解析云端 v1 扁平 JSON 信令消息。
 * @param[in] text 待解析的完整文本消息。
 * @param[out] message 校验成功后的字段集合；失败时为空。
 * @retval true 消息是 version=1、包含非空 type 且不存在重复键。
 * @retval false 消息超长、格式非法、版本错误或缺少 type。
 */
bool cloud_signaling_parse_message(const std::string &text,
                                   CloudSignalingMessage &message);

/** @brief 构造设备连接建立后的首条 register 消息，凭据只进入返回文本。 */
std::string cloud_signaling_make_registration(
    const std::string &deviceId,
    const std::string &token,
    const std::vector<std::string> &streamIds);

/** @brief 构造主动关闭单个云端会话的消息，不修改任何路由状态。 */
std::string cloud_signaling_make_close(const std::string &sessionId,
                                       const std::string &reason);

/**
 * @brief 给本地格式的 SDP/candidate JSON 追加云端 session_id 路由字段。
 * @retval true 输入是非空、以右花括号结尾的对象文本，已生成 envelope。
 * @retval false session_id 或消息为空，或者消息不是对象结尾。
 */
bool cloud_signaling_wrap_session_message(const std::string &sessionId,
                                          const std::string &message,
                                          std::string &envelope);

} // namespace webrtc
} // namespace rkmedia

#endif
