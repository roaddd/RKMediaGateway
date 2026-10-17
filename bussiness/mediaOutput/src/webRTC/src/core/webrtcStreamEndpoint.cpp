/**
 * @file webrtcStreamEndpoint.cpp
 * @brief 实现单路 WebRTC 输出的多会话管理、媒体扇出及关键帧调度。
 */
#include "webrtcStreamEndpoint.h"

#include <exception>
#include <vector>

#include "logger.h"

namespace rkmedia {
namespace webrtc {

/* PLI/FIR 触发的 IDR 最小间隔，兼顾关键帧恢复速度和编码器码率稳定性。 */
static const std::chrono::milliseconds WEBRTC_PLI_IDR_MIN_INTERVAL(1000);

WebRtcStreamEndpoint::WebRtcStreamEndpoint()
    : running_(false)
{
}

WebRtcStreamEndpoint::~WebRtcStreamEndpoint()
{
    stop();
}

/*
 * 启动单路媒体端点。
 *
 * 这里只保存与 PeerConnection 和媒体处理有关的配置并开放会话入口。
 * 本地监听或云端注册必须由外部信令适配器在本函数成功后启动。
 */
bool WebRtcStreamEndpoint::start(const WebRtcStreamEndpointConfig &config)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) {
        LOG_ERROR("[WEBRTC] stream endpoint start failed: already running name=%s",
                  config_.name.c_str());
        return false;
    }
    config_ = config;
    running_ = true;
    LOG_INFO("[WEBRTC] stream endpoint started name=%s", config_.name.c_str());
    return true;
}

/* 停止媒体端点并关闭全部会话；调用方必须先停止信令适配器以阻止新会话进入。 */
void WebRtcStreamEndpoint::stop()
{
    std::map<std::string, std::shared_ptr<WebRtcSession>> sessions;
    std::map<std::string, std::shared_ptr<WebRtcSession>>::iterator iter;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            LOG_DEBUG("[WEBRTC] stream endpoint stop ignored: already stopped");
            return;
        }
        running_ = false;
        sessions.swap(sessions_);
    }

    for (iter = sessions.begin(); iter != sessions.end(); ++iter) {
        iter->second->close();
    }
}

/*
 * 广播一帧 H264 到所有已就绪的浏览器会话。
 * mediaOutput 层已经负责队列、关键帧保护和统计，这里只负责实际 WebRTC 发送。
 */
int WebRtcStreamEndpoint::sendVideoFrame(const WebRtcVideoFrame &frame)
{
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    std::map<std::string, std::shared_ptr<WebRtcSession>>::iterator iter;
    std::chrono::steady_clock::time_point now;
    size_t i;
    int sentCount;

    sentCount = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            LOG_WARN("[WEBRTC] send video ignored: stream endpoint not running");
            return 0;
        }
        ++mediaCounters_.videoInput.frames;
        mediaCounters_.videoInput.bytes += frame.size;
        if (frame.keyFrame) {
            /* 此时 MPP 已经产出 IDR 且媒体帧进入 WebRTC，作为请求到编码完成的终点。 */
            now = std::chrono::steady_clock::now();
            keyframeState_.timing.lastIdrInputTime = now;
            keyframeState_.timing.hasLastIdrInputTime = true;
            if (keyframeState_.pending.waitingIdrInput) {
                keyframeState_.timing.lastIdrRequestToInputMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - keyframeState_.timing.waitingIdrRequestTime)
                        .count());
                keyframeState_.timing.hasLastIdrRequestToInputDelay = true;
                keyframeState_.pending.waitingIdrInput = false;
            }
        }
        for (iter = sessions_.begin();
             iter != sessions_.end();
             ++iter) {
            sessions.push_back(iter->second);
        }
    }

    if (sessions.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++mediaCounters_.videoBroadcast.noReadySession;
        LOG_DEBUG("[WEBRTC] send video ignored: no browser session");
        return 0;
    }

    for (i = 0; i < sessions.size(); ++i) {
        if (sessions[i]->sendVideoFrame(frame)) {
            ++sentCount;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);

        mediaCounters_.videoBroadcast.targets += static_cast<uint64_t>(sentCount);
        if (sentCount <= 0) {
            ++mediaCounters_.videoBroadcast.noReadySession;
        }
    }

    return sentCount;
}

