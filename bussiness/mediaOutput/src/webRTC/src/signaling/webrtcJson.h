/**
 * @file webrtcJson.h
 * @brief 定义 WebRTC 内部多个文本协议共用的最小 JSON 字符串工具。
 */
#ifndef __WEBRTC_JSON_H__
#define __WEBRTC_JSON_H__

#include <string>

namespace rkmedia {
namespace webrtc {

/**
 * @brief 转义需要写入 JSON 字符串字段的原始文本。
 * @param[in] value 不包含外围双引号的原始字符串。
 * @return 已处理引号、反斜杠和控制字符的字段内容。
 */
std::string webrtc_json_escape(const std::string &value);

} // namespace webrtc
} // namespace rkmedia

#endif
