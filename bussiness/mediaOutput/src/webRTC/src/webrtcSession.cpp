#include "../inc/webrtcSession.h"

#include "../inc/webrtcSignaling.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/nalunit.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtppacketizer.hpp>

#include "logger.h"

namespace rkmedia {
namespace webrtc {

WebRtcSession::WebRtcSession(int id,
                             const WebRtcServerConfig &config,
                             const std::shared_ptr<communication::WebSocketConnection> &connection,
                             const WebRtcSessionClosedCallback &closedCallback)
{
    id_ = id;
    config_ = config;
    connection_ = connection;
    closedCallback_ = closedCallback;
    closed_ = false;
}

WebRtcSession::~WebRtcSession()
{
    close();
}

/* 返回当前会话 ID，仅用于服务端会话表管理和日志。 */
int WebRtcSession::id() const
{
    return id_;
}

/*
 * 绑定 WebSocket 回调。
 * 调用时机：WebRtcServer 接收到新的浏览器 WebSocket 连接后立即调用。
 */
void WebRtcSession::start()
{
    std::weak_ptr<WebRtcSession> weakSession;

    if (!connection_) {
        LOG_ERROR("[WEBRTC] session=%d start failed: websocket connection is NULL", id_);
        return;
    }

    weakSession = shared_from_this();
    connection_->onOpen([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d websocket open", session->id_);
        }
    });
    connection_->onClosed([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d websocket closed", session->id_);
            session->close();
        }
    });
    connection_->onError([weakSession](const std::string &error) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_ERROR("[WEBRTC] session=%d websocket error=%s", session->id_, error.c_str());
        }
    });
    connection_->onText([weakSession](const std::string &message) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            session->handleWsText(message);
        }
    });
}

/* 关闭当前会话持有的 WebSocket、PeerConnection、DataChannel 和 Track。 */
void WebRtcSession::close()
{
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<communication::WebSocketConnection> connection;
    WebRtcSessionClosedCallback closedCallback;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            LOG_DEBUG("[WEBRTC] session=%d close ignored: already closed", id_);
            return;
        }
        closed_ = true;
        closedCallback = closedCallback_;
        pc = pc_;
        connection = connection_;
        videoTrack_.reset();
        audioTrack_.reset();
        dc_.reset();
        pc_.reset();
        connection_.reset();
    }

    if (pc) {
        pc->close();
    }
    if (connection) {
        connection->close();
    }
    if (closedCallback) {
        closedCallback(id_);
    }
}

/* 当前视频 Track 是否已经进入可发送状态。 */
bool WebRtcSession::isVideoReady() const
{
    std::shared_ptr<rtc::Track> track;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        track = videoTrack_;
    }

    return track && track->isOpen();
}

/* 当前音频 Track 是否已经进入可发送状态。 */
bool WebRtcSession::isAudioReady() const
{
    std::shared_ptr<rtc::Track> track;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        track = audioTrack_;
    }

    return track && track->isOpen();
}

/* 判断当前位置是否是 Annex-B 起始码。 */
static size_t annexb_start_code_size(const uint8_t *data, size_t size, size_t pos)
{
    if (pos + 4 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
        data[pos + 2] == 0x00 && data[pos + 3] == 0x01) {
        return 4;
    }
    if (pos + 3 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01) {
        return 3;
    }
    return 0;
}

/* 从指定位置查找下一个 Annex-B 起始码。 */
static size_t find_annexb_start_code(const uint8_t *data, size_t size, size_t pos)
{
    size_t i;

    for (i = pos; i + 3 <= size; ++i) {
        if (annexb_start_code_size(data, size, i) > 0) {
            return i;
        }
    }
    return std::string::npos;
}

/* 向 libdatachannel 输入帧中追加一个长度前缀 NALU。 */
static void append_length_prefixed_nalu(rtc::binary &frame, const uint8_t *nalu, size_t naluSize)
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
 * 将 Annex-B H264 帧转换为长度前缀格式。
 * MPP 编码器通常输出 Annex-B，本函数把 00 00 00 01 起始码剥掉后交给 H264RtpPacketizer。
 */
