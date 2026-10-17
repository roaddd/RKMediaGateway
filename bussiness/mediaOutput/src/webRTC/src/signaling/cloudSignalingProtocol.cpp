/**
 * @file cloudSignalingProtocol.cpp
 * @brief 实现云端 WebRTC 信令 v1 JSON 的解析和构造。
 */
#include "cloudSignalingProtocol.h"

#include "webrtcJson.h"

#include <cctype>

namespace rkmedia {
namespace webrtc {

static void cloud_skip_space(const std::string &text, size_t &position)
{
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position]))) {
        ++position;
    }
}

/** @brief 读取一个 JSON 字符串，并拒绝截断或非法转义。 */
static bool cloud_read_string(const std::string &text,
                              size_t &position,
                              std::string &value)
{
    char escaped = 0;
    size_t index = 0;

    value.clear();
    if (position >= text.size() || text[position++] != '"') {
        return false;
    }
    while (position < text.size()) {
        escaped = text[position++];
        if (escaped == '"') return true;
        if (static_cast<unsigned char>(escaped) < 0x20) return false;
        if (escaped != '\\') {
            value.push_back(escaped);
            continue;
        }
        if (position >= text.size()) return false;
        escaped = text[position++];
        switch (escaped) {
        case '"': case '\\': case '/': value.push_back(escaped); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u':
            if (position + 4 > text.size()) return false;
            for (index = 0; index < 4; ++index) {
                if (!std::isxdigit(
                        static_cast<unsigned char>(text[position + index]))) {
                    return false;
                }
            }
            position += 4;
            value.push_back('?');
            break;
        default:
            return false;
        }
    }
    return false;
}

std::string CloudSignalingMessage::get(const std::string &name) const
{
    std::map<std::string, std::string>::const_iterator found;

    found = fields.find(name);
    return found == fields.end() ? "" : found->second;
}

bool cloud_signaling_parse_message(const std::string &text, CloudSignalingMessage &message)
{
    size_t position = 0;
    std::string key;
    std::string value;

    message.fields.clear();
    if (text.size() > 262144) return false;
    cloud_skip_space(text, position);
    if (position >= text.size() || text[position++] != '{') return false;
    cloud_skip_space(text, position);
    if (position < text.size() && text[position] == '}') return false;

    while (position < text.size()) {
        if (!cloud_read_string(text, position, key) || message.fields.count(key)) {
            return false;
        }
        cloud_skip_space(text, position);
        if (position >= text.size() || text[position++] != ':') return false;
        cloud_skip_space(text, position);
        if (position >= text.size()) return false;

        /* v1 的 version 必须是整数，其余字段必须是字符串。 */
        if ((key == "version") !=
            (text[position] >= '0' && text[position] <= '9')) {
            return false;
        }
        if (text[position] == '"') {
            if (!cloud_read_string(text, position, value)) return false;
        } else if (text[position] >= '0' && text[position] <= '9') {
            value.clear();
            while (position < text.size() &&
                   text[position] >= '0' && text[position] <= '9') {
                value.push_back(text[position++]);
            }
        } else {
            return false;
        }
        message.fields[key] = value;
        cloud_skip_space(text, position);
        if (position >= text.size()) return false;
        if (text[position] == '}') {
            ++position;
            cloud_skip_space(text, position);
            return position == text.size() && message.get("version") == "1" &&
                   !message.get("type").empty();
        }
        if (text[position++] != ',') return false;
        cloud_skip_space(text, position);
    }
    return false;
}

std::string cloud_signaling_make_registration(
    const std::string &deviceId,
    const std::string &token,
    const std::vector<std::string> &streamIds)
{
    std::string message;
    size_t index = 0;

    message = "{\"version\":1,\"type\":\"register\",\"device_id\":\"" +
        webrtc_json_escape(deviceId) + "\",\"token\":\"" +
        webrtc_json_escape(token) + "\",\"streams\":[";
    for (index = 0; index < streamIds.size(); ++index) {
        if (index > 0) message += ",";
        message += "\"" + webrtc_json_escape(streamIds[index]) + "\"";
    }
    message += "]}";
    return message;
}

std::string cloud_signaling_make_close(const std::string &sessionId,
                                       const std::string &reason)
{
    return "{\"version\":1,\"type\":\"close\",\"session_id\":\"" +
        webrtc_json_escape(sessionId) + "\",\"reason\":\"" +
        webrtc_json_escape(reason) + "\"}";
}

bool cloud_signaling_wrap_session_message(const std::string &sessionId,
                                          const std::string &message,
                                          std::string &envelope)
{
    envelope.clear();
    if (sessionId.empty() || message.empty() || message.back() != '}') {
        return false;
    }
    envelope = message.substr(0, message.size() - 1) +
        ",\"version\":1,\"session_id\":\"" +
        webrtc_json_escape(sessionId) + "\"}";
    return true;
}

} // namespace webrtc
} // namespace rkmedia
