#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rtc/datachannel.hpp>
#include <rtc/global.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/track.hpp>
#include <rtc/websocket.hpp>
#include <rtc/websocketserver.hpp>

namespace {

struct WsSession {
    int id = 0;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> videoTrack;
    std::thread videoThread;
    bool videoRunning = false;
    std::string h264FilePath;
    uint32_t videoFps = 30;
    std::mutex mutex;
};

std::atomic<int> g_next_session_id(1);
std::mutex g_sessions_mutex;
std::map<int, std::shared_ptr<WsSession>> g_sessions;
std::string g_h264_file_path;
uint32_t g_video_fps = 30;

/*
 * 将字符串安全放进 JSON 字符串字段。
 * SDP 和 candidate 中包含换行、反斜杠等字符，直接拼 JSON 会破坏格式。
 */
std::string jsonEscape(const std::string &value)
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

/*
 * 从固定格式的扁平 JSON 中读取字符串字段。
 * 当前测试只处理浏览器页面发来的 type/sdp/candidate/mid/cmd，不引入额外 JSON 库。
 */
bool jsonGetString(const std::string &json, const std::string &key, std::string &value)
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
        return false;
    }

    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) {
        return false;
    }

    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
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
                    return false;
                }
            }
            if (code < 0x80) {
                value.push_back(static_cast<char>(code));
            } else {
                value.push_back('?');
            }
            break;
        default:
            value.push_back(esc);
            break;
        }
    }

    return false;
}

/* 通过 WebSocket 发送信令消息。 */
void wsSend(const std::shared_ptr<rtc::WebSocket> &ws, const std::string &message)
{
    if (ws && ws->isOpen()) {
        ws->send(message);
        std::cout << "[WS] send " << message << std::endl;
    }
}

/* 发送 SDP 描述，type 为 answer 或 offer。 */
void wsSendDescription(const std::shared_ptr<rtc::WebSocket> &ws,
                       const std::string &type,
                       const std::string &sdp)
{
    std::string message;

    message = "{\"type\":\"" + jsonEscape(type) + "\",\"sdp\":\"" + jsonEscape(sdp) + "\"}";
    wsSend(ws, message);
}

/* 将 libdatachannel 生成的本地 ICE candidate 转成浏览器可识别的信令 JSON。 */
void wsSendCandidate(const std::shared_ptr<rtc::WebSocket> &ws, const rtc::Candidate &candidate)
{
    std::string message;

    message = "{\"type\":\"candidate\",\"candidate\":\"" + jsonEscape(candidate.candidate()) +
              "\",\"mid\":\"" + jsonEscape(candidate.mid()) + "\"}";
    wsSend(ws, message);
}

/* 通过 DataChannel 发送 IPC JSON 消息。 */
void dcSendJson(const std::shared_ptr<rtc::DataChannel> &dc, const std::string &message)
{
    if (dc && dc->isOpen()) {
        dc->send(message);
        std::cout << "[DC] send " << message << std::endl;
    }
}

/*
 * 处理浏览器 DataChannel 发来的 IPC 消息。
 * 第一阶段只支持 ping 和 get_status，用于验证 DataChannel 双向收发。
 */
void handleIpcMessage(const std::shared_ptr<WsSession> &session, const std::string &message)
{
    std::shared_ptr<rtc::DataChannel> dc;
    std::string cmd;
    std::string response;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        dc = session->dc;
    }

    std::cout << "[DC] recv " << message << std::endl;
    jsonGetString(message, "cmd", cmd);

    /* ping/pong 用于验证 IPC 通路最小闭环。 */
    if (cmd == "ping") {
        response = "{\"cmd\":\"pong\",\"code\":0}";
    } else if (cmd == "get_status") {
        response = "{\"cmd\":\"status\",\"code\":0,\"data\":{\"webrtc\":\"connected\"}}";
    } else {
        response = "{\"cmd\":\"unknown\",\"code\":400,\"message\":\"unsupported command\"}";
    }

    dcSendJson(dc, response);
}

/*
 * 绑定浏览器创建的 DataChannel。
 * 浏览器页面作为 offer 方会先 createDataChannel("ipc")，设备端通过 onDataChannel 收到。
 */
