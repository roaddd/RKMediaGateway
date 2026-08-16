#include "../inc/webrtcSession.h"

#include "../inc/webrtcSignaling.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/nalunit.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtppacketizer.hpp>

#include "logger.h"

namespace rkmedia {
namespace webrtc {

/* 将 PeerConnection 状态转换为 shell 调试命令更容易阅读的文本。 */
static const char *peer_connection_state_name(rtc::PeerConnection::State state)
{
    switch (state) {
    case rtc::PeerConnection::State::New:
        return "new";
    case rtc::PeerConnection::State::Connecting:
        return "connecting";
    case rtc::PeerConnection::State::Connected:
        return "connected";
    case rtc::PeerConnection::State::Disconnected:
        return "disconnected";
    case rtc::PeerConnection::State::Failed:
        return "failed";
    case rtc::PeerConnection::State::Closed:
        return "closed";
    default:
        return "unknown";
    }
}

/* 将 ICE 状态转换为 shell 调试命令更容易阅读的文本。 */
static const char *ice_state_name(rtc::PeerConnection::IceState state)
{
    switch (state) {
    case rtc::PeerConnection::IceState::New:
        return "new";
    case rtc::PeerConnection::IceState::Checking:
        return "checking";
    case rtc::PeerConnection::IceState::Connected:
        return "connected";
    case rtc::PeerConnection::IceState::Completed:
        return "completed";
    case rtc::PeerConnection::IceState::Failed:
        return "failed";
    case rtc::PeerConnection::IceState::Disconnected:
        return "disconnected";
    case rtc::PeerConnection::IceState::Closed:
        return "closed";
    default:
        return "unknown";
    }
}

/* 将 ICE candidate 收集状态转换为 shell 调试命令更容易阅读的文本。 */
static const char *gathering_state_name(rtc::PeerConnection::GatheringState state)
{
    switch (state) {
    case rtc::PeerConnection::GatheringState::New:
        return "new";
    case rtc::PeerConnection::GatheringState::InProgress:
        return "in-progress";
    case rtc::PeerConnection::GatheringState::Complete:
        return "complete";
    default:
        return "unknown";
    }
}

/* PLI 测试最多抑制视频 15 秒，超时后自动恢复该浏览器的视频。 */
static const std::chrono::seconds WEBRTC_PLI_TEST_TIMEOUT(15);

WebRtcSession::WebRtcSession(int id,
                             const WebRtcServerConfig &config,
                             const std::shared_ptr<communication::WebSocketConnection> &connection,
                             const WebRtcSessionClosedCallback &closedCallback,
                             const WebRtcSessionKeyframeRequestCallback &keyframeRequestCallback)
{
    id_ = id;
    config_ = config;
    transport_.connection = connection;
    closedCallback_ = closedCallback;
    keyframeRequestCallback_ = keyframeRequestCallback;
    state_.closed = false;
    state_.waitingForVideoKeyframe = false;
    state_.pliTestSuppressingVideo = false;
    state_.remoteAddress = connection ? connection->remoteAddress() : "";
    state_.path = connection ? connection->path() : "";
    state_.peerState = "new";
    state_.iceState = "new";
    state_.gatheringState = "new";
    counters_.signalingRxMessages = 0;
    counters_.signalingTxMessages = 0;
    counters_.localCandidates = 0;
    counters_.remoteCandidates = 0;
    counters_.videoFrames = 0;
    counters_.videoBytes = 0;
    counters_.videoNotReady = 0;
    counters_.videoWaitingKeyframeDrops = 0;
    counters_.videoPliReceived = 0;
    counters_.pliTestStarts = 0;
    counters_.pliTestSuppressedVideoFrames = 0;
    counters_.pliTestPliRecovered = 0;
    counters_.pliTestTimeouts = 0;
    counters_.videoSendFail = 0;
    counters_.audioFrames = 0;
    counters_.audioBytes = 0;
    counters_.audioNotReady = 0;
    counters_.audioSendFail = 0;
    counters_.dataChannelRxMessages = 0;
    counters_.dataChannelTxMessages = 0;
    counters_.dataChannelRxBytes = 0;
    counters_.dataChannelTxBytes = 0;
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
 * 将本会话的关键帧需求上报给 WebRtcServer。
 * 调用时机：video Track 打开后请求首个 IDR，或浏览器通过 RTCP PLI/FIR
 * 表示解码参考链已丢失时请求恢复帧。限频和多会话合并由 WebRtcServer 统一处理。
 */
void WebRtcSession::requestVideoKeyframe(WebRtcKeyframeRequestReason reason)
{
    WebRtcSessionKeyframeRequestCallback callback;
    bool pliTestRecovered = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        /* 已关闭 session 不再向 server 上报，避免关闭后的异步 PLI 触发无效 IDR。 */
        if (state_.closed) {
            return;
        }
        /* PLI 计数归属到产生反馈的浏览器 session，便于定位异常解码端。 */
        if (reason == WEBRTC_KEYFRAME_REQUEST_PLI) {
            ++counters_.videoPliReceived;
            /* PLI 测试等待的是该浏览器反馈；收到后立即恢复该会话的视频发送。 */
            if (state_.pliTestSuppressingVideo) {
                state_.pliTestSuppressingVideo = false;
                ++counters_.pliTestPliRecovered;
                pliTestRecovered = true;
            }
        }
        /* 只在锁内复制回调，避免执行 server 逻辑时长时间占用 session 锁。 */
        callback = keyframeRequestCallback_;
    }
    if (callback) {
        callback(id_, reason);
    }
    if (pliTestRecovered) {
        LOG_WARN("[WEBRTC] session=%d PLI test received PLI, resume video delivery", id_);
    }
}

/*
 * 绑定 WebSocket 回调。
 * 调用时机：WebRtcServer 接收到新的浏览器 WebSocket 连接后立即调用。
 */
void WebRtcSession::start()
{
    std::weak_ptr<WebRtcSession> weakSession;

    if (!transport_.connection) {
        LOG_ERROR("[WEBRTC] session=%d start failed: websocket connection is NULL", id_);
        return;
    }

    weakSession = shared_from_this();
    transport_.connection->onOpen([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d websocket open", session->id_);
        }
    });
    transport_.connection->onClosed([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d websocket closed", session->id_);
            session->close();
        }
    });
    transport_.connection->onError([weakSession](const std::string &error) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_ERROR("[WEBRTC] session=%d websocket error=%s", session->id_, error.c_str());
        }
    });
    transport_.connection->onText([weakSession](const std::string &message) {
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
        if (state_.closed) {
            LOG_DEBUG("[WEBRTC] session=%d close ignored: already closed", id_);
            return;
        }
        state_.closed = true;
        closedCallback = closedCallback_;
        pc = transport_.pc;
        connection = transport_.connection;
        transport_.videoTrack.reset();
        transport_.audioTrack.reset();
        transport_.dc.reset();
        transport_.pc.reset();
        transport_.connection.reset();
        state_.peerState = "closed";
        state_.iceState = "closed";
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
        track = transport_.videoTrack;
    }

    return track && track->isOpen();
}

