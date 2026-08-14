#include "../inc/webrtcServer.h"

#include <exception>
#include <rtc/global.hpp>
#include <vector>

#include "logger.h"

namespace rkmedia {
namespace webrtc {

/* PLI/FIR 触发的 IDR 最小间隔，兼顾关键帧恢复速度和编码器码率稳定性。 */
static const std::chrono::milliseconds WEBRTC_PLI_IDR_MIN_INTERVAL(1000);

WebRtcServer::WebRtcServer()
{
    nextSessionId_ = 1;
    running_ = false;
}

WebRtcServer::~WebRtcServer()
{
    stop();
}

/*
 * 启动 WebRTC 输出服务。
 * 当前服务包含一个 WebSocket 信令监听器，浏览器连接后再按需创建 PeerConnection。
 */
bool WebRtcServer::start(const WebRtcServerConfig &config)
{
    std::shared_ptr<communication::WebSocketServer> wsServer;

    config_ = config;
    try {
        /* 目前每个输出通道都会创建一个webSocketServer和webRTCServer */
        /**
         * 后面做公网 WebSocketClient 注册模式，最好是一个统一信令层，而不是每个输出通道都自己起一个 server
         */
        wsServer = std::make_shared<communication::WebSocketServer>();
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBRTC] create websocket server failed: %s", e.what());
        return false;
    }
    wsServer->onClient([this](const std::shared_ptr<communication::WebSocketConnection> &connection) {
        handleClient(connection);
    });

    if (!wsServer->start(config.bindAddress, config.port)) {
        LOG_ERROR("[WEBRTC] websocket server start failed bind=%s port=%u",
                  config.bindAddress.c_str(),
                  config.port);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        wsServer_ = wsServer;
        running_ = true;
    }

    LOG_INFO("[WEBRTC] server started bind=%s port=%u",
             config.bindAddress.c_str(),
             wsServer->port());
    return true;
}

/* 停止 WebRTC 输出服务，并关闭所有浏览器会话。 */
void WebRtcServer::stop()
{
    std::map<int, std::shared_ptr<WebRtcSession>> sessions;
    std::shared_ptr<communication::WebSocketServer> wsServer;
    std::map<int, std::shared_ptr<WebRtcSession>>::iterator iter;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !wsServer_) {
            LOG_DEBUG("[WEBRTC] server stop ignored: already stopped");
            return;
        }
        running_ = false;
        wsServer = wsServer_;
        sessions.swap(sessions_);
        wsServer_.reset();
    }

    for (iter = sessions.begin(); iter != sessions.end(); ++iter) {
        iter->second->close();
    }
    if (wsServer) {
        wsServer->stop();
    }
}

/*
 * 广播一帧 H264 到所有已就绪的浏览器会话。
 * mediaOutput 层已经负责队列、关键帧保护和统计，这里只负责实际 WebRTC 发送。
 */
int WebRtcServer::sendVideoFrame(const WebRtcVideoFrame &frame)
{
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    std::map<int, std::shared_ptr<WebRtcSession>>::iterator iter;
    std::chrono::steady_clock::time_point now;
    size_t i;
    int sentCount;

    sentCount = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            LOG_WARN("[WEBRTC] send video ignored: server not running");
            return 0;
        }
        ++mediaCounters_.inputVideoFrames;
        mediaCounters_.inputVideoBytes += frame.size;
        if (frame.keyFrame) {
            /* 此时 MPP 已经产出 IDR 且媒体帧进入 WebRTC，作为请求到编码完成的终点。 */
            now = std::chrono::steady_clock::now();
            keyframeState_.lastVideoIdrInputTime = now;
            keyframeState_.hasLastVideoIdrInputTime = true;
            if (keyframeState_.waitingVideoIdrInput) {
                keyframeState_.lastIdrRequestToInputMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - keyframeState_.waitingVideoIdrRequestTime)
                        .count());
                keyframeState_.hasLastIdrRequestToInputDelay = true;
                keyframeState_.waitingVideoIdrInput = false;
            }
        }
        for (iter = sessions_.begin(); iter != sessions_.end(); ++iter) {
            sessions.push_back(iter->second);
        }
    }

    if (sessions.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++mediaCounters_.videoNoReadySession;
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

        mediaCounters_.videoBroadcastTargets += static_cast<uint64_t>(sentCount);
        if (sentCount <= 0) {
            ++mediaCounters_.videoNoReadySession;
        }
    }

    return sentCount;
}

/* 返回当前 WebRTC 会话数量。 */
/*
 * 广播一帧已编码音频到所有已就绪且编码类型匹配的浏览器会话。
 * mediaOutput 负责队列和生命周期，这里只遍历当前 session 快照，避免持锁发送。
 */