void bindDataChannel(const std::shared_ptr<WsSession> &session,
                     const std::shared_ptr<rtc::DataChannel> &dc)
{
    std::weak_ptr<WsSession> weakSession;
    int id;

    weakSession = session;
    id = session->id;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->dc = dc;
    }

    std::cout << "[DC] session " << id << " datachannel label=" << dc->label() << std::endl;

    dc->onOpen([id]() {
        std::cout << "[DC] session " << id << " open" << std::endl;
    });

    dc->onClosed([id]() {
        std::cout << "[DC] session " << id << " closed" << std::endl;
    });

    dc->onError([id](std::string error) {
        std::cout << "[DC] session " << id << " error: " << error << std::endl;
    });

    dc->onMessage(nullptr, [weakSession](std::string message) {
        std::shared_ptr<WsSession> session;

        session = weakSession.lock();
        if (!session) {
            return;
        }
        handleIpcMessage(session, message);
    });
}

/*
 * 判断浏览器 Offer 是否请求接收视频。
 * 本测试只在 SDP 中出现 m=video 时才添加 H264 sendonly Track。
 */
bool offerHasVideo(const std::string &sdp)
{
    return sdp.find("\nm=video ") != std::string::npos ||
           sdp.find("\r\nm=video ") != std::string::npos ||
           sdp.rfind("m=video ", 0) == 0;
}

/*
 * 从浏览器 Offer 的 video 媒体段中提取 mid。
 * Answer 里的媒体 mid 需要和 Offer 对应，否则浏览器可能无法正确匹配收流轨道。
 */
std::string getVideoMidFromOffer(const std::string &sdp)
{
    const std::string mediaToken = "m=video ";
    const std::string midToken = "a=mid:";
    std::string mid;
    size_t mediaPos;
    size_t nextMediaPos;
    size_t midPos;
    size_t lineEnd;

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
        return "video";
    }

    nextMediaPos = sdp.find("\nm=", mediaPos + mediaToken.size());
    midPos = sdp.find(midToken, mediaPos);
    if (midPos == std::string::npos || (nextMediaPos != std::string::npos && midPos > nextMediaPos)) {
        return "video";
    }

    midPos += midToken.size();
    lineEnd = sdp.find_first_of("\r\n", midPos);
    if (lineEnd == std::string::npos) {
        mid = sdp.substr(midPos);
    } else {
        mid = sdp.substr(midPos, lineEnd - midPos);
    }

    if (mid.empty()) {
        mid = "video";
    }

    return mid;
}

/*
 * 提取浏览器 Offer 的 video 媒体段。
 * 后续解析 payload type 时只看 video 段，避免误读其他 m-line。
 */
std::string getVideoSectionFromOffer(const std::string &sdp)
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
    if (nextMediaPos == std::string::npos) {
        section = sdp.substr(mediaPos);
    } else {
        section = sdp.substr(mediaPos, nextMediaPos - mediaPos);
    }

    return section;
}

/*
 * 从浏览器 Offer 中选择 H264 payload type。
 * 优先选择 baseline 42e01f + packetization-mode=1，兼容 libdatachannel 默认 H264 profile。
 */
uint8_t getH264PayloadTypeFromOffer(const std::string &sdp)
{
    std::string section;
    std::istringstream lines;
    std::string line;
    std::vector<int> h264PayloadTypes;
    std::map<int, std::string> fmtps;
    size_t prefixSize;
    size_t spacePos;
    int payloadType;
    int selectedPayloadType;
    size_t i;

    section = getVideoSectionFromOffer(sdp);
    lines.str(section);
    prefixSize = std::string("a=rtpmap:").size();
    selectedPayloadType = 102;

    while (std::getline(lines, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }

        if (line.rfind("a=rtpmap:", 0) == 0 && line.find("H264/90000") != std::string::npos) {
            spacePos = line.find(' ', prefixSize);
            if (spacePos != std::string::npos) {
                payloadType = std::stoi(line.substr(prefixSize, spacePos - prefixSize));
                h264PayloadTypes.push_back(payloadType);
            }
        } else if (line.rfind("a=fmtp:", 0) == 0) {
            prefixSize = std::string("a=fmtp:").size();
            spacePos = line.find(' ', prefixSize);
            if (spacePos != std::string::npos) {
                payloadType = std::stoi(line.substr(prefixSize, spacePos - prefixSize));
                fmtps[payloadType] = line.substr(spacePos + 1);
            }
            prefixSize = std::string("a=rtpmap:").size();
        }
    }

    for (i = 0; i < h264PayloadTypes.size(); ++i) {
        payloadType = h264PayloadTypes[i];
        if (fmtps[payloadType].find("packetization-mode=1") != std::string::npos &&
            fmtps[payloadType].find("profile-level-id=42e01f") != std::string::npos) {
            selectedPayloadType = payloadType;
            return static_cast<uint8_t>(selectedPayloadType);
        }
    }

    for (i = 0; i < h264PayloadTypes.size(); ++i) {
        payloadType = h264PayloadTypes[i];
        if (fmtps[payloadType].find("packetization-mode=1") != std::string::npos) {
            selectedPayloadType = payloadType;
            return static_cast<uint8_t>(selectedPayloadType);
        }
    }

    if (!h264PayloadTypes.empty()) {
        selectedPayloadType = h264PayloadTypes[0];
    }

    return static_cast<uint8_t>(selectedPayloadType);
}