/* 返回当前 WebRTC 会话数量。 */
/*
 * 广播一帧已编码音频到所有已就绪且编码类型匹配的浏览器会话。
 * mediaOutput 负责队列和生命周期，这里只遍历当前 session 快照，避免持锁发送。
 */
int WebRtcStreamEndpoint::sendAudioFrame(const WebRtcAudioFrame &frame)
{
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    std::map<std::string, std::shared_ptr<WebRtcSession>>::iterator iter;
    size_t i;
    int sentCount;

    sentCount = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            LOG_WARN("[WEBRTC] send audio ignored: stream endpoint not running");
            return 0;
        }
        ++mediaCounters_.audioInput.frames;
        mediaCounters_.audioInput.bytes += frame.size;
        for (iter = sessions_.begin();
             iter != sessions_.end();
             ++iter) {
            sessions.push_back(iter->second);
        }
    }

    if (sessions.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++mediaCounters_.audioBroadcast.noReadySession;
        LOG_DEBUG("[WEBRTC] send audio ignored: no browser session");
        return 0;
    }

    for (i = 0; i < sessions.size(); ++i) {
        if (sessions[i]->sendAudioFrame(frame)) {
            ++sentCount;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);

        mediaCounters_.audioBroadcast.targets += static_cast<uint64_t>(sentCount);
        if (sentCount <= 0) {
            ++mediaCounters_.audioBroadcast.noReadySession;
        }
    }

    return sentCount;
}

int WebRtcStreamEndpoint::sessionCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return static_cast<int>(sessions_.size());
}

/*
 * 消费待发送给 MPP 编码器的一次性 IDR 请求。
 * 新客户端请求会立即消费；PLI/FIR 若处于限频窗口，则保留为延迟请求，
 * 到达最小间隔后由下一轮编码自动消费，保证恢复不会因单次 PLI 被永久忽略。
 */
bool WebRtcStreamEndpoint::consumeVideoKeyframeRequest()
{
    std::chrono::steady_clock::time_point now;
    bool pliRequestDue;
    std::lock_guard<std::mutex> lock(mutex_);

    now = std::chrono::steady_clock::now();
    /*
     * pendingPliKeyframeRequest 只会在已有 IDR 请求时间且仍处于限频窗口时置位，
     * 因此其为 true 时 hasLastVideoIdrRequestTime 必然为 true，可以直接比较时间间隔。
     */
    pliRequestDue = keyframeState_.pending.deferredPliRequest &&
                    now - keyframeState_.timing.lastIdrRequestTime >= WEBRTC_PLI_IDR_MIN_INTERVAL;

    /* 立即请求优先：新会话或未受限 PLI 都应在下一帧编码前直接请求 IDR。 */
    if (keyframeState_.pending.immediateRequest) {
        keyframeState_.pending.immediateRequest = false;
        /* 即将生成的同一帧 IDR 同时满足此前延迟的 PLI，无需再次请求。 */
        keyframeState_.pending.deferredPliRequest = false;
        recordVideoIdrRequestConsumed(now);
        return true;
    } else if (pliRequestDue) {
        /* 限频窗口结束，消费合并后的一个 PLI 请求；多个 PLI 只产生一个 IDR。 */
        keyframeState_.pending.deferredPliRequest = false;
        recordVideoIdrRequestConsumed(now);
        ++keyframeState_.counters.requests;
        LOG_INFO("[WEBRTC] PLI rate limit elapsed, request IDR");
        return true;
    }
    return false;
}

/*
 * 启动或取消指定浏览器 session 的 PLI 测试。
 * 仅把会话对象副本带出端点锁，避免在 session 内部加锁时形成嵌套锁依赖。
 */