int WebRtcServer::sendAudioFrame(const WebRtcAudioFrame &frame)
{
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    std::map<int, std::shared_ptr<WebRtcSession>>::iterator iter;
    size_t i;
    int sentCount;

    sentCount = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            LOG_WARN("[WEBRTC] send audio ignored: server not running");
            return 0;
        }
        ++mediaCounters_.inputAudioFrames;
        mediaCounters_.inputAudioBytes += frame.size;
        for (iter = sessions_.begin(); iter != sessions_.end(); ++iter) {
            sessions.push_back(iter->second);
        }
    }

    if (sessions.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);

        ++mediaCounters_.audioNoReadySession;
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

        mediaCounters_.audioBroadcastTargets += static_cast<uint64_t>(sentCount);
        if (sentCount <= 0) {
            ++mediaCounters_.audioNoReadySession;
        }
    }

    return sentCount;
}

int WebRtcServer::sessionCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return static_cast<int>(sessions_.size());
}

/*
 * 消费待发送给 MPP 编码器的一次性 IDR 请求。
 * 新客户端请求会立即消费；PLI/FIR 若处于限频窗口，则保留为延迟请求，
 * 到达最小间隔后由下一轮编码自动消费，保证恢复不会因单次 PLI 被永久忽略。
 */
bool WebRtcServer::consumeVideoKeyframeRequest()
{
    std::chrono::steady_clock::time_point now;
    bool pliRequestDue;
    std::lock_guard<std::mutex> lock(mutex_);

    now = std::chrono::steady_clock::now();
    /*
     * pendingPliKeyframeRequest 只会在已有 IDR 请求时间且仍处于限频窗口时置位，
     * 因此其为 true 时 hasLastVideoIdrRequestTime 必然为 true，可以直接比较时间间隔。
     */
    pliRequestDue = keyframeState_.pendingPliKeyframeRequest &&
                    now - keyframeState_.lastVideoIdrRequestTime >= WEBRTC_PLI_IDR_MIN_INTERVAL;

    /* 立即请求优先：新会话或未受限 PLI 都应在下一帧编码前直接请求 IDR。 */
    if (keyframeState_.pendingVideoKeyframeRequest) {
        keyframeState_.pendingVideoKeyframeRequest = false;
        /* 即将生成的同一帧 IDR 同时满足此前延迟的 PLI，无需再次请求。 */
        keyframeState_.pendingPliKeyframeRequest = false;
        recordVideoIdrRequestConsumed(now);
        return true;
    } else if (pliRequestDue) {
        /* 限频窗口结束，消费合并后的一个 PLI 请求；多个 PLI 只产生一个 IDR。 */
        keyframeState_.pendingPliKeyframeRequest = false;
        recordVideoIdrRequestConsumed(now);
        ++keyframeState_.videoKeyframeRequests;
        LOG_INFO("[WEBRTC] PLI rate limit elapsed, request IDR");
        return true;
    }
    return false;
}

/*
 * 启动或取消指定浏览器 session 的 PLI 测试。
 * 仅把会话对象副本带出 server 锁，避免在 session 内部加锁时形成嵌套锁依赖。
 */
bool WebRtcServer::setSessionPliTestVideoSuppression(int sessionId, bool enabled)
{
    std::shared_ptr<WebRtcSession> session;
    std::map<int, std::shared_ptr<WebRtcSession>>::iterator iter;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            LOG_WARN("[WEBRTC] PLI test ignored: server not running session=%d", sessionId);
            return false;
        }
        iter = sessions_.find(sessionId);
        if (iter == sessions_.end()) {
            LOG_WARN("[WEBRTC] PLI test failed: session=%d not found", sessionId);
            return false;
        }
        session = iter->second;
    }

    if (!session->setPliTestVideoSuppression(enabled)) {
        LOG_WARN("[WEBRTC] PLI test failed: session=%d video track not ready or closed", sessionId);
        return false;
    }
    return true;
}

/*
 * 记录一次外部 IDR 请求已被 mediaGateway 消费。
 * 调用方已持有 mutex_；mediaGateway 随后会在同一帧处理周期内调用 mpp_encoder_request_idr。
 */
void WebRtcServer::recordVideoIdrRequestConsumed(const std::chrono::steady_clock::time_point &now)
{
    keyframeState_.lastVideoIdrRequestTime = now;
    keyframeState_.hasLastVideoIdrRequestTime = true;
    keyframeState_.waitingVideoIdrRequestTime = now;
    keyframeState_.waitingVideoIdrInput = true;

    if (keyframeState_.hasPendingPliTriggerTime) {
        keyframeState_.lastPliToIdrRequestMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - keyframeState_.pendingPliTriggerTime)
                .count());
        keyframeState_.hasLastPliToIdrRequestDelay = true;
        keyframeState_.hasPendingPliTriggerTime = false;
    }
}