/* 读取完整文件到内存，供单个 Annex-B H264 测试文件解析使用。 */
bool readBinaryFile(const std::string &path, std::vector<uint8_t> &data)
{
    std::ifstream input;

    input.open(path.c_str(), std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !data.empty();
}

/* 判断当前位置是否是 H264 Annex-B 起始码。 */
size_t getAnnexBStartCodeSize(const std::vector<uint8_t> &data, size_t pos)
{
    if (pos + 4 <= data.size() &&
        data[pos] == 0x00 && data[pos + 1] == 0x00 &&
        data[pos + 2] == 0x00 && data[pos + 3] == 0x01) {
        return 4;
    }
    if (pos + 3 <= data.size() &&
        data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01) {
        return 3;
    }

    return 0;
}

/* 从指定位置开始查找下一个 Annex-B 起始码。 */
size_t findAnnexBStartCode(const std::vector<uint8_t> &data, size_t pos)
{
    size_t i;

    for (i = pos; i + 3 <= data.size(); ++i) {
        if (getAnnexBStartCodeSize(data, i) > 0) {
            return i;
        }
    }

    return std::string::npos;
}

/* 将一个 Annex-B NALU 转成 4 字节长度前缀 NALU。 */
void appendLengthPrefixedNalu(rtc::binary &frame, const uint8_t *nalu, size_t naluSize)
{
    uint32_t length;

    length = static_cast<uint32_t>(naluSize);
    frame.push_back(static_cast<std::byte>((length >> 24) & 0xff));
    frame.push_back(static_cast<std::byte>((length >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((length >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(length & 0xff));
    frame.insert(frame.end(),
                 reinterpret_cast<const std::byte *>(nalu),
                 reinterpret_cast<const std::byte *>(nalu + naluSize));
}

/*
 * 将单个 Annex-B H264 裸流解析成一组视频帧。
 * SPS/PPS/SEI 会跟随后面的第一个 VCL NALU 一起发送，保证浏览器收到关键帧参数集。
 */
bool parseAnnexBFrames(const std::vector<uint8_t> &data, std::vector<rtc::binary> &frames)
{
    rtc::binary frame;
    size_t startPos;
    size_t startCodeSize;
    size_t naluStart;
    size_t nextStartPos;
    size_t naluSize;
    uint8_t naluType;
    bool frameHasVcl;

    frames.clear();
    frameHasVcl = false;
    startPos = findAnnexBStartCode(data, 0);

    while (startPos != std::string::npos) {
        startCodeSize = getAnnexBStartCodeSize(data, startPos);
        naluStart = startPos + startCodeSize;
        nextStartPos = findAnnexBStartCode(data, naluStart);
        if (nextStartPos == std::string::npos) {
            naluSize = data.size() - naluStart;
        } else {
            naluSize = nextStartPos - naluStart;
        }

        while (naluSize > 0 && data[naluStart + naluSize - 1] == 0x00) {
            --naluSize;
        }

        if (naluSize > 0) {
            naluType = data[naluStart] & 0x1f;

            /*
             * 一个新的 VCL NALU 到来时，前一个已含 VCL 的组合帧可以发送。
             * 当前测试文件由 ffmpeg 生成，baseline 单 slice 场景下这个规则足够稳定。
             */
            if ((naluType == 1 || naluType == 5) && frameHasVcl) {
                frames.push_back(frame);
                frame.clear();
                frameHasVcl = false;
            }

            appendLengthPrefixedNalu(frame, &data[naluStart], naluSize);
            if (naluType == 1 || naluType == 5) {
                frameHasVcl = true;
            }
        }

        startPos = nextStartPos;
    }

    if (frameHasVcl && !frame.empty()) {
        frames.push_back(frame);
    }

    return !frames.empty();
}

/* 加载单个 Annex-B H264 文件，并解析成 libdatachannel packetizer 需要的长度前缀帧。 */
bool loadAnnexBFrames(const std::string &path, std::vector<rtc::binary> &frames)
{
    std::vector<uint8_t> data;

    if (!readBinaryFile(path, data)) {
        return false;
    }

    return parseAnnexBFrames(data, frames);
}

/*
 * 停止当前会话的视频发送线程。
 * WebSocket 断开、PeerConnection 关闭或重新建链时都要调用，避免后台线程继续访问旧 Track。
 */
void stopVideoSender(const std::shared_ptr<WsSession> &session)
{
    std::thread oldThread;

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->videoRunning = false;
        if (session->videoThread.joinable()) {
            if (session->videoThread.get_id() == std::this_thread::get_id()) {
                session->videoThread.detach();
            } else {
                oldThread = std::move(session->videoThread);
            }
        }
    }

    if (oldThread.joinable()) {
        oldThread.join();
    }
}

/*
 * 循环读取 H264 测试帧并送入 Track。
 * 输入是单个 Annex-B .h264 裸流文件，启动发送线程时一次性解析成帧数组。
 */
void videoSenderLoop(std::weak_ptr<WsSession> weakSession)
{
    std::shared_ptr<WsSession> session;
    std::shared_ptr<rtc::Track> track;
    std::string filePath;
    std::vector<rtc::binary> fileFrames;
    uint32_t fps;
    uint32_t index;
    uint64_t sampleTimeUs;
    uint64_t frameDurationUs;
    rtc::binary sample;
    bool running;
    bool loadOk;

    index = 0;
    sampleTimeUs = 0;

    session = weakSession.lock();
    if (!session) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        filePath = session->h264FilePath;
    }

    if (filePath.empty()) {
        std::cout << "[VIDEO] H264 file path is empty, video sender stopped" << std::endl;
        return;
    }

    loadOk = loadAnnexBFrames(filePath, fileFrames);
    if (!loadOk) {
        std::cout << "[VIDEO] invalid Annex-B H264 file: " << filePath << std::endl;
        return;
    }
    std::cout << "[VIDEO] loaded Annex-B H264 file frames=" << fileFrames.size()
              << " path=" << filePath << std::endl;

    while (true) {
        session = weakSession.lock();
        if (!session) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            running = session->videoRunning;
            track = session->videoTrack;
            fps = session->videoFps;
        }

        if (!running || !track || track->isClosed()) {
            return;
        }
        if (fps == 0) {
            fps = 30;
        }

        frameDurationUs = 1000000ULL / fps;
        if (fileFrames.empty()) {
            break;
        }
        if (index >= fileFrames.size()) {
            index = 0;
        }
        sample = fileFrames[index];

        try {
            track->sendFrame(sample, std::chrono::duration<double, std::micro>(sampleTimeUs));
            std::cout << "[VIDEO] send frame index=" << index << " bytes=" << sample.size() << std::endl;
        } catch (const std::exception &e) {
            std::cout << "[VIDEO] send failed: " << e.what() << std::endl;
            break;
        }

        ++index;
        sampleTimeUs += frameDurationUs;
        std::this_thread::sleep_for(std::chrono::microseconds(frameDurationUs));
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->videoRunning = false;
    }
}

/*
 * Track open 后启动 H264 样本发送。
 * 为了保持测试简单，这里只允许每个会话一个视频发送线程。
 */
void startVideoSender(const std::shared_ptr<WsSession> &session)
{
    std::weak_ptr<WsSession> weakSession;

    stopVideoSender(session);

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->videoRunning = true;
    }

    weakSession = session;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->videoThread = std::thread(videoSenderLoop, weakSession);
    }
}

