#include "../inc/webrtcSignaling.h"

#include <cctype>
#include <map>
#include <sstream>
#include <vector>

#include "logger.h"

namespace rkmedia {
namespace webrtc {

/* 将字符串安全放进 JSON 字符串字段，避免 SDP 中的换行和反斜杠破坏 JSON。 */
std::string signaling_json_escape(const std::string &value)
{
    std::ostringstream out;
    unsigned char uch;
    char ch;
    size_t i;

    for (i = 0; i < value.size(); ++i) {
        ch = value[i];
        uch = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (uch < 0x20) {
                out << "\\u00";
                out << "0123456789abcdef"[(uch >> 4) & 0x0f];
                out << "0123456789abcdef"[uch & 0x0f];
            } else {
                out << ch;
            }
            break;
        }
    }

    return out.str();
}

/* 从简单 JSON 文本中读取字符串字段，当前只用于浏览器信令测试消息。 */
static bool json_get_string(const std::string &json, const std::string &key, std::string &value)
{
    std::string pattern;
    size_t pos;
    size_t i;
    char ch;
    char esc;
    int code;
    int digit;

    pattern = "\"" + key + "\"";
    pos = json.find(pattern);
    if (pos == std::string::npos) {
        LOG_DEBUG("[WEBRTC][SIGNALING] json field not found key=%s bytes=%zu",
                  key.c_str(),
                  json.size());
        return false;
    }

    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) {
        LOG_WARN("[WEBRTC][SIGNALING] json field missing colon key=%s bytes=%zu",
                 key.c_str(),
                 json.size());
        return false;
    }

    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        LOG_WARN("[WEBRTC][SIGNALING] json field is not string key=%s bytes=%zu",
                 key.c_str(),
                 json.size());
        return false;
    }

    value.clear();
    for (i = pos + 1; i < json.size(); ++i) {
        ch = json[i];
        if (ch == '"') {
            return true;
        }
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }

        ++i;
        if (i >= json.size()) {
            LOG_WARN("[WEBRTC][SIGNALING] json escape truncated key=%s bytes=%zu",
                     key.c_str(),
                     json.size());
            return false;
        }

        esc = json[i];
        switch (esc) {
        case '"':
        case '\\':
        case '/':
            value.push_back(esc);
            break;
        case 'b':
            value.push_back('\b');
            break;
        case 'f':
            value.push_back('\f');
            break;
        case 'n':
            value.push_back('\n');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case 't':
            value.push_back('\t');
            break;
        case 'u':
            code = 0;
            if (i + 4 >= json.size()) {
                LOG_WARN("[WEBRTC][SIGNALING] json unicode escape truncated key=%s bytes=%zu",
                         key.c_str(),
                         json.size());
                return false;
            }
            for (digit = 0; digit < 4; ++digit) {
                ++i;
                ch = json[i];
                code <<= 4;
                if (ch >= '0' && ch <= '9') {
                    code += ch - '0';
                } else if (ch >= 'a' && ch <= 'f') {
                    code += ch - 'a' + 10;
                } else if (ch >= 'A' && ch <= 'F') {
                    code += ch - 'A' + 10;
                } else {
                    LOG_WARN("[WEBRTC][SIGNALING] json unicode escape invalid key=%s bytes=%zu",
                             key.c_str(),
                             json.size());
                    return false;
                }
            }
            value.push_back(code < 0x80 ? static_cast<char>(code) : '?');
            break;
        default:
            value.push_back(esc);
            break;
        }
    }

    LOG_WARN("[WEBRTC][SIGNALING] json string not closed key=%s bytes=%zu",
             key.c_str(),
             json.size());
    return false;
}

/* 解析 WebRTC 信令消息，只抽取当前协议需要的字符串字段。 */
bool signaling_parse_message(const std::string &json, SignalingMessage &message)
{
    message = SignalingMessage();
    json_get_string(json, "type", message.type);
    json_get_string(json, "sdp", message.sdp);
    json_get_string(json, "description", message.sdp);
    json_get_string(json, "candidate", message.candidate);
    json_get_string(json, "mid", message.mid);
    json_get_string(json, "sdpMid", message.mid);
    json_get_string(json, "cmd", message.cmd);
    if (message.type.empty()) {
        LOG_WARN("[WEBRTC][SIGNALING] parse message failed: type missing bytes=%zu", json.size());
        return false;
    }

    return true;
}