bool WebRtcSession::convertAnnexBToLengthPrefixed(const uint8_t *data, size_t size, rtc::binary &frame)
{
    size_t startPos;
    size_t nextStartPos;
    size_t startCodeSize;
    size_t naluStart;
    size_t naluSize;

    frame.clear();
    if (!data || size == 0) {
        LOG_WARN("[WEBRTC] session=%d Annex-B convert failed: data=%p size=%zu",
                 id_,
                 (const void *)data,
                 size);
        return false;
    }

    startPos = find_annexb_start_code(data, size, 0);
    while (startPos != std::string::npos) {
        startCodeSize = annexb_start_code_size(data, size, startPos);
        naluStart = startPos + startCodeSize;
        nextStartPos = find_annexb_start_code(data, size, naluStart);
        naluSize = (nextStartPos == std::string::npos) ? size - naluStart : nextStartPos - naluStart;
        while (naluSize > 0 && data[naluStart + naluSize - 1] == 0x00) {
            --naluSize;
        }
        if (naluSize > 0) {
            append_length_prefixed_nalu(frame, data + naluStart, naluSize);
        }
        startPos = nextStartPos;
    }

    if (frame.empty()) {
        LOG_WARN("[WEBRTC] session=%d Annex-B convert failed: no start code found size=%zu",
                 id_,
                 size);
        return false;
    }

    return true;
}

/* 复制已经是长度前缀格式的 H264 帧。 */
bool WebRtcSession::copyLengthPrefixedFrame(const uint8_t *data, size_t size, rtc::binary &frame)
{
    frame.clear();
    if (!data || size == 0) {
        LOG_WARN("[WEBRTC] session=%d length-prefixed copy failed: data=%p size=%zu",
                 id_,
                 (const void *)data,
                 size);
        return false;
    }
    frame.resize(size);
    std::memcpy(frame.data(), data, size);
    return true;
}

/*
 * 发送一帧 H264 视频。
 * 调用时机：mediaOutput 后台发送线程从队列取到 H264 视频包后调用。
 */
bool WebRtcSession::sendVideoFrame(const WebRtcVideoFrame &frame)
{
    std::shared_ptr<rtc::Track> track;
    rtc::binary payload;
    bool ok;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        track = videoTrack_;
    }
    if (!track || !track->isOpen()) {
        LOG_DEBUG("[WEBRTC] session=%d video frame ignored: track not ready", id_);
        return false;
    }

    if (frame.format == H264_FRAME_FORMAT_ANNEX_B) {
        ok = convertAnnexBToLengthPrefixed(frame.data, frame.size, payload);
    } else {
        ok = copyLengthPrefixedFrame(frame.data, frame.size, payload);
    }
    if (!ok) {
        LOG_WARN("[WEBRTC] session=%d invalid H264 frame size=%zu", id_, frame.size);
        return false;
    }

    try {
        track->sendFrame(payload, std::chrono::duration<double, std::micro>(frame.ptsUs));
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBRTC] session=%d send video failed: %s", id_, e.what());
        return false;
    }

    return true;
}

/* WebSocket 文本消息入口，根据 type 分发到 offer/candidate/close。 */
/*
 * 发送一帧 G711 音频。
 * 调用时机：mediaOutput 后台发送线程从音频队列取到 G711A/G711U 包后调用。
 * 这里不做音频重编码，只检查编码类型是否和协商出的 Track 匹配，再交给 RTP packetizer。
 */