/*
 * 为当前 PeerConnection 添加 H264 sendonly Track。
 * 这里只做文件样本推流验证，不接摄像头、不接 MPP 编码器。
 */
void addH264VideoTrack(const std::shared_ptr<WsSession> &session,
                       const std::shared_ptr<rtc::PeerConnection> &pc,
                       const std::string &mid,
                       uint8_t payloadType)
{
    rtc::Description::Video video;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    std::shared_ptr<rtc::H264RtpPacketizer> packetizer;
    std::shared_ptr<rtc::RtcpSrReporter> srReporter;
    std::shared_ptr<rtc::RtcpNackResponder> nackResponder;
    std::weak_ptr<WsSession> weakSession;
    std::string cname;
    std::string msid;
    uint32_t ssrc;
    int id;

    ssrc = 1234;
    id = session->id;
    cname = "rkmedia-video-" + std::to_string(id);
    msid = "rkmedia-stream";
    video = rtc::Description::Video(mid, rtc::Description::Direction::SendOnly);
    video.addH264Codec(payloadType);
    video.addSSRC(ssrc, cname, msid, cname);

    track = pc->addTrack(video);
    rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, cname, payloadType, rtc::H264RtpPacketizer::ClockRate);
    packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::Length, rtpConfig);
    srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    nackResponder = std::make_shared<rtc::RtcpNackResponder>();

    /*
     * RTP packetizer 负责 H264 -> RTP。
     * RTCP SR 给浏览器提供 RTP 时间戳和 NTP 时间的对应关系，NACK responder 用于响应丢包重传请求。
     */
    packetizer->addToChain(srReporter);
    packetizer->addToChain(nackResponder);
    track->setMediaHandler(packetizer);

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->videoTrack = track;
    }

    weakSession = session;
    track->onOpen([weakSession, id]() {
        std::shared_ptr<WsSession> session;

        session = weakSession.lock();
        if (!session) {
            return;
        }
        std::cout << "[VIDEO] session " << id << " track open" << std::endl;
        startVideoSender(session);
    });

    track->onClosed([weakSession, id]() {
        std::shared_ptr<WsSession> session;

        session = weakSession.lock();
        std::cout << "[VIDEO] session " << id << " track closed" << std::endl;
        if (session) {
            stopVideoSender(session);
        }
    });

    std::cout << "[VIDEO] session " << id << " add H264 track mid=" << mid
              << " file=" << session->h264FilePath
              << " fps=" << session->videoFps << std::endl;
}