/*
 * 启动或取消当前浏览器 session 的 PLI 测试视频抑制。
 * 启动后仅抑制视频帧，音频和所有连接控制协议仍正常收发，直到 PLI/FIR 到达或超时。
 */
bool WebRtcSession::setPliTestVideoSuppression(bool enabled)
{
    std::shared_ptr<rtc::Track> track;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        track = transport_.videoTrack;
        if (state_.closed || (enabled && (!track || !track->isOpen()))) {
            return false;
        }
        if (enabled) {
            state_.pliTestSuppressingVideo = true;
            state_.pliTestStartTime = std::chrono::steady_clock::now();
            ++counters_.pliTestStarts;
        } else {
            changed = state_.pliTestSuppressingVideo;
            state_.pliTestSuppressingVideo = false;
        }
    }

    if (enabled) {
        LOG_WARN("[WEBRTC] session=%d PLI test started: suppress video until PLI or timeout=%llds",
                 id_,
                 static_cast<long long>(WEBRTC_PLI_TEST_TIMEOUT.count()));
    } else if (changed) {
        LOG_WARN("[WEBRTC] session=%d PLI test cancelled: resume video delivery", id_);
        /* 手工取消时立即走统一 IDR 请求路径，避免浏览器继续等待下一个周期性 GOP。 */
        requestVideoKeyframe(WEBRTC_KEYFRAME_REQUEST_TEST_TIMEOUT);
    }
    return true;
}