/* 生成 offer/answer JSON。当前设备端主要发送 answer。 */
std::string signaling_make_description(const std::string &type, const std::string &sdp)
{
    return "{\"type\":\"" + signaling_json_escape(type) +
           "\",\"sdp\":\"" + signaling_json_escape(sdp) + "\"}";
}

/* 生成 ICE candidate JSON，浏览器收到后调用 addIceCandidate。 */
std::string signaling_make_candidate(const rtc::Candidate &candidate)
{
    return "{\"type\":\"candidate\",\"candidate\":\"" + signaling_json_escape(candidate.candidate()) +
           "\",\"mid\":\"" + signaling_json_escape(candidate.mid()) + "\"}";
}

/* 判断浏览器 Offer 是否包含 video m-line。 */
bool signaling_offer_has_video(const std::string &sdp)
{
    return sdp.find("\nm=video ") != std::string::npos ||
           sdp.find("\r\nm=video ") != std::string::npos ||
           sdp.rfind("m=video ", 0) == 0;
}

/* 判断浏览器 Offer 是否包含 audio m-line。 */
bool signaling_offer_has_audio(const std::string &sdp)
{
    return sdp.find("\nm=audio ") != std::string::npos ||
           sdp.find("\r\nm=audio ") != std::string::npos ||
           sdp.rfind("m=audio ", 0) == 0;
}

/* 提取浏览器 Offer 的 video 媒体段。 */
static std::string get_video_section(const std::string &sdp)
{
    const std::string mediaToken = "m=video ";
    std::string section;
    size_t mediaPos;
    size_t nextMediaPos;

    mediaPos = sdp.find("\r\n" + mediaToken);
    if (mediaPos != std::string::npos) {
        mediaPos += 2;
    } else {
        mediaPos = sdp.find("\n" + mediaToken);
        if (mediaPos != std::string::npos) {
            mediaPos += 1;
        } else {
            mediaPos = sdp.rfind(mediaToken, 0) == 0 ? 0 : std::string::npos;
        }
    }

    if (mediaPos == std::string::npos) {
        return "";
    }

    nextMediaPos = sdp.find("\nm=", mediaPos + mediaToken.size());
    section = (nextMediaPos == std::string::npos) ? sdp.substr(mediaPos)
                                                  : sdp.substr(mediaPos, nextMediaPos - mediaPos);
    return section;
}

/* 提取浏览器 Offer 的 audio 媒体段。 */
static std::string get_audio_section(const std::string &sdp)
{
    const std::string mediaToken = "m=audio ";
    std::string section;
    size_t mediaPos;
    size_t nextMediaPos;

    mediaPos = sdp.find("\r\n" + mediaToken);
    if (mediaPos != std::string::npos) {
        mediaPos += 2;
    } else {
        mediaPos = sdp.find("\n" + mediaToken);
        if (mediaPos != std::string::npos) {
            mediaPos += 1;
        } else {
            mediaPos = sdp.rfind(mediaToken, 0) == 0 ? 0 : std::string::npos;
        }
    }

    if (mediaPos == std::string::npos) {
        return "";
    }

    nextMediaPos = sdp.find("\nm=", mediaPos + mediaToken.size());
    section = (nextMediaPos == std::string::npos) ? sdp.substr(mediaPos)
                                                  : sdp.substr(mediaPos, nextMediaPos - mediaPos);
    return section;
}

/*
 * 从浏览器 Offer 的 video 媒体段中提取 mid。
 *
 * mid 是 SDP 中每个 m-line 的媒体标识，例如：
 *   m=video ...
 *   a=mid:0
 *
 * 服务端添加 sendonly Track 时必须使用浏览器 video m-line 的同一个 mid，
 * 这样浏览器收到 Answer 后才能把服务端返回的视频能力匹配到原来的 video receiver。
 *
 * 返回 false 表示 Offer 的 video 媒体段不完整，本次 video 协商应失败。
 */