/*
 * 为一个浏览器 WebSocket 会话创建 PeerConnection。
 * 这里集中注册 SDP、ICE、DataChannel 和状态回调。
 */
std::shared_ptr<rtc::PeerConnection> createPeerConnection(const std::shared_ptr<WsSession> &session)
{
    rtc::Configuration config;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::weak_ptr<WsSession> weakSession;
    int id;

    config.disableAutoNegotiation = true;
    pc = std::make_shared<rtc::PeerConnection>(config);
    weakSession = session;
    id = session->id;

    /*
     * 设备端生成 Answer 后会进入该回调。
     * 回调内只负责把 SDP 通过 WebSocket 发回浏览器。
     */
    pc->onLocalDescription([weakSession](rtc::Description description) {
        std::shared_ptr<WsSession> session;
        std::shared_ptr<rtc::WebSocket> ws;
        std::string type;
        std::string sdp;

        session = weakSession.lock();
        if (!session) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            ws = session->ws;
        }

        type = description.typeString();
        sdp = description.generateSdp();
        std::cout << "[PC] local description type=" << type << " bytes=" << sdp.size() << std::endl;
        wsSendDescription(ws, type, sdp);
    });

    /*
     * ICE candidate 使用 trickle 方式发送。
     * 浏览器收到后调用 addIceCandidate。
     */
    pc->onLocalCandidate([weakSession](rtc::Candidate candidate) {
        std::shared_ptr<WsSession> session;
        std::shared_ptr<rtc::WebSocket> ws;

        session = weakSession.lock();
        if (!session) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            ws = session->ws;
        }

        std::cout << "[PC] local candidate " << candidate.candidate()
                  << " mid=" << candidate.mid() << std::endl;
        wsSendCandidate(ws, candidate);
    });

    /* 浏览器端创建 DataChannel 后，设备端会在这里收到并绑定 IPC 处理。 */
    pc->onDataChannel([weakSession](std::shared_ptr<rtc::DataChannel> dc) {
        std::shared_ptr<WsSession> session;

        session = weakSession.lock();
        if (!session) {
            return;
        }
        bindDataChannel(session, dc);
    });

    pc->onStateChange([id](rtc::PeerConnection::State state) {
        std::cout << "[PC] session " << id << " state=" << state << std::endl;
    });

    pc->onIceStateChange([id](rtc::PeerConnection::IceState state) {
        std::cout << "[PC] session " << id << " ice=" << state << std::endl;
    });

    pc->onGatheringStateChange([id](rtc::PeerConnection::GatheringState state) {
        std::cout << "[PC] session " << id << " gathering=" << state << std::endl;
    });

    return pc;
}