bool WebRtcSession::sendAudioFrame(const WebRtcAudioFrame &frame)
{
    std::shared_ptr<rtc::Track> track;
    rtc::binary payload;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        track = audioTrack_;
    }
    if (!track || !track->isOpen()) {
        LOG_DEBUG("[WEBRTC] session=%d audio frame ignored: track not ready", id_);
        return false;
    }
    if (!frame.data || frame.size == 0) {
        LOG_WARN("[WEBRTC] session=%d audio frame ignored: data=%p size=%zu",
                 id_,
                 (const void *)frame.data,
                 frame.size);
        return false;
    }
    if (frame.codec != config_.audioCodec) {
        LOG_WARN("[WEBRTC] session=%d audio frame ignored: codec=%d expected=%d",
                 id_,
                 static_cast<int>(frame.codec),
                 static_cast<int>(config_.audioCodec));
        return false;
    }

    payload.resize(frame.size);
    std::memcpy(payload.data(), frame.data, frame.size);
    try {
        track->sendFrame(payload, std::chrono::duration<double, std::micro>(frame.ptsUs));
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBRTC] session=%d send audio failed: %s", id_, e.what());
        return false;
    }

    return true;
}

void WebRtcSession::handleWsText(const std::string &message)
{
    SignalingMessage signaling;

    if (!signaling_parse_message(message, signaling)) {
        LOG_WARN("[WEBRTC] session=%d invalid signaling message bytes=%zu", id_, message.size());
        return;
    }

    if (signaling.type == "offer") {
        handleOffer(message);
    } else if (signaling.type == "candidate") {
        handleCandidate(message);
    } else if (signaling.type == "close") {
        close();
    } else {
        LOG_WARN("[WEBRTC] session=%d unsupported signaling type=%s", id_, signaling.type.c_str());
    }
}

/* 创建 PeerConnection 并注册 SDP、ICE、DataChannel 和状态回调。 */
void WebRtcSession::createPeerConnection()
{
    rtc::Configuration rtcConfig;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::weak_ptr<WebRtcSession> weakSession;

    rtcConfig.disableAutoNegotiation = true;
    pc = std::make_shared<rtc::PeerConnection>(rtcConfig);
    if (!pc) {
        LOG_ERROR("[WEBRTC] session=%d create PeerConnection failed", id_);
        return;
    }
    weakSession = shared_from_this();

    /*
     * onLocalDescription 调用时机：
     * setLocalDescription(answer) 后由 libdatachannel 生成本地 SDP 时触发。
     */
    pc->onLocalDescription([weakSession](rtc::Description description) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            session->sendDescription(description);
        }
    });

    /*
     * onLocalCandidate 调用时机：
     * ICE gathering 过程中发现本地 candidate 时触发，需要通过 WebSocket 发给浏览器。
     */
    pc->onLocalCandidate([weakSession](rtc::Candidate candidate) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            session->sendCandidate(candidate);
        }
    });

    /*
     * onDataChannel 调用时机：
     * 浏览器 Offer 方 createDataChannel("ipc") 后，设备端 setRemoteDescription 时会收到该回调。
     */
    pc->onDataChannel([weakSession](std::shared_ptr<rtc::DataChannel> dc) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            session->bindDataChannel(dc);
        }
    });

    pc->onStateChange([weakSession](rtc::PeerConnection::State state) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d pc state=%d", session->id_, static_cast<int>(state));
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pc_ = pc;
    }
}

