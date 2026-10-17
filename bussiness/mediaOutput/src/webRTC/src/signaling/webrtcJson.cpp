/**
 * @file webrtcJson.cpp
 * @brief 实现 WebRTC 内部文本协议共用的 JSON 字符串转义。
 */
#include "webrtcJson.h"

#include <sstream>

namespace rkmedia {
namespace webrtc {

std::string webrtc_json_escape(const std::string &value)
{
    std::ostringstream output;
    unsigned char unsignedCharacter = 0;
    char character = '\0';
    size_t index = 0;

    for (index = 0; index < value.size(); ++index) {
        character = value[index];
        unsignedCharacter = static_cast<unsigned char>(character);
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (unsignedCharacter < 0x20) {
                output << "\\u00";
                output << "0123456789abcdef"[(unsignedCharacter >> 4) & 0x0f];
                output << "0123456789abcdef"[unsignedCharacter & 0x0f];
            } else {
                output << character;
            }
            break;
        }
    }
    return output.str();
}

} // namespace webrtc
} // namespace rkmedia