bool signaling_get_video_mid(const std::string &sdp, std::string &mid)
{
    const std::string midToken = "a=mid:";
    std::string section;
    size_t midPos;
    size_t lineEnd;

    mid.clear();
    /* 只在 video 媒体段里查找 a=mid，避免误取 application/datachannel 的 mid。 */
    section = get_video_section(sdp);
    if (section.empty()) {
        LOG_ERROR("[WEBRTC][SIGNALING] get video mid failed: video section missing sdp_bytes=%zu",
                  sdp.size());
        return false;
    }

    midPos = section.find(midToken);
    if (midPos == std::string::npos) {
        /*
         * 正常浏览器 Offer 都会带 a=mid。
         * 正式接入时如果缺失，不能随便用默认值继续协商，否则 Answer 可能无法匹配浏览器的 m-line。
         */
        LOG_ERROR("[WEBRTC][SIGNALING] get video mid failed: a=mid missing section_bytes=%zu",
                  section.size());
        return false;
    }

    /* midToken 后面一直到行尾就是 mid 的值，可能是 "0"、"1" 或浏览器自定义字符串。 */
    midPos += midToken.size();
    lineEnd = section.find_first_of("\r\n", midPos);
    mid = (lineEnd == std::string::npos) ? section.substr(midPos)
                                         : section.substr(midPos, lineEnd - midPos);
    /* 空 mid 没有协商意义，直接让上层拒绝本次 video 协商。 */
    if (mid.empty()) {
        LOG_ERROR("[WEBRTC][SIGNALING] get video mid failed: mid is empty");
        return false;
    }

    return true;
}

/*
 * 从浏览器 Offer 中选择 H264 payload type。
 * 优先选择 packetization-mode=1 的 baseline profile，避免把 H264 RTP 包按 VP8 PT 发出去。
 * 返回 false 表示浏览器 Offer 中没有服务端可发送的 H264 能力，本次协商应失败。
 */
/*
 * 从浏览器 Offer 的 audio 媒体段中提取 mid。
 * 调用时机：收到浏览器 Offer 并准备添加音频 Track 之前。
 * 缺少 mid 时直接返回失败，避免 Answer 里的音频 Track 无法和浏览器 m-line 对齐。
 */
bool signaling_get_audio_mid(const std::string &sdp, std::string &mid)
{
    const std::string midToken = "a=mid:";
    std::string section;
    size_t midPos;
    size_t lineEnd;

    mid.clear();
    section = get_audio_section(sdp);
    if (section.empty()) {
        LOG_ERROR("[WEBRTC][SIGNALING] get audio mid failed: audio section missing sdp_bytes=%zu",
                  sdp.size());
        return false;
    }

    midPos = section.find(midToken);
    if (midPos == std::string::npos) {
        LOG_ERROR("[WEBRTC][SIGNALING] get audio mid failed: a=mid missing section_bytes=%zu",
                  section.size());
        return false;
    }

    midPos += midToken.size();
    lineEnd = section.find_first_of("\r\n", midPos);
    mid = (lineEnd == std::string::npos) ? section.substr(midPos)
                                         : section.substr(midPos, lineEnd - midPos);
    if (mid.empty()) {
        LOG_ERROR("[WEBRTC][SIGNALING] get audio mid failed: mid is empty");
        return false;
    }

    return true;
}