/* 处理浏览器 Offer，创建 Answer 并通过 WebSocket 返回。 */
void WebRtcSession::handleOffer(const std::string &message)
{
    SignalingMessage signaling;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::string videoMid;
    std::string audioMid;
    uint8_t payloadType;
    uint8_t audioPayloadType;
    std::unique_ptr<rtc::Description> offer;

    if (!signaling_parse_message(message, signaling) || signaling.sdp.empty()) {
        LOG_ERROR("[WEBRTC] session=%d offer missing sdp bytes=%zu", id_, message.size());
        return;
    }

    createPeerConnection();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pc = pc_;
    }
    if (!pc) {
        LOG_ERROR("[WEBRTC] session=%d offer handling failed: PeerConnection is NULL", id_);
        return;
    }

    /*
     * 浏览器需要接收视频时，Offer 会带 m=video。
     * 必须先 addTrack 再 setLocalDescription(answer)，Answer 里才会带 video sendonly。
     */
    if (signaling_offer_has_video(signaling.sdp)) {
        if (!signaling_get_video_mid(signaling.sdp, videoMid)) {
            LOG_ERROR("[WEBRTC] session=%d offer rejected: video mid missing", id_);
            close();
            return;
        }
        if (!signaling_select_h264_payload_type(signaling.sdp, payloadType)) {
            LOG_ERROR("[WEBRTC] session=%d offer rejected: no usable H264 payload type", id_);
            close();
            return;
        }
        addH264VideoTrack(videoMid, payloadType);
    }

    if (config_.audioCodec != WEBRTC_AUDIO_CODEC_NONE && signaling_offer_has_audio(signaling.sdp)) {
        if (!signaling_get_audio_mid(signaling.sdp, audioMid)) {
            LOG_ERROR("[WEBRTC] session=%d offer rejected: audio mid missing", id_);
            close();
            return;
        }
        if (!signaling_select_g711_payload_type(signaling.sdp,
                                                config_.audioCodec,
                                                audioPayloadType)) {
            LOG_ERROR("[WEBRTC] session=%d offer rejected: no usable G711 payload type codec=%d",
                      id_,
                      static_cast<int>(config_.audioCodec));
            close();
            return;
        }
        addG711AudioTrack(audioMid, audioPayloadType, config_.audioCodec);
    }

    offer.reset(new rtc::Description(signaling.sdp, "offer"));
    try {
        pc->setRemoteDescription(*offer);
        pc->setLocalDescription(rtc::Description::Type::Answer);
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBRTC] session=%d set description failed: %s", id_, e.what());
    }
}

/*
 * 处理浏览器 trickle ICE candidate。
 *
 * candidate 字段里包含候选 IP/端口和网络类型，表示“这一路可以怎么连”。
 * 例如：
 * {
 *     "type": "candidate",
 *     "candidate": "candidate:... 192.168.1.3 60665 typ host ...",
 *     "mid": "0"
 * }
 *
 * mid/sdpMid 表示该 candidate 属于 SDP 中哪一个 m-line：
 * - m=video 对应自己的 a=mid。
 * - m=application/webrtc-datachannel 也对应自己的 a=mid。
 *
 * 即使后续通过 BUNDLE 复用同一个 UDP 端口，ICE 协商阶段仍然需要 mid 来匹配 candidate 归属。
 * 因此不能只传 IP/端口，也不能在 mid 缺失时默认填 0。
 */
void WebRtcSession::handleCandidate(const std::string &message)
{
    SignalingMessage signaling;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::unique_ptr<rtc::Candidate> candidate;

    if (!signaling_parse_message(message, signaling) || signaling.candidate.empty()) {
        LOG_WARN("[WEBRTC] session=%d candidate ignored: missing candidate bytes=%zu",
                 id_,
                 message.size());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pc = pc_;
    }
    if (!pc) {
        LOG_WARN("[WEBRTC] session=%d candidate ignored before offer", id_);
        return;
    }

    if (signaling.mid.empty()) {
        LOG_ERROR("[WEBRTC] session=%d candidate ignored: mid missing", id_);
        return;
    }
    candidate.reset(new rtc::Candidate(signaling.candidate, signaling.mid));
    try {
        pc->addRemoteCandidate(*candidate);
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBRTC] session=%d add remote candidate failed: %s", id_, e.what());
    }
}

/* 把本地 SDP 描述通过 WebSocket 发回浏览器。 */
void WebRtcSession::sendDescription(const rtc::Description &description)
{
    std::shared_ptr<communication::WebSocketConnection> connection;
    std::string message;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection = connection_;
    }
    if (!connection || !connection->isOpen()) {
        LOG_WARN("[WEBRTC] session=%d send description failed: websocket not open", id_);
        return;
    }

    message = signaling_make_description(description.typeString(), description.generateSdp());
    connection->sendText(message);
}