/* 当前音频 Track 是否已经进入可发送状态。 */
bool WebRtcSession::isAudioReady() const
{
    std::shared_ptr<rtc::Track> track;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        track = transport_.audioTrack;
    }

    return track && track->isOpen();
}

/*
 * 生成当前会话的统计快照。
 * 调用时机：shell 调试命令查询 WebRTC 状态时调用；函数内部只短暂持锁复制字段，不阻塞媒体发送。
 */
void WebRtcSession::getStats(WebRtcSessionStats &stats) const
{
    std::shared_ptr<communication::WebSocketConnection> connection;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;

    std::lock_guard<std::mutex> lock(mutex_);

    connection = transport_.connection;
    dc = transport_.dc;
    videoTrack = transport_.videoTrack;
    audioTrack = transport_.audioTrack;
    stats.id = id_;
    stats.remoteAddress = state_.remoteAddress;
    stats.path = state_.path;
    stats.peerState = state_.peerState;
    stats.iceState = state_.iceState;
    stats.gatheringState = state_.gatheringState;
    stats.closed = state_.closed;
    stats.websocketOpen = connection && connection->isOpen();
    stats.dataChannelOpen = dc && dc->isOpen();
    stats.videoTrackReady = videoTrack && videoTrack->isOpen();
    stats.waitingForVideoKeyframe = state_.waitingForVideoKeyframe;
    stats.pliTestSuppressingVideo = state_.pliTestSuppressingVideo;
    stats.audioTrackReady = audioTrack && audioTrack->isOpen();
    stats.signalingRxMessages = counters_.signalingRxMessages;
    stats.signalingTxMessages = counters_.signalingTxMessages;
    stats.localCandidates = counters_.localCandidates;
    stats.remoteCandidates = counters_.remoteCandidates;
    stats.videoFrames = counters_.videoFrames;
    stats.videoBytes = counters_.videoBytes;
    stats.videoNotReady = counters_.videoNotReady;
    stats.videoWaitingKeyframeDrops = counters_.videoWaitingKeyframeDrops;
    stats.videoPliReceived = counters_.videoPliReceived;
    stats.pliTestStarts = counters_.pliTestStarts;
    stats.pliTestSuppressedVideoFrames = counters_.pliTestSuppressedVideoFrames;
    stats.pliTestPliRecovered = counters_.pliTestPliRecovered;
    stats.pliTestTimeouts = counters_.pliTestTimeouts;
    stats.videoSendFail = counters_.videoSendFail;
    stats.audioFrames = counters_.audioFrames;
    stats.audioBytes = counters_.audioBytes;
    stats.audioNotReady = counters_.audioNotReady;
    stats.audioSendFail = counters_.audioSendFail;
    stats.dataChannelRxMessages = counters_.dataChannelRxMessages;
    stats.dataChannelTxMessages = counters_.dataChannelTxMessages;
    stats.dataChannelRxBytes = counters_.dataChannelRxBytes;
    stats.dataChannelTxBytes = counters_.dataChannelTxBytes;
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
    std::chrono::steady_clock::time_point now;
    bool ok;
    bool waitingForKeyframe;
    bool suppressTestVideo;
    bool requestTimeoutRecovery;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        track = transport_.videoTrack;
        waitingForKeyframe = state_.waitingForVideoKeyframe;
        suppressTestVideo = false;
        requestTimeoutRecovery = false;
        if (state_.pliTestSuppressingVideo) {
            now = std::chrono::steady_clock::now();
            if (now - state_.pliTestStartTime >= WEBRTC_PLI_TEST_TIMEOUT) {
                /* 浏览器未发送 PLI 时必须恢复，避免调试命令让该 session 永久黑屏。 */
                state_.pliTestSuppressingVideo = false;
                ++counters_.pliTestTimeouts;
                requestTimeoutRecovery = !frame.keyFrame;
            } else {
                /* 不发送任何视频 RTP，音频和 RTCP 仍可正常传输以接收浏览器 PLI。 */
                ++counters_.pliTestSuppressedVideoFrames;
                suppressTestVideo = true;
            }
        }
    }
    if (!track || !track->isOpen()) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.videoNotReady;
        LOG_DEBUG("[WEBRTC] session=%d video frame ignored: track not ready", id_);
        return false;
    }

    if (suppressTestVideo) {
        LOG_DEBUG("[WEBRTC] session=%d PLI test suppress video frame", id_);
        return false;
    }
    if (requestTimeoutRecovery) {
        LOG_WARN("[WEBRTC] session=%d PLI test timeout, request recovery IDR", id_);
        requestVideoKeyframe(WEBRTC_KEYFRAME_REQUEST_TEST_TIMEOUT);
    }

    /*
     * 新浏览器在中途加入 H264 码流时，不能从 P/B 帧开始解码。
     * video Track 打开后先等待 IDR；IDR 到来前的非关键帧不发送，
     * 避免浏览器收到无参考帧后持续黑屏或出现花屏。
     */
    if (waitingForKeyframe && !frame.keyFrame) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.videoWaitingKeyframeDrops;
        return false;
    }

    if (frame.format == H264_FRAME_FORMAT_ANNEX_B) {
        ok = convertAnnexBToLengthPrefixed(frame.data, frame.size, payload);
    } else {
        ok = copyLengthPrefixedFrame(frame.data, frame.size, payload);
    }
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.videoSendFail;
        LOG_WARN("[WEBRTC] session=%d invalid H264 frame size=%zu", id_, frame.size);
        return false;
    }

    try {
        track->sendFrame(payload, std::chrono::duration<double, std::micro>(frame.ptsUs));
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.videoSendFail;
        LOG_ERROR("[WEBRTC] session=%d send video failed: %s", id_, e.what());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_.waitingForVideoKeyframe && frame.keyFrame) {
            state_.waitingForVideoKeyframe = false;
            LOG_INFO("[WEBRTC] session=%d first video keyframe sent", id_);
        }
        ++counters_.videoFrames;
        counters_.videoBytes += frame.size;
    }

    return true;
}