bool WebRtcStreamEndpoint::setSessionPliTestVideoSuppression(
    const std::string &sessionId,
    bool enabled)
{
    std::shared_ptr<WebRtcSession> session;
    std::map<std::string, std::shared_ptr<WebRtcSession>>::iterator iter;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            LOG_WARN("[WEBRTC] PLI test ignored: stream endpoint not running session=%s",
                     sessionId.c_str());
            return false;
        }
        iter = sessions_.find(sessionId);
        if (iter == sessions_.end()) {
            LOG_WARN("[WEBRTC] PLI test failed: session=%s not found", sessionId.c_str());
            return false;
        }
        session = iter->second;
    }

    if (!session->setPliTestVideoSuppression(enabled)) {
        LOG_WARN("[WEBRTC] PLI test failed: session=%s video track not ready or closed",
                 sessionId.c_str());
        return false;
    }
    return true;
}

/*
 * 记录一次外部 IDR 请求已被 mediaGateway 消费。
 * 调用方已持有 mutex_；mediaGateway 随后会在同一帧处理周期内调用 mpp_encoder_request_idr。
 */
void WebRtcStreamEndpoint::recordVideoIdrRequestConsumed(const std::chrono::steady_clock::time_point &now)
{
    keyframeState_.timing.lastIdrRequestTime = now;
    keyframeState_.timing.hasLastIdrRequestTime = true;
    keyframeState_.timing.waitingIdrRequestTime = now;
    keyframeState_.pending.waitingIdrInput = true;

    if (keyframeState_.timing.hasPendingPliTriggerTime) {
        keyframeState_.timing.lastPliToIdrRequestMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - keyframeState_.timing.pendingPliTriggerTime)
                .count());
        keyframeState_.timing.hasLastPliToIdrRequestDelay = true;
        keyframeState_.timing.hasPendingPliTriggerTime = false;
    }
}

/*
 * 生成 WebRTC 媒体端点统计快照。
 * 调用时机：shell 调试命令查询 WebRTC 状态时调用；先复制 session 指针，再逐个读取 session 快照，避免长时间持有端点锁。
 */