/*
 * 处理浏览器发来的 offer。
 * 流程：创建 PeerConnection -> setRemoteDescription(offer) -> setLocalDescription(answer)。
 */
void handleOffer(const std::shared_ptr<WsSession> &session, const std::string &message)
{
    std::shared_ptr<rtc::PeerConnection> pc;
    std::string sdp;
    std::string videoMid;
    uint8_t videoPayloadType;
    std::unique_ptr<rtc::Description> offer;

    if (!jsonGetString(message, "sdp", sdp) && !jsonGetString(message, "description", sdp)) {
        std::cout << "[WS] offer missing sdp" << std::endl;
        return;
    }

    pc = createPeerConnection(session);
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->pc = pc;
    }

    /*
     * 浏览器勾选“接收视频”后，Offer 会带 m=video。
     * 服务端在 setRemoteDescription 前添加本地 sendonly Track，让 Answer 能携带视频媒体能力。
     */
    if (offerHasVideo(sdp)) {
        videoMid = getVideoMidFromOffer(sdp);
        videoPayloadType = getH264PayloadTypeFromOffer(sdp);
        std::cout << "[VIDEO] selected H264 payload type=" << static_cast<int>(videoPayloadType) << std::endl;
        addH264VideoTrack(session, pc, videoMid, videoPayloadType);
    }

    offer.reset(new rtc::Description(sdp, "offer"));
    pc->setRemoteDescription(*offer);
    pc->setLocalDescription(rtc::Description::Type::Answer);
}

/* 处理浏览器通过 WebSocket 发来的远端 ICE candidate。 */
void handleCandidate(const std::shared_ptr<WsSession> &session, const std::string &message)
{
    std::shared_ptr<rtc::PeerConnection> pc;
    std::string candidateText;
    std::string mid;
    rtc::Candidate candidate;

    if (!jsonGetString(message, "candidate", candidateText)) {
        return;
    }
    if (!jsonGetString(message, "mid", mid) && !jsonGetString(message, "sdpMid", mid)) {
        mid = "0";
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        pc = session->pc;
    }

    if (!pc) {
        std::cout << "[WS] candidate ignored before offer" << std::endl;
        return;
    }

    candidate = rtc::Candidate(candidateText, mid);
    pc->addRemoteCandidate(candidate);
    std::cout << "[PC] remote candidate " << candidateText << " mid=" << mid << std::endl;
}

/* WebSocket 文本消息入口，根据 type 分发到 offer/candidate/close 处理。 */
void handleWsText(const std::shared_ptr<WsSession> &session, const std::string &message)
{
    std::string type;

    std::cout << "[WS] recv " << message << std::endl;
    if (!jsonGetString(message, "type", type)) {
        std::cout << "[WS] message missing type" << std::endl;
        return;
    }

    if (type == "offer") {
        handleOffer(session, message);
    } else if (type == "candidate") {
        handleCandidate(session, message);
    } else if (type == "close") {
        std::cout << "[WS] close requested" << std::endl;
        session->ws->close();
    } else {
        std::cout << "[WS] unsupported type=" << type << std::endl;
    }
}

/*
 * 绑定一个浏览器 WebSocket 连接。
 * WebSocket 断开时同步关闭并释放对应 PeerConnection 和 DataChannel。
 */
void bindWebSocket(const std::shared_ptr<WsSession> &session)
{
    std::weak_ptr<WsSession> weakSession;
    std::shared_ptr<rtc::WebSocket> ws;
    int id;

    weakSession = session;
    ws = session->ws;
    id = session->id;

    ws->onOpen([id]() {
        std::cout << "[WS] session " << id << " open" << std::endl;
    });

    ws->onClosed([weakSession, id]() {
        std::shared_ptr<WsSession> session;
        std::shared_ptr<rtc::PeerConnection> pc;

        session = weakSession.lock();
        std::cout << "[WS] session " << id << " closed" << std::endl;
        if (!session) {
            return;
        }

        stopVideoSender(session);
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            pc = session->pc;
            session->dc.reset();
            session->videoTrack.reset();
            session->pc.reset();
        }
        if (pc) {
            pc->close();
        }

        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            g_sessions.erase(id);
        }
    });

    ws->onError([id](std::string error) {
        std::cout << "[WS] session " << id << " error: " << error << std::endl;
    });

    ws->onMessage(nullptr, [weakSession](std::string message) {
        std::shared_ptr<WsSession> session;

        session = weakSession.lock();
        if (!session) {
            return;
        }
        handleWsText(session, message);
    });
}