/*
 * 生成 WebRTC server 统计快照。
 * 调用时机：shell 调试命令查询 WebRTC 状态时调用；先复制 session 指针，再逐个读取 session 快照，避免长时间持有 server 锁。
 */
void WebRtcServer::getStats(WebRtcServerStats &stats) const
{
    std::vector<std::shared_ptr<WebRtcSession>> sessions;
    std::map<int, std::shared_ptr<WebRtcSession>>::const_iterator iter;
    std::chrono::steady_clock::time_point now;
    size_t i;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        now = std::chrono::steady_clock::now();
        stats.name = config_.name;
        stats.bindAddress = config_.bindAddress;
        stats.port = config_.port;
        stats.running = running_;
        stats.videoFps = config_.videoFps;
        stats.audioCodec = config_.audioCodec;
        stats.audioSampleRate = config_.audioSampleRate;
        stats.audioChannels = config_.audioChannels;
        stats.inputVideoFrames = mediaCounters_.inputVideoFrames;
        stats.inputVideoBytes = mediaCounters_.inputVideoBytes;
        stats.inputAudioFrames = mediaCounters_.inputAudioFrames;
        stats.inputAudioBytes = mediaCounters_.inputAudioBytes;
        stats.videoBroadcastTargets = mediaCounters_.videoBroadcastTargets;
        stats.audioBroadcastTargets = mediaCounters_.audioBroadcastTargets;
        stats.videoNoReadySession = mediaCounters_.videoNoReadySession;
        stats.audioNoReadySession = mediaCounters_.audioNoReadySession;
        stats.pendingVideoKeyframeRequest = keyframeState_.pendingVideoKeyframeRequest;
        stats.pendingPliKeyframeRequest = keyframeState_.pendingPliKeyframeRequest;
        stats.videoKeyframeRequests = keyframeState_.videoKeyframeRequests;
        stats.videoPliReceived = keyframeState_.videoPliReceived;
        stats.videoPliAccepted = keyframeState_.videoPliAccepted;
        stats.videoPliDeferred = keyframeState_.videoPliDeferred;
        stats.hasLastVideoPliTime = keyframeState_.hasLastVideoPliTime;
        stats.hasLastVideoIdrRequestTime = keyframeState_.hasLastVideoIdrRequestTime;
        stats.hasLastVideoIdrInputTime = keyframeState_.hasLastVideoIdrInputTime;
        stats.lastVideoPliAgeMs = stats.hasLastVideoPliTime
                                      ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                      now - keyframeState_.lastVideoPliTime)
                                                                      .count())
                                      : 0;
        stats.lastVideoIdrRequestAgeMs = stats.hasLastVideoIdrRequestTime
                                             ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                             now - keyframeState_.lastVideoIdrRequestTime)
                                                                             .count())
                                             : 0;
        stats.lastVideoIdrInputAgeMs = stats.hasLastVideoIdrInputTime
                                           ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                           now - keyframeState_.lastVideoIdrInputTime)
                                                                           .count())
                                           : 0;
        stats.hasLastPliToIdrRequestDelay = keyframeState_.hasLastPliToIdrRequestDelay;
        stats.hasLastIdrRequestToInputDelay = keyframeState_.hasLastIdrRequestToInputDelay;
        stats.lastPliToIdrRequestMs = keyframeState_.lastPliToIdrRequestMs;
        stats.lastIdrRequestToInputMs = keyframeState_.lastIdrRequestToInputMs;
        stats.sessions.clear();

        for (iter = sessions_.begin(); iter != sessions_.end(); ++iter) {
            sessions.push_back(iter->second);
        }
    }

    stats.sessions.resize(sessions.size());
    for (i = 0; i < sessions.size(); ++i) {
        sessions[i]->getStats(stats.sessions[i]);
    }
}

/*
 * 新浏览器 WebSocket 连接入口。
 * 这里只创建会话对象并绑定回调，真正的 PeerConnection 在收到 offer 后创建。
 */
void WebRtcServer::handleClient(const std::shared_ptr<communication::WebSocketConnection> &connection)
{
    std::shared_ptr<WebRtcSession> session;
    int id;

    if (!connection) {
        LOG_ERROR("[WEBRTC] new browser ignored: connection is NULL");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            LOG_WARN("[WEBRTC] new browser ignored: server not running");
            return;
        }
        id = nextSessionId_++;
        session = std::make_shared<WebRtcSession>(
            id,
            config_,
            connection,
            [this](int sessionId) {
                removeSession(sessionId);
            },
            [this](int sessionId, WebRtcKeyframeRequestReason reason) {
                requestVideoKeyframe(sessionId, reason);
            });
        sessions_[id] = session;
    }

    LOG_INFO("[WEBRTC] new browser session=%d remote=%s path=%s",
             id,
             connection->remoteAddress().c_str(),
             connection->path().c_str());
    session->start();
}