void WebRtcStreamEndpoint::getStats(WebRtcStreamEndpointStats &stats) const
{
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    std::map<std::string, std::shared_ptr<WebRtcSession>>::const_iterator iter;
    std::chrono::steady_clock::time_point now;
    std::function<bool()> signalingReadyCallback;
    size_t i;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        now = std::chrono::steady_clock::now();
        stats.name = config_.name;
        stats.streamId = config_.streamId;
        stats.signalingEndpoint = config_.signalingEndpoint;
        signalingReadyCallback = config_.signalingReadyCallback;
        stats.running = running_;
        stats.videoFps = config_.videoFps;
        stats.audioCodec = config_.audioCodec;
        stats.audioSampleRate = config_.audioSampleRate;
        stats.audioChannels = config_.audioChannels;
        stats.inputVideoFrames = mediaCounters_.videoInput.frames;
        stats.inputVideoBytes = mediaCounters_.videoInput.bytes;
        stats.inputAudioFrames = mediaCounters_.audioInput.frames;
        stats.inputAudioBytes = mediaCounters_.audioInput.bytes;
        stats.videoBroadcastTargets = mediaCounters_.videoBroadcast.targets;
        stats.audioBroadcastTargets = mediaCounters_.audioBroadcast.targets;
        stats.videoNoReadySession = mediaCounters_.videoBroadcast.noReadySession;
        stats.audioNoReadySession = mediaCounters_.audioBroadcast.noReadySession;
        stats.incomingAudioPackets = mediaCounters_.incomingAudio.packets;
        stats.incomingAudioPayloadBytes = mediaCounters_.incomingAudio.payloadBytes;
        stats.pendingVideoKeyframeRequest = keyframeState_.pending.immediateRequest;
        stats.pendingPliKeyframeRequest = keyframeState_.pending.deferredPliRequest;
        stats.videoKeyframeRequests = keyframeState_.counters.requests;
        stats.videoPliReceived = keyframeState_.counters.pliReceived;
        stats.videoPliAccepted = keyframeState_.counters.pliAccepted;
        stats.videoPliDeferred = keyframeState_.counters.pliDeferred;
        stats.hasLastVideoPliTime = keyframeState_.timing.hasLastPliTime;
        stats.hasLastVideoIdrRequestTime = keyframeState_.timing.hasLastIdrRequestTime;
        stats.hasLastVideoIdrInputTime = keyframeState_.timing.hasLastIdrInputTime;
        stats.lastVideoPliAgeMs = stats.hasLastVideoPliTime
                                      ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                      now - keyframeState_.timing.lastPliTime)
                                                                      .count())
                                      : 0;
        stats.lastVideoIdrRequestAgeMs = stats.hasLastVideoIdrRequestTime
                                             ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                             now - keyframeState_.timing.lastIdrRequestTime)
                                                                             .count())
                                             : 0;
        stats.lastVideoIdrInputAgeMs = stats.hasLastVideoIdrInputTime
                                           ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                           now - keyframeState_.timing.lastIdrInputTime)
                                                                           .count())
                                           : 0;
        stats.hasLastPliToIdrRequestDelay = keyframeState_.timing.hasLastPliToIdrRequestDelay;
        stats.hasLastIdrRequestToInputDelay = keyframeState_.timing.hasLastIdrRequestToInputDelay;
        stats.lastPliToIdrRequestMs = keyframeState_.timing.lastPliToIdrRequestMs;
        stats.lastIdrRequestToInputMs = keyframeState_.timing.lastIdrRequestToInputMs;
        stats.sessions.clear();

        for (iter = sessions_.begin();
             iter != sessions_.end();
             ++iter) {
            sessions.push_back(iter->second);
        }
    }

    stats.signalingConnected = signalingReadyCallback && signalingReadyCallback();
    stats.sessions.resize(sessions.size());
    for (i = 0; i < sessions.size(); ++i) {
        sessions[i]->getStats(stats.sessions[i]);
    }
}

/*
 * 统一的浏览器会话创建入口。
 * 云端客户端已经按 stream_id 找到本端点；端点使用云端 session_id 创建媒体会话，
 * 真正的 PeerConnection 仍在收到 offer 后创建。
 */
bool WebRtcStreamEndpoint::openSession(
    const std::string &sessionId,
    const WebRtcSessionSignalingSendCallback &signalingSendCallback,
    const WebRtcSessionSignalingCloseCallback &signalingCloseCallback)
{
    std::shared_ptr<WebRtcSession> session;

    if (sessionId.empty() || !signalingSendCallback || !signalingCloseCallback) {
        LOG_ERROR("[WEBRTC] new session rejected: invalid signaling context session=%s",
                  sessionId.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            LOG_WARN("[WEBRTC] new browser ignored: stream endpoint not running");
            return false;
        }
        if (sessions_.count(sessionId) != 0) {
            LOG_ERROR("[WEBRTC] new session rejected: duplicate session_id=%s",
                      sessionId.c_str());
            return false;
        }
        try {
            /* 每个浏览器连接独立拥有会话和后续创建的 PeerConnection。 */
            session = std::make_shared<WebRtcSession>(
                sessionId,
                config_,
                signalingSendCallback,
                signalingCloseCallback,
                [this](const std::string &closedSessionId) {
                    removeSession(closedSessionId);
                },
                [this](const std::string &requestSessionId,
                       WebRtcKeyframeRequestReason reason) {
                    requestVideoKeyframe(requestSessionId, reason);
                },
                [this](const WebRtcIncomingAudioPacket &packet) {
                    handleIncomingAudioPacket(packet);
                });
        } catch (const std::exception &e) {
            LOG_ERROR("[WEBRTC] session allocation failed id=%s: %s",
                      sessionId.c_str(),
                      e.what());
            return false;
        }
        sessions_[sessionId] = session;
    }

    LOG_INFO("[WEBRTC] new cloud session=%s", sessionId.c_str());
    return true;
}

