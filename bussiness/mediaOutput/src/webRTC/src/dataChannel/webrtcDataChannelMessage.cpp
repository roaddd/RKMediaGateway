/**
 * @file webrtcDataChannelMessage.cpp
 * @brief 实现 WebRTC DataChannel 设备控制消息解析。
 */
#include "webrtcDataChannelMessage.h"

#include <cctype>

#include "logger.h"

namespace rkmedia {
namespace webrtc {

/** @brief 读取扁平 JSON 对象中的字符串字段，不接受非字符串类型。 */
static bool data_channel_get_json_string(const std::string &json,
                                         const std::string &key,
                                         std::string &value)
{
    std::string pattern;
    size_t position = 0;
    size_t index = 0;
    char character = '\0';
    char escapedCharacter = '\0';

    value.clear();
    pattern = "\"" + key + "\"";
    position = json.find(pattern);
    if (position == std::string::npos) {
        return false;
    }
    position = json.find(':', position + pattern.size());
    if (position == std::string::npos) {
        return false;
    }

    ++position;
    while (position < json.size() &&
           std::isspace(static_cast<unsigned char>(json[position]))) {
        ++position;
    }
    if (position >= json.size() || json[position] != '"') {
        return false;
    }

    /* 设备控制命令只需支持 JSON 常用转义；不在这里解释其他业务字段。 */
    for (index = position + 1; index < json.size(); ++index) {
        character = json[index];
        if (character == '"') {
            return true;
        }
        if (character != '\\') {
            value.push_back(character);
            continue;
        }
        ++index;
        if (index >= json.size()) {
            value.clear();
            return false;
        }
        escapedCharacter = json[index];
        if (escapedCharacter == 'n') {
            value.push_back('\n');
        } else if (escapedCharacter == 'r') {
            value.push_back('\r');
        } else if (escapedCharacter == 't') {
            value.push_back('\t');
        } else if (escapedCharacter == 'b') {
            value.push_back('\b');
        } else if (escapedCharacter == 'f') {
            value.push_back('\f');
        } else if (escapedCharacter == '"' || escapedCharacter == '\\' ||
                   escapedCharacter == '/') {
            value.push_back(escapedCharacter);
        } else {
            value.clear();
            return false;
        }
    }

    value.clear();
    return false;
}

bool data_channel_parse_command(const std::string &json, std::string &command)
{
    bool parsed = false;

    command.clear();
    parsed = data_channel_get_json_string(json, "cmd", command);
    if (!parsed || command.empty()) {
        LOG_ERROR("[WEBRTC][DATA_CHANNEL] parse command failed: cmd missing bytes=%zu",
                  json.size());
        command.clear();
        return false;
    }
    return true;
}

} // namespace webrtc
} // namespace rkmedia