bool signaling_select_h264_payload_type(const std::string &sdp, uint8_t &selectedPayloadType)
{
    std::string section;
    std::istringstream lines;
    std::string line;
    std::vector<int> h264PayloadTypes;
    std::map<int, std::string> fmtps;
    size_t prefixSize;
    size_t spacePos;
    int payloadType;
    size_t i;

    selectedPayloadType = 0;
    section = get_video_section(sdp);
    lines.str(section);
    prefixSize = std::string("a=rtpmap:").size();

    /*
     * 第一轮扫描 video 媒体段：
     * 1. 从 a=rtpmap:<pt> H264/90000 找出所有 H264 payload type。
     *    例如浏览器可能声明 a=rtpmap:106 H264/90000，此时 106 才是 H264 的 RTP PT。
     * 2. 从 a=fmtp:<pt> ... 保存对应 PT 的 H264 参数。
     *    fmtp 里会带 packetization-mode/profile-level-id 等约束。
     */
    while (std::getline(lines, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        if (line.rfind("a=rtpmap:", 0) == 0 && line.find("H264/90000") != std::string::npos) {
            /* rtpmap 的空格前是 PT，空格后是编码名和时钟，例如 H264/90000。 */
            spacePos = line.find(' ', prefixSize);
            if (spacePos != std::string::npos) {
                payloadType = std::stoi(line.substr(prefixSize, spacePos - prefixSize));
                h264PayloadTypes.push_back(payloadType);
            }
        } else if (line.rfind("a=fmtp:", 0) == 0) {
            /* fmtp 的空格前是 PT，空格后是该 PT 对应的编码参数。 */
            prefixSize = std::string("a=fmtp:").size();
            spacePos = line.find(' ', prefixSize);
            if (spacePos != std::string::npos) {
                payloadType = std::stoi(line.substr(prefixSize, spacePos - prefixSize));
                fmtps[payloadType] = line.substr(spacePos + 1);
            }
            prefixSize = std::string("a=rtpmap:").size();
        }
    }

    /*
     * 优先级 1：
     * 选择 packetization-mode=1 且 profile-level-id=42e01f 的 H264 PT。
     * - packetization-mode=1 支持 FU-A 分片，适合真实编码器产生的大帧。
     * - 42e01f 是 baseline profile，和当前 ffmpeg 测试文件及多数浏览器兼容性更好。
     */
    for (i = 0; i < h264PayloadTypes.size(); ++i) {
        payloadType = h264PayloadTypes[i];
        if (fmtps[payloadType].find("packetization-mode=1") != std::string::npos &&
            fmtps[payloadType].find("profile-level-id=42e01f") != std::string::npos) {
            selectedPayloadType = static_cast<uint8_t>(payloadType);
            return true;
        }
    }

    /*
     * 优先级 2：
     * 如果没有完全匹配 baseline profile，就退而选择任意 packetization-mode=1 的 H264 PT。
     * 这样至少保证 RTP 打包模式和 H264RtpPacketizer 的发送方式匹配。
     */
    for (i = 0; i < h264PayloadTypes.size(); ++i) {
        payloadType = h264PayloadTypes[i];
        if (fmtps[payloadType].find("packetization-mode=1") != std::string::npos) {
            selectedPayloadType = static_cast<uint8_t>(payloadType);
            return true;
        }
    }

    /*
     * 正式接入策略：
     * - 没有 H264 PT：浏览器不支持接收 H264，不能继续按 H264 发送。
     * - 有 H264 但没有 packetization-mode=1：当前 H264RtpPacketizer 发送方式不匹配。
     * 这两种情况都返回 false，由会话层拒绝 video 协商。
     */
    LOG_ERROR("[WEBRTC][SIGNALING] select H264 payload type failed: h264_pt_count=%zu",
              h264PayloadTypes.size());
    return false;
}

/*
 * 从浏览器 Offer 中选择 G711 音频 payload type。
 * 调用时机：收到 Offer 后、添加音频 Track 前。
 * 选择原则：
 * 1. 设备端当前编码为 G711A 时只接受 PCMA/8000；
 * 2. 设备端当前编码为 G711U 时只接受 PCMU/8000；
 * 3. 不使用固定 PT 兜底，必须以浏览器实际 Offer 为准。
 */
bool signaling_select_g711_payload_type(const std::string &sdp,
                                        WebRtcAudioCodec codec,
                                        uint8_t &payloadType)
{
    std::string section;
    std::istringstream lines;
    std::string line;
    std::string codecToken;
    size_t prefixSize;
    size_t spacePos;
    int parsedPayloadType;

    payloadType = 0;
    section = get_audio_section(sdp);
    if (section.empty()) {
        LOG_ERROR("[WEBRTC][SIGNALING] select G711 payload type failed: audio section missing");
        return false;
    }

    if (codec == WEBRTC_AUDIO_CODEC_PCMA) {
        codecToken = " PCMA/8000";
    } else if (codec == WEBRTC_AUDIO_CODEC_PCMU) {
        codecToken = " PCMU/8000";
    } else {
        LOG_ERROR("[WEBRTC][SIGNALING] select G711 payload type failed: unsupported codec=%d",
                  static_cast<int>(codec));
        return false;
    }

    prefixSize = std::string("a=rtpmap:").size();
    lines.str(section);
    while (std::getline(lines, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        if (line.rfind("a=rtpmap:", 0) != 0 ||
            line.find(codecToken) == std::string::npos) {
            continue;
        }

        spacePos = line.find(' ', prefixSize);
        if (spacePos == std::string::npos) {
            LOG_WARN("[WEBRTC][SIGNALING] ignore invalid audio rtpmap line=%s", line.c_str());
            continue;
        }
        parsedPayloadType = std::stoi(line.substr(prefixSize, spacePos - prefixSize));
        if (parsedPayloadType < 0 || parsedPayloadType > 255) {
            LOG_WARN("[WEBRTC][SIGNALING] ignore audio payload type out of range pt=%d",
                     parsedPayloadType);
            continue;
        }
        payloadType = static_cast<uint8_t>(parsedPayloadType);
        return true;
    }

    LOG_ERROR("[WEBRTC][SIGNALING] select G711 payload type failed: codec=%d section_bytes=%zu",
              static_cast<int>(codec),
              section.size());
    return false;
}

} // namespace webrtc
} // namespace rkmedia