void WebRtcStreamEndpoint::handleSessionSignalingMessage(const std::string &sessionId, const std::string &message)
{
    std::shared_ptr<WebRtcSession> session;
    std::map<std::string, std::shared_ptr<WebRtcSession>>::iterator iter;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        iter = sessions_.find(sessionId);
        if (iter == sessions_.end()) {
            LOG_ERROR("[WEBRTC] signaling message rejected: unknown session=%s",
                      sessionId.c_str());
            return;
        }
        session = iter->second;
    }
    session->handleSignalingMessage(message);
}

void WebRtcStreamEndpoint::handleSessionSignalingClosed(const std::string &sessionId)
{
    std::shared_ptr<WebRtcSession> session;
    std::map<std::string, std::shared_ptr<WebRtcSession>>::iterator iter;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        iter = sessions_.find(sessionId);
        if (iter == sessions_.end()) {
            return;
        }
        session = iter->second;
    }
    session->handleSignalingClosed();
}

/**
 * @brief 汇总浏览器到设备的有效音频 RTP 包。
 *
 * WebRtcAudioReceiver 已经完成 RTP 边界校验和 payload 拷贝。本阶段媒体端点只累计
 * 可观测数据，不直接解码或写 ALSA；下一阶段可在这里把 packet 投递到抖动缓冲队列。
 */
void WebRtcStreamEndpoint::handleIncomingAudioPacket(const WebRtcIncomingAudioPacket &packet)
{
    uint64_t packetCount = 0;
    std::function<void(const WebRtcIncomingAudioPacket &)> callback;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            LOG_WARN("[WEBRTC] session=%s incoming audio ignored: stream endpoint not running",
                     packet.sessionId.c_str());
            return;
        }
        ++mediaCounters_.incomingAudio.packets;
        mediaCounters_.incomingAudio.payloadBytes += packet.payload.size();
        packetCount = mediaCounters_.incomingAudio.packets;
        callback = config_.incomingAudioCallback;
    }

    /* 在端点锁外调用业务消费者，避免队列背压阻塞会话管理和统计读取。 */
    if (callback) {
        callback(packet);
    }

    /* 每 250 包（20 ms Opus 时约 5 秒）打印一次采样日志，避免逐包日志干扰媒体线程。 */
    if (packetCount == 1 || packetCount % 250 == 0) {
        LOG_INFO("[WEBRTC] incoming audio: session=%s packets=%llu pt=%u ssrc=%u seq=%u timestamp=%u payload=%zu",
                 packet.sessionId.c_str(),
                 static_cast<unsigned long long>(packetCount),
                 static_cast<unsigned int>(packet.payloadType),
                 static_cast<unsigned int>(packet.ssrc),
                 static_cast<unsigned int>(packet.sequenceNumber),
                 static_cast<unsigned int>(packet.rtpTimestamp),
                 packet.payload.size());
    }
}

/*
 * 处理 WebRtcSession 的关键帧请求。
 * 新 video Track 打开时立即请求 IDR，保证新浏览器不必等下一个 GOP；浏览器 PLI/FIR
 * 则按 1 秒最小间隔限频。限频内的 PLI 不丢失，而是合并成一个延迟请求。
 */