/* WebSocket 文本消息入口，根据 type 分发到 offer/candidate/close。 */
/*
 * 发送一帧 G711 或 Opus 音频。
 * 调用时机：mediaOutput 后台发送线程从音频队列取到编码音频包后调用。
 * 这里不做音频重编码，只检查编码类型是否和协商出的 Track 匹配，再交给 RTP packetizer。
 */
bool WebRtcSession::sendAudioFrame(const WebRtcAudioFrame &frame)
{
    std::shared_ptr<rtc::Track> track;
    rtc::binary payload;
    std::chrono::steady_clock::time_point sendStart;
    std::chrono::steady_clock::time_point sendDone;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        track = transport_.audioTrack;
    }
    if (!track || !track->isOpen()) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.audioNotReady;
        LOG_DEBUG("[WEBRTC] session=%d audio frame ignored: track not ready", id_);
        return false;
    }
    if (!frame.data || frame.size == 0) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.audioSendFail;
        LOG_WARN("[WEBRTC] session=%d audio frame ignored: data=%p size=%zu",
                 id_,
                 (const void *)frame.data,
                 frame.size);
        return false;
    }
    if (frame.codec != config_.audioCodec) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.audioSendFail;
        LOG_WARN("[WEBRTC] session=%d audio frame ignored: codec=%d expected=%d",
                 id_,
                 static_cast<int>(frame.codec),
                 static_cast<int>(config_.audioCodec));
        return false;
    }

    payload.resize(frame.size);
    std::memcpy(payload.data(), frame.data, frame.size);
    try {
        sendStart = std::chrono::steady_clock::now();
        track->sendFrame(payload, std::chrono::duration<double, std::micro>(frame.ptsUs));
        sendDone = std::chrono::steady_clock::now();
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.audioSendFail;
        LOG_ERROR("[WEBRTC] session=%d send audio failed: %s", id_, e.what());
        return false;
    }

    if (frame.traceSample) {
        const uint64_t sendFrameUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(sendDone - sendStart).count());
        LOG_INFO("[WEBRTC_AUDIO_SEND] session=%d frame_id=%llu send_frame_us=%llu",
                 id_,
                 static_cast<unsigned long long>(frame.frameId),
                 static_cast<unsigned long long>(sendFrameUs));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.audioFrames;
        counters_.audioBytes += frame.size;
    }

    return true;
}