/* 把本地 ICE candidate 通过 WebSocket 发给浏览器。 */
void WebRtcSession::sendCandidate(const rtc::Candidate &candidate)
{
    std::shared_ptr<communication::WebSocketConnection> connection;
    std::string message;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection = connection_;
    }
    if (!connection || !connection->isOpen()) {
        LOG_WARN("[WEBRTC] session=%d send candidate failed: websocket not open", id_);
        return;
    }

    message = signaling_make_candidate(candidate);
    connection->sendText(message);
}

/*
 * 绑定浏览器创建的 DataChannel。
 * 当前 IPC 只保留 ping/status 最小闭环，后续再扩展 start_stream/stop_stream 等命令。
 */
void WebRtcSession::bindDataChannel(const std::shared_ptr<rtc::DataChannel> &dc)
{
    std::weak_ptr<WebRtcSession> weakSession;

    weakSession = shared_from_this();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dc_ = dc;
    }

    dc->onOpen([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d datachannel open", session->id_);
        }
    });
    dc->onClosed([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d datachannel closed", session->id_);
        }
    });
    dc->onMessage(nullptr, [weakSession](std::string message) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            session->handleIpcMessage(message);
        }
    });
}

/* 处理 DataChannel IPC 消息。 */
void WebRtcSession::handleIpcMessage(const std::string &message)
{
    SignalingMessage signaling;
    std::shared_ptr<rtc::DataChannel> dc;
    std::string response;

    signaling_parse_message(message, signaling);
    if (signaling.cmd == "ping") {
        response = "{\"cmd\":\"pong\",\"code\":0}";
    } else if (signaling.cmd == "get_status") {
        response = "{\"cmd\":\"status\",\"code\":0,\"data\":{\"webrtc\":\"connected\"}}";
    } else {
        response = "{\"cmd\":\"unknown\",\"code\":400,\"message\":\"unsupported command\"}";
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        dc = dc_;
    }
    if (dc && dc->isOpen()) {
        dc->send(response);
    } else {
        LOG_WARN("[WEBRTC] session=%d IPC response dropped: datachannel not open", id_);
    }
}

/* 添加 H264 sendonly Track，并配置 RTP/RTCP 处理链。 */
void WebRtcSession::addH264VideoTrack(const std::string &mid, uint8_t payloadType)
{
    rtc::Description::Video video;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    std::shared_ptr<rtc::H264RtpPacketizer> packetizer;
    std::shared_ptr<rtc::RtcpSrReporter> srReporter;
    std::shared_ptr<rtc::RtcpNackResponder> nackResponder;
    std::weak_ptr<WebRtcSession> weakSession;
    std::string cname;
    std::string msid;
    uint32_t ssrc;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pc = pc_;
    }
    if (!pc) {
        LOG_ERROR("[WEBRTC] session=%d add H264 track failed: PeerConnection is NULL", id_);
        return;
    }

    ssrc = static_cast<uint32_t>(1000 + id_);
    cname = "rkmedia-webrtc-" + std::to_string(id_);
    msid = "rkmedia-stream";

    /*
     * mid 和 payloadType 都来自浏览器 Offer。
     * 这两个值决定 Answer SDP 如何匹配浏览器侧的 video receiver。
     */
    video = rtc::Description::Video(mid, rtc::Description::Direction::SendOnly);
    video.addH264Codec(payloadType);
    video.addSSRC(ssrc, cname, msid, cname);
    track = pc->addTrack(video);

    /*
     * H264 RTP 固定 90000Hz 时钟。
     * H264RtpPacketizer 负责把一帧 H264 数据切分成 RTP 包。
     */
    rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, cname, payloadType, rtc::H264RtpPacketizer::ClockRate);
    packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::Length, rtpConfig);
    srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(srReporter);
    packetizer->addToChain(nackResponder);
    track->setMediaHandler(packetizer);

    /*
     * onOpen 调用时机：
     * - Answer 已发给浏览器。
     * - ICE 连通、DTLS 握手和 SRTP 密钥协商已经完成。
     * - 这条 video Track 已经具备发送 RTP 视频帧的条件。
     *
     * 当前模块的视频帧来自 mediaOutput 队列，所以这里不启动独立发送线程；
     * sendVideoFrame() 会通过 track->isOpen() 判断是否真正发送。
     */
    weakSession = shared_from_this();
    track->onOpen([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d video track open", session->id_);
        }
    });

    /*
     * onClosed 调用时机：
     * - 浏览器关闭 PeerConnection 或页面断开。
     * - ICE/DTLS 失败或服务端主动关闭 PeerConnection。
     * 关闭后 mediaOutput 后续送来的帧会在 sendVideoFrame() 中被 isOpen() 拦截。
     */
    track->onClosed([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d video track closed", session->id_);
        }
    });

    track->onError([weakSession](std::string error) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_ERROR("[WEBRTC] session=%d video track error=%s", session->id_, error.c_str());
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        videoTrack_ = track;
    }

    LOG_INFO("[WEBRTC] session=%d add h264 track mid=%s pt=%u", id_, mid.c_str(), payloadType);
}