/* 退出进程前清理所有仍在线的测试会话。 */
void closeAllSessions()
{
    std::vector<std::shared_ptr<WsSession>> sessions;
    std::shared_ptr<WsSession> session;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::WebSocket> ws;
    std::map<int, std::shared_ptr<WsSession>>::iterator iter;
    size_t i;

    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (iter = g_sessions.begin(); iter != g_sessions.end(); ++iter) {
            sessions.push_back(iter->second);
        }
        g_sessions.clear();
    }

    for (i = 0; i < sessions.size(); ++i) {
        session = sessions[i];
        stopVideoSender(session);
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            pc = session->pc;
            ws = session->ws;
            session->dc.reset();
            session->videoTrack.reset();
            session->pc.reset();
            session->ws.reset();
        }
        if (pc) {
            pc->close();
        }
        if (ws) {
            ws->close();
        }
    }
}

/* 打印命令行用法。 */
void printUsage(const char *program)
{
    std::cout << "Usage: " << program << " [port] [bind_address] [h264_file] [fps]" << std::endl;
    std::cout << "Default: " << program << " 8000 0.0.0.0 \"\" 30" << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
    rtc::WebSocketServer::Configuration wsConfig;
    std::shared_ptr<rtc::WebSocketServer> server;
    uint16_t port;
    std::string bindAddress;
    int fps;

    port = 8000;
    bindAddress = "0.0.0.0";
    fps = 30;

    if (argc > 1) {
        if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    if (argc > 2) {
        bindAddress = argv[2];
    }
    if (argc > 3) {
        g_h264_file_path = argv[3];
    }
    if (argc > 4) {
        fps = std::stoi(argv[4]);
        if (fps > 0) {
            g_video_fps = static_cast<uint32_t>(fps);
        }
    }

    try {
        rtc::InitLogger(rtc::LogLevel::Info);
        rtc::Preload();

        /* WebSocketServer 监听浏览器页面配置的 ws://设备IP:端口/browser。 */
        wsConfig.port = port;
        wsConfig.bindAddress = bindAddress;
        server = std::make_shared<rtc::WebSocketServer>(wsConfig);

        /*
         * 每个浏览器 WebSocket 连接创建一个 WsSession。
         * session 放入全局表，保证异步回调期间对象生命周期有效。
         */
        server->onClient([](std::shared_ptr<rtc::WebSocket> ws) {
            std::shared_ptr<WsSession> session;
            int id;

            id = g_next_session_id.fetch_add(1);
            session = std::make_shared<WsSession>();
            session->id = id;
            session->ws = ws;
            session->h264FilePath = g_h264_file_path;
            session->videoFps = g_video_fps;

            {
                std::lock_guard<std::mutex> lock(g_sessions_mutex);
                g_sessions[id] = session;
            }

            std::cout << "[WS] session " << id << " connected";
            if (ws->remoteAddress()) {
                std::cout << " remote=" << *ws->remoteAddress();
            }
            if (ws->path()) {
                std::cout << " path=" << *ws->path();
            }
            std::cout << std::endl;

            bindWebSocket(session);
        });

        std::cout << "[OK] WebSocket signaling server listening on "
                  << bindAddress << ":" << server->port() << std::endl;
        std::cout << "[INFO] browser url example: ws://" << bindAddress
                  << ":" << server->port() << "/browser" << std::endl;
        std::cout << "[INFO] H264 file: "
                  << (g_h264_file_path.empty() ? "(disabled)" : g_h264_file_path)
                  << ", fps=" << g_video_fps << std::endl;
        std::cout << "[INFO] press ENTER to exit" << std::endl;
        std::cin.get();

        /* 退出时先停止监听，再清理所有仍在线的测试会话。 */
        server->stop();
        closeAllSessions();
        rtc::Cleanup().wait();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[FAIL] exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[FAIL] unknown exception" << std::endl;
    }

    return 1;
}