void WebRtcSession::handleWsText(const std::string &message)
{
    SignalingMessage signaling;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.signalingRxMessages;
    }

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
            {
                std::lock_guard<std::mutex> lock(session->mutex_);

                session->state_.peerState = peer_connection_state_name(state);
            }
            LOG_INFO("[WEBRTC] session=%d pc state=%d", session->id_, static_cast<int>(state));
        }
    });

    pc->onIceStateChange([weakSession](rtc::PeerConnection::IceState state) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            {
                std::lock_guard<std::mutex> lock(session->mutex_);

                session->state_.iceState = ice_state_name(state);
            }
            LOG_INFO("[WEBRTC] session=%d ice state=%s", session->id_, ice_state_name(state));
        }
    });

    pc->onGatheringStateChange([weakSession](rtc::PeerConnection::GatheringState state) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            {
                std::lock_guard<std::mutex> lock(session->mutex_);

                session->state_.gatheringState = gathering_state_name(state);
            }
            LOG_INFO("[WEBRTC] session=%d gathering state=%s", session->id_, gathering_state_name(state));
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport_.pc = pc;
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
        pc = transport_.pc;
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
        if (!signaling_select_audio_payload_type(signaling.sdp,
                                                 config_.audioCodec,
                                                 audioPayloadType)) {
            LOG_ERROR("[WEBRTC] session=%d offer rejected: no usable audio payload type codec=%d",
                      id_,
                      static_cast<int>(config_.audioCodec));
            close();
            return;
        }
        addAudioTrack(audioMid, audioPayloadType, config_.audioCodec);
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
        pc = transport_.pc;
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
        {
            std::lock_guard<std::mutex> lock(mutex_);

            ++counters_.remoteCandidates;
        }
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
        connection = transport_.connection;
    }
    if (!connection || !connection->isOpen()) {
        LOG_WARN("[WEBRTC] session=%d send description failed: websocket not open", id_);
        return;
    }

    message = signaling_make_description(description.typeString(), description.generateSdp());
    connection->sendText(message);
    {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.signalingTxMessages;
    }
}