/*
 * 处理 WebRtcSession 的关键帧请求。
 * 新 video Track 打开时立即请求 IDR，保证新浏览器不必等下一个 GOP；浏览器 PLI/FIR
 * 则按 1 秒最小间隔限频。限频内的 PLI 不丢失，而是合并成一个延迟请求。
 */
void WebRtcServer::requestVideoKeyframe(int sessionId, WebRtcKeyframeRequestReason reason)
{
    std::chrono::steady_clock::time_point now;
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_) {
        LOG_WARN("[WEBRTC] session=%d keyframe request ignored: server not running", sessionId);
        return;
    }

    now = std::chrono::steady_clock::now();
    switch (reason) {
    case WEBRTC_KEYFRAME_REQUEST_PLI:
        /*
         * PLI/FIR 表示已建立会话的浏览器解码参考链丢失。
         * 此类请求必须限频，避免网络异常时浏览器连续反馈造成 I 帧风暴。
         */
        ++keyframeState_.videoPliReceived;
        /* 记录浏览器 PLI/FIR 抵达本模块的时刻，用于量化反馈到 IDR 的恢复链路。 */
        keyframeState_.lastVideoPliTime = now;
        keyframeState_.hasLastVideoPliTime = true;
        if (!keyframeState_.hasPendingPliTriggerTime) {
            /* 多个 PLI 合并为同一次 IDR 时，统计从首个触发 PLI 开始计算恢复时间。 */
            keyframeState_.pendingPliTriggerTime = now;
            keyframeState_.hasPendingPliTriggerTime = true;
        }
        if (keyframeState_.pendingVideoKeyframeRequest) {
            /* 已有 IDR 会在下一帧编码前请求，本次 PLI 由该 IDR 一并满足。 */
            ++keyframeState_.videoPliDeferred;
            return;
        }
        if (keyframeState_.hasLastVideoIdrRequestTime &&
            now - keyframeState_.lastVideoIdrRequestTime < WEBRTC_PLI_IDR_MIN_INTERVAL) {
            /* 不立即再强制 I 帧，保留一个待执行标记；consume() 到时会自动触发。 */
            keyframeState_.pendingPliKeyframeRequest = true;
            ++keyframeState_.videoPliDeferred;
            return;
        }
        keyframeState_.pendingVideoKeyframeRequest = true;
        /* 当前不在限频窗口，PLI 可以直接驱动下一轮编码输出 IDR。 */
        ++keyframeState_.videoKeyframeRequests;
        ++keyframeState_.videoPliAccepted;
        LOG_INFO("[WEBRTC] session=%d PLI received, request IDR", sessionId);
        return;

    case WEBRTC_KEYFRAME_REQUEST_NEW_SESSION:
        /*
         * 新 video Track 不能从当前 GOP 的 P/B 帧开始解码，必须尽快获得 IDR。
         * 多个新会话共用同一个待请求 IDR；该 IDR 也能一并满足已延迟的 PLI。
         */
        if (keyframeState_.pendingVideoKeyframeRequest) {
            /* 多个浏览器同时打开时复用同一个即将生成的 IDR，避免接入高峰产生 I 帧风暴。 */
            return;
        }
        keyframeState_.pendingVideoKeyframeRequest = true;
        /* 新客户端优先于此前的延迟 PLI，下一帧 IDR 可同时满足两种需求。 */
        keyframeState_.pendingPliKeyframeRequest = false;
        ++keyframeState_.videoKeyframeRequests;
        LOG_INFO("[WEBRTC] session=%d video ready, request IDR", sessionId);
        return;

    case WEBRTC_KEYFRAME_REQUEST_TEST_TIMEOUT:
        /* PLI 测试保护超时后恢复视频；语义与新会话相同，均需要下一帧可解码 IDR。 */
        if (keyframeState_.pendingVideoKeyframeRequest) {
            return;
        }
        keyframeState_.pendingVideoKeyframeRequest = true;
        keyframeState_.pendingPliKeyframeRequest = false;
        ++keyframeState_.videoKeyframeRequests;
        LOG_WARN("[WEBRTC] session=%d PLI test timeout, request recovery IDR", sessionId);
        return;

    default:
        /* 防止后续扩展枚举但遗漏调度策略时，错误地按新会话路径请求 IDR。 */
        LOG_ERROR("[WEBRTC] session=%d keyframe request ignored: unsupported reason=%d",
                  sessionId,
                  static_cast<int>(reason));
        return;
    }
}

/* 从会话表移除指定会话，浏览器断开或 PeerConnection 关闭后调用。 */
void WebRtcServer::removeSession(int id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    LOG_INFO("[WEBRTC] remove browser session=%d", id);
    sessions_.erase(id);
}

} // namespace webrtc
} // namespace rkmedia