/*
 * 添加 G711 sendonly Track，并配置音频 RTP/RTCP 处理链。
 * 调用时机：收到浏览器 Offer 后，确认 m=audio、mid 和 PCMA/PCMU PT 都可用时调用。
 */
void WebRtcSession::addG711AudioTrack(const std::string &mid,
                                      uint8_t payloadType,
                                      WebRtcAudioCodec codec)
{
    rtc::Description::Audio audio;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    std::shared_ptr<rtc::RtpPacketizer> packetizer;
    std::shared_ptr<rtc::RtcpSrReporter> srReporter;
    std::shared_ptr<rtc::RtcpNackResponder> nackResponder;
    std::weak_ptr<WebRtcSession> weakSession;
    std::string cname;
    std::string msid;
    uint32_t ssrc;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pc = pc_;
    }
    if (!pc) {
        LOG_ERROR("[WEBRTC] session=%d add G711 track failed: PeerConnection is NULL", id_);
        return;
    }
    if (codec != WEBRTC_AUDIO_CODEC_PCMA && codec != WEBRTC_AUDIO_CODEC_PCMU) {
        LOG_ERROR("[WEBRTC] session=%d add G711 track failed: unsupported codec=%d",
                  id_,
                  static_cast<int>(codec));
        return;
    }

    ssrc = static_cast<uint32_t>(2000 + id_);
    cname = "rkmedia-webrtc-audio-" + std::to_string(id_);
    msid = "rkmedia-stream";

    /*
     * mid 和 payloadType 都来自浏览器 Offer。
     * 设备端只决定发送 PCMA 还是 PCMU，不能把本地固定 PT 强塞给浏览器。
     */
    audio = rtc::Description::Audio(mid, rtc::Description::Direction::SendOnly);
    if (codec == WEBRTC_AUDIO_CODEC_PCMA) {
        audio.addPCMACodec(payloadType);
    } else {
        audio.addPCMUCodec(payloadType);
    }
    audio.addSSRC(ssrc, cname, msid, cname);
    track = pc->addTrack(audio);

    rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, cname, payloadType, rtc::PCMARtpPacketizer::DefaultClockRate);
    if (codec == WEBRTC_AUDIO_CODEC_PCMA) {
        packetizer = std::make_shared<rtc::PCMARtpPacketizer>(rtpConfig);
    } else {
        packetizer = std::make_shared<rtc::PCMURtpPacketizer>(rtpConfig);
    }
    srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(srReporter);
    packetizer->addToChain(nackResponder);
    track->setMediaHandler(packetizer);

    weakSession = shared_from_this();
    track->onOpen([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d audio track open", session->id_);
        }
    });
    track->onClosed([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d audio track closed", session->id_);
        }
    });
    track->onError([weakSession](std::string error) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_ERROR("[WEBRTC] session=%d audio track error=%s", session->id_, error.c_str());
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        audioTrack_ = track;
    }

    LOG_INFO("[WEBRTC] session=%d add g711 track mid=%s pt=%u codec=%d",
             id_,
             mid.c_str(),
             payloadType,
             static_cast<int>(codec));
}

} // namespace webrtc
} // namespace rkmedia