/* 把本地 ICE candidate 通过 WebSocket 发给浏览器。 */
void WebRtcSession::sendCandidate(const rtc::Candidate &candidate)
{
    std::shared_ptr<communication::WebSocketConnection> connection;
    std::string message;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection = transport_.connection;
    }
    if (!connection || !connection->isOpen()) {
        LOG_WARN("[WEBRTC] session=%d send candidate failed: websocket not open", id_);
        return;
    }

    message = signaling_make_candidate(candidate);
    connection->sendText(message);
    {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.signalingTxMessages;
        ++counters_.localCandidates;
    }
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
        transport_.dc = dc;
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

    {
        std::lock_guard<std::mutex> lock(mutex_);

        ++counters_.dataChannelRxMessages;
        counters_.dataChannelRxBytes += message.size();
    }

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
        dc = transport_.dc;
    }
    if (dc && dc->isOpen()) {
        dc->send(response);
        {
            std::lock_guard<std::mutex> lock(mutex_);

            ++counters_.dataChannelTxMessages;
            counters_.dataChannelTxBytes += response.size();
        }
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
    std::shared_ptr<rtc::PliHandler> pliHandler;
    std::weak_ptr<WebRtcSession> weakSession;
    std::string cname;
    std::string msid;
    uint32_t ssrc;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pc = transport_.pc;
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
    weakSession = shared_from_this();

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
    /*
     * PliHandler 会解析浏览器发回的 RTCP PLI（PT=206/FMT=1）和 FIR。
     * 浏览器解码器丢失关键帧或参考帧时会发送这些反馈；回调不直接操作 MPP，
     * 而是交给 WebRtcServer 与新客户端请求共用同一套 IDR 合并和限频策略。
     */
    pliHandler = std::make_shared<rtc::PliHandler>([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        /* 回调由 libdatachannel 的 RTCP 接收路径触发，先提升 weak_ptr，
         * 防止浏览器断开并释放 session 后仍访问悬空对象。 */
        session = weakSession.lock();
        if (session) {
            session->requestVideoKeyframe(WEBRTC_KEYFRAME_REQUEST_PLI);
        }
    });
    packetizer->addToChain(srReporter);
    packetizer->addToChain(nackResponder);
    packetizer->addToChain(pliHandler);
    track->setMediaHandler(packetizer);

    /*
     * onOpen 调用时机：
     * - Answer 已发给浏览器。
     * - ICE 连通、DTLS 握手和 SRTP 密钥协商已经完成。
     * - 这条 video Track 已经具备发送 RTP 视频帧的条件。
     *
     * 注意：Track 打开只表示设备端可以发送受 SRTP 保护的 RTP，
     * 不表示浏览器已经渲染出图；新会话仍需等待并发送一个可解码的 H264 IDR。
     * 它与 WebSocket 信令连接、DataChannel 的 onOpen 是彼此独立的状态回调。
     *
     * 当前模块的视频帧来自 mediaOutput 队列，所以这里不启动独立发送线程；
     * 回调中置位等待关键帧状态并请求编码器输出 IDR；sendVideoFrame()
     * 会通过 track->isOpen() 和该状态决定是否真正发送。
     */
    track->onOpen([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            {
                std::lock_guard<std::mutex> lock(session->mutex_);

                session->state_.waitingForVideoKeyframe = true;
            }
            LOG_INFO("[WEBRTC] session=%d video track open", session->id_);
            /* 新会话不能从当前 GOP 中间的 P/B 帧开始，因此立即请求一帧 IDR。 */
            session->requestVideoKeyframe(WEBRTC_KEYFRAME_REQUEST_NEW_SESSION);
        }
    });

    /*
     * onClosed 调用时机：
     * - 浏览器关闭 PeerConnection 或页面断开。
     * - ICE/DTLS 失败或服务端主动关闭 PeerConnection。
     *
     * 该回调只说明媒体 Track 不再能收发 RTP，不必然表示 WebSocket 已关闭；
     * 信令 WebSocket、DataChannel 会各自触发独立的关闭回调。
     * 关闭后 mediaOutput 后续送来的帧会在 sendVideoFrame() 中被 isOpen() 拦截。
     */
    track->onClosed([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d video track closed", session->id_);
        }
    });

    /*
     * onError 调用时机：
     * - Track 底层 RTP/RTCP 处理链、SRTP 发送或 libdatachannel 内部状态发生错误时触发。
     * - 它不等同于浏览器的解码错误；浏览器端解码失败需要从 DevTools 的 WebRTC
     *   统计或控制台单独分析。
     *
     * 当前回调只记录错误。后续的 onClosed 或 PeerConnection 状态回调会负责
     * 生命周期收敛，不能在此处直接销毁 session，避免回调线程重入关闭流程。
     */
    track->onError([weakSession](std::string error) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_ERROR("[WEBRTC] session=%d video track error=%s", session->id_, error.c_str());
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);

        /*
         * 从创建 Track 起就禁止发送非关键帧。
         * 这样即使 Track 的 isOpen() 先变为 true、onOpen 回调稍后才被调度，
         * 也不会在该极小时间窗口把 P/B 帧发送给新浏览器。
         */
        state_.waitingForVideoKeyframe = true;
        transport_.videoTrack = track;
    }

    LOG_INFO("[WEBRTC] session=%d add h264 track mid=%s pt=%u", id_, mid.c_str(), payloadType);
}

/*
 * 添加 G711/Opus sendonly Track，并配置对应音频 RTP/RTCP 处理链。
 * 调用时机：收到浏览器 Offer 后，确认 m=audio、mid 和目标 codec PT 都可用时调用。
 */