void WebRtcStreamEndpoint::requestVideoKeyframe(
    const std::string &sessionId,
    WebRtcKeyframeRequestReason reason)
{
    std::chrono::steady_clock::time_point now;
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_) {
        LOG_WARN("[WEBRTC] session=%s keyframe request ignored: stream endpoint not running",
                 sessionId.c_str());
        return;
    }

    now = std::chrono::steady_clock::now();
    switch (reason) {
    case WEBRTC_KEYFRAME_REQUEST_PLI:
        /*
         * PLI/FIR 表示已建立会话的浏览器解码参考链丢失。
         * 此类请求必须限频，避免网络异常时浏览器连续反馈造成 I 帧风暴。
         */
        ++keyframeState_.counters.pliReceived;
        /* 记录浏览器 PLI/FIR 抵达本模块的时刻，用于量化反馈到 IDR 的恢复链路。 */
        keyframeState_.timing.lastPliTime = now;
        keyframeState_.timing.hasLastPliTime = true;
        if (!keyframeState_.timing.hasPendingPliTriggerTime) {
            /* 多个 PLI 合并为同一次 IDR 时，统计从首个触发 PLI 开始计算恢复时间。 */
            keyframeState_.timing.pendingPliTriggerTime = now;
            keyframeState_.timing.hasPendingPliTriggerTime = true;
        }
        if (keyframeState_.pending.immediateRequest) {
            /* 已有 IDR 会在下一帧编码前请求，本次 PLI 由该 IDR 一并满足。 */
            ++keyframeState_.counters.pliDeferred;
            return;
        }
        if (keyframeState_.timing.hasLastIdrRequestTime &&
            now - keyframeState_.timing.lastIdrRequestTime < WEBRTC_PLI_IDR_MIN_INTERVAL) {
            /* 不立即再强制 I 帧，保留一个待执行标记；consume() 到时会自动触发。 */
            keyframeState_.pending.deferredPliRequest = true;
            ++keyframeState_.counters.pliDeferred;
            return;
        }
        keyframeState_.pending.immediateRequest = true;
        /* 当前不在限频窗口，PLI 可以直接驱动下一轮编码输出 IDR。 */
        ++keyframeState_.counters.requests;
        ++keyframeState_.counters.pliAccepted;
        LOG_INFO("[WEBRTC] session=%s PLI received, request IDR", sessionId.c_str());
        return;

    case WEBRTC_KEYFRAME_REQUEST_NEW_SESSION:
        /*
         * 新 video Track 不能从当前 GOP 的 P/B 帧开始解码，必须尽快获得 IDR。
         * 多个新会话共用同一个待请求 IDR；该 IDR 也能一并满足已延迟的 PLI。
         */
        if (keyframeState_.pending.immediateRequest) {
            /* 多个浏览器同时打开时复用同一个即将生成的 IDR，避免接入高峰产生 I 帧风暴。 */
            return;
        }
        keyframeState_.pending.immediateRequest = true;
        /* 新客户端优先于此前的延迟 PLI，下一帧 IDR 可同时满足两种需求。 */
        keyframeState_.pending.deferredPliRequest = false;
        ++keyframeState_.counters.requests;
        LOG_INFO("[WEBRTC] session=%s video ready, request IDR", sessionId.c_str());
        return;

    case WEBRTC_KEYFRAME_REQUEST_TEST_TIMEOUT:
        /* PLI 测试保护超时后恢复视频；语义与新会话相同，均需要下一帧可解码 IDR。 */
        if (keyframeState_.pending.immediateRequest) {
            return;
        }
        keyframeState_.pending.immediateRequest = true;
        keyframeState_.pending.deferredPliRequest = false;
        ++keyframeState_.counters.requests;
        LOG_WARN("[WEBRTC] session=%s PLI test timeout, request recovery IDR", sessionId.c_str());
        return;

    default:
        /* 防止后续扩展枚举但遗漏调度策略时，错误地按新会话路径请求 IDR。 */
        LOG_ERROR("[WEBRTC] session=%s keyframe request ignored: unsupported reason=%d",
                  sessionId.c_str(),
                  static_cast<int>(reason));
        return;
    }
}

/* 从会话表移除指定会话，浏览器断开或 PeerConnection 关闭后调用。 */
void WebRtcStreamEndpoint::removeSession(const std::string &sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);

    sessions_.erase(sessionId);
    LOG_INFO("[WEBRTC] remove browser session=%s", sessionId.c_str());
}

} // namespace webrtc
} // namespace rkmedia