void WebRtcSession::addAudioTrack(const std::string &mid,
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
        pc = transport_.pc;
    }
    if (!pc) {
        LOG_ERROR("[WEBRTC] session=%d add audio track failed: PeerConnection is NULL", id_);
        return;
    }
    if (codec != WEBRTC_AUDIO_CODEC_PCMA && codec != WEBRTC_AUDIO_CODEC_PCMU &&
        codec != WEBRTC_AUDIO_CODEC_OPUS) {
        LOG_ERROR("[WEBRTC] session=%d add audio track failed: unsupported codec=%d",
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
    } else if (codec == WEBRTC_AUDIO_CODEC_PCMU) {
        audio.addPCMUCodec(payloadType);
    } else {
        /*
         * libdatachannel 的默认 Opus profile 固定声明 stereo=1。
         * 根据实际编码声道数生成 fmtp，避免单声道负载被浏览器按双声道能力解释。
         */
        audio.addOpusCodec(payloadType,
                           config_.audioChannels == 2
                               ? "minptime=10;stereo=1;sprop-stereo=1;useinbandfec=1"
                               : "minptime=10;stereo=0;sprop-stereo=0;useinbandfec=1");
    }
    audio.addSSRC(ssrc, cname, msid, cname);
    track = pc->addTrack(audio);

    rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, cname, payloadType,
        codec == WEBRTC_AUDIO_CODEC_OPUS ? rtc::OpusRtpPacketizer::DefaultClockRate
                                        : rtc::PCMARtpPacketizer::DefaultClockRate);
    if (codec == WEBRTC_AUDIO_CODEC_PCMA) {
        packetizer = std::make_shared<rtc::PCMARtpPacketizer>(rtpConfig);
    } else if (codec == WEBRTC_AUDIO_CODEC_PCMU) {
        packetizer = std::make_shared<rtc::PCMURtpPacketizer>(rtpConfig);
    } else {
        /* 编码器已经产出完整裸 Opus packet，AudioRtpPacketizer 每帧生成一个 RTP 包。 */
        packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    }
    srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(srReporter);
    packetizer->addToChain(nackResponder);
    track->setMediaHandler(packetizer);

    weakSession = shared_from_this();
    /*
     * onOpen 调用时机：
     * - Answer 已完成协商，ICE、DTLS 和 SRTP 已建立。
     * - 这条 audio Track 可以开始发送加密的 RTP 音频包。
     *
     * 音频不依赖视频 IDR，所以这里只记录状态，不触发关键帧请求。
     * Track 打开也不代表浏览器扬声器已经听到声音，仍会受到浏览器自动播放策略、
     * 音频缓冲和本地静音状态影响。
     */
    track->onOpen([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d audio track open", session->id_);
        }
    });
    /*
     * onClosed 调用时机：
     * - 浏览器刷新、关闭页面或关闭 PeerConnection。
     * - ICE/DTLS 失败，或设备端主动关闭 PeerConnection。
     *
     * 回调后 audio Track 不可再发送 RTP。WebSocket 和 DataChannel 是否仍然打开
     * 由它们各自的状态回调决定，不能据此判断整个会话已经销毁。
     */
    track->onClosed([weakSession]() {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_INFO("[WEBRTC] session=%d audio track closed", session->id_);
        }
    });
    /*
     * onError 调用时机：音频 Track 的 RTP/RTCP/SRTP 处理链或 libdatachannel
     * 内部发生异常时触发。该错误不等同于浏览器端音频解码或播放错误。
     *
     * 当前仅记录日志，实际资源回收仍由 onClosed 和 PeerConnection 状态回调完成，
     * 避免在底层回调线程中直接关闭 session 造成重入。
     */
    track->onError([weakSession](std::string error) {
        std::shared_ptr<WebRtcSession> session;

        session = weakSession.lock();
        if (session) {
            LOG_ERROR("[WEBRTC] session=%d audio track error=%s", session->id_, error.c_str());
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport_.audioTrack = track;
    }

    LOG_INFO("[WEBRTC] session=%d add audio track mid=%s pt=%u codec=%d",
             id_,
             mid.c_str(),
             payloadType,
             static_cast<int>(codec));
}

} // namespace webrtc
} // namespace rkmedia
