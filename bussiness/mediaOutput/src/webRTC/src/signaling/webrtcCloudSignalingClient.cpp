/**
 * @file webrtcCloudSignalingClient.cpp
 * @brief 实现网关唯一云端信令长连接、重连及会话路由。
 */
#include "webrtcCloudSignalingClient.h"

#include "cloudSignalingProtocol.h"
#include "websocketClient.h"
#include "websocketConnection.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>
#include <vector>

#include "logger.h"

namespace rkmedia {
namespace webrtc {

/** @brief 保存网关级连接参数，尚不启动 WebSocket。 */
WebRtcCloudSignalingClient::WebRtcCloudSignalingClient(const WebRtcCloudConnectionConfig &config)
    : config_(config), started_(false), running_(false),
      connectionState_(ConnectionState::Idle), connectionAttemptId_(0)
{
}

/** @brief 确保共享连接先于本对象销毁。 */
WebRtcCloudSignalingClient::~WebRtcCloudSignalingClient()
{
    stop();
}

/**
 * @brief 启动前登记一个云端码流。
 *
 * 所有码流必须在 start 之前登记；连接打开时一次性发送完整 streams 数组。
 */
bool WebRtcCloudSignalingClient::addStream(
    const std::string &streamId,
    const std::shared_ptr<WebRtcCloudStreamBinding> &binding)
{
    if (!binding || streamId.empty()) {
        LOG_ERROR("[WEBRTC][CLOUD] add stream failed: missing binding or stream_id");
        return false;
    }
    {
        std::lock_guard<std::mutex> bindingLock(binding->mutex);
        if (!binding->openSession || !binding->deliverMessage || !binding->closeSession) {
            LOG_ERROR("[WEBRTC][CLOUD] add stream failed: output binding unavailable stream=%s",
                      streamId.c_str());
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_ || streamBindings_.count(streamId)) {
            LOG_ERROR("[WEBRTC][CLOUD] add stream failed: already started or duplicate stream=%s",
                      streamId.c_str());
            return false;
        }
        streamBindings_[streamId] = binding;
    }
    LOG_INFO("[WEBRTC][CLOUD] stream registered locally: %s", streamId.c_str());
    return true;
}

/**
 * @brief 所有码流登记完成后，启动唯一的云端连接 worker。
 *
 * 启动前必须至少登记一路输出，避免建立一个无法承载任何会话的设备长连接。
 */
bool WebRtcCloudSignalingClient::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::weak_ptr<WebRtcCloudStreamBinding>>::const_iterator stream =
        streamBindings_.begin();

    if (started_) 
    {
        LOG_INFO("[WEBRTC][CLOUD] start ignored: already started");
        return true;
    }

    if (streamBindings_.empty()) 
    {
        LOG_ERROR("[WEBRTC][CLOUD] start failed: no streams registered");
        return false;
    }

    if (config_.url.empty() || config_.deviceId.empty() || config_.token.empty()) {
        LOG_ERROR("[WEBRTC][CLOUD] start failed: URL, device ID or token missing");
        return false;
    }

    for (; stream != streamBindings_.end(); ++stream) {
        if (stream->second.expired()) {
            LOG_ERROR("[WEBRTC][CLOUD] start failed: output binding expired stream=%s",
                      stream->first.c_str());
            return false;
        }
    }

    /* worker 只在全部码流登记完成后启动，因此 register 不需要因新码流而重连。 */
    running_ = true;
    connectionState_ = ConnectionState::Idle;
    try {
        worker_ = std::thread(&WebRtcCloudSignalingClient::workerMain, this);
    } catch (const std::exception &e) {
        running_ = false;
        LOG_ERROR("[WEBRTC][CLOUD] start worker failed: %s", e.what());
        return false;
    }
    started_ = true;
    return true;
}

/**
 * @brief 停止设备连接和重连线程，并使已有云端会话失效。
 *
 * 连接关闭可能同步触发 WebSocket 回调，因此不能持有模块锁调用 close 或 join。
 */
void WebRtcCloudSignalingClient::stop()
{
    std::shared_ptr<communication::WebSocketConnection> connection;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ && !worker_.joinable()) return;
        running_ = false;
        connectionState_ = ConnectionState::Disconnected;
        connection = connection_;
        condition_.notify_all();
    }
    if (connection) connection->close();
    if (worker_.joinable()) worker_.join();

    /* worker 退出后不再产生新会话；再清理输出中残留的旧会话。 */
    handleDisconnected();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_.reset();
        connectionState_ = ConnectionState::Idle;
        routes_.clear();
        started_ = false;
    }
}

/** @brief 取消一个输出的登记和路由；共享连接的停止由网关统一调用 stop。 */
void WebRtcCloudSignalingClient::removeStream(const std::string &streamId)
{
    std::shared_ptr<WebRtcCloudStreamBinding> binding;
    std::vector<std::string> sessionIds;
    size_t index = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, std::weak_ptr<WebRtcCloudStreamBinding>>::iterator stream =
            streamBindings_.find(streamId);
        std::map<std::string, WebRtcCloudSessionRoute>::iterator route = routes_.begin();

        if (stream == streamBindings_.end()) return;
        binding = stream->second.lock();
        streamBindings_.erase(stream);
        for (route = routes_.begin(); route != routes_.end();) {
            if (route->second.streamId == streamId) {
                sessionIds.push_back(route->first);
                route = routes_.erase(route);
            } else {
                ++route;
            }
        }
    }
    if (binding) {
        std::function<void(const std::string &)> closeCallback;
        {
            std::lock_guard<std::mutex> bindingLock(binding->mutex);
            closeCallback = binding->closeSession;
            binding->openSession = std::function<bool(const std::string &)>();
            binding->deliverMessage =
                std::function<void(const std::string &, const std::string &)>();
            binding->closeSession = std::function<void(const std::string &)>();
        }
        for (index = 0; closeCallback && index < sessionIds.size(); ++index) {
            closeCallback(sessionIds[index]);
        }
    }
}

/** @brief 查询共享设备连接是否已经完成云端登记。 */
bool WebRtcCloudSignalingClient::isReady() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connectionState_ == ConnectionState::Ready &&
           connection_ && connection_->isOpen();
}

/** @brief 发送属于已打开会话的 SDP 或 candidate。 */
bool WebRtcCloudSignalingClient::sendSessionMessage(const std::string &sessionId,
                                              const std::string &message)
{
    std::shared_ptr<communication::WebSocketConnection> connection;
    std::string envelope;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connectionState_ != ConnectionState::Ready || !routes_.count(sessionId)) {
            LOG_ERROR("[WEBRTC][CLOUD] send failed: session not registered id=%s", sessionId.c_str());
            return false;
        }
        connection = connection_;
    }
    if (!cloud_signaling_wrap_session_message(sessionId, message, envelope)) {
        LOG_ERROR("[WEBRTC][CLOUD] send failed: invalid session message id=%s",
                  sessionId.c_str());
        return false;
    }
    if (!connection || !connection->isOpen() || envelope.size() > 262144) {
        LOG_ERROR("[WEBRTC][CLOUD] send failed: connection closed or message too large id=%s", sessionId.c_str());
        return false;
    }
    return connection->sendText(envelope);
}

/** @brief 主动关闭一个云端会话；其他浏览会话和共享设备连接不受影响。 */
void WebRtcCloudSignalingClient::closeSession(const std::string &sessionId, const char *reason)
{
    size_t erasedRoutes = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        erasedRoutes = routes_.erase(sessionId);
    }
    /* 通道可能因回调失败已先关闭；只有实际撤销路由的一方负责发送 close。 */
    if (erasedRoutes == 0) return;
    sendCloseMessage(sessionId, reason);
}

void WebRtcCloudSignalingClient::sendCloseMessage(const std::string &sessionId,
                                                  const char *reason)
{
    std::string message;
    std::shared_ptr<communication::WebSocketConnection> connection;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connectionState_ == ConnectionState::Ready) connection = connection_;
    }
    if (!connection || !connection->isOpen()) return;
    message = cloud_signaling_make_close(
        sessionId,
        reason ? reason : "client_closed");
    if (!connection->sendText(message)) {
        LOG_ERROR("[WEBRTC][CLOUD] close send failed session=%s", sessionId.c_str());
    }
}

/** @brief 连接断开后清理旧 session_id；旧会话不能跨重连继续使用。 */
void WebRtcCloudSignalingClient::handleDisconnected()
{
    std::vector<std::pair<std::string, std::shared_ptr<WebRtcCloudStreamBinding>>> sessions;
    std::map<std::string, WebRtcCloudSessionRoute>::iterator route;
    size_t index = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connectionState_ = ConnectionState::Disconnected;
        for (route = routes_.begin(); route != routes_.end(); ++route) {
            std::shared_ptr<WebRtcCloudStreamBinding> binding = route->second.binding.lock();
            if (binding) {
                sessions.push_back(std::make_pair(route->first, binding));
            }
        }
        routes_.clear();
    }
    /* 不持有模块锁关闭媒体会话，避免会话析构路径再次进入共享客户端形成死锁。 */
    for (index = 0; index < sessions.size(); ++index) {
        std::function<void(const std::string &)> closeCallback;
        {
            std::lock_guard<std::mutex> bindingLock(sessions[index].second->mutex);
            closeCallback = sessions[index].second->closeSession;
        }
        if (closeCallback) {
            closeCallback(sessions[index].first);
        }
    }
}

/** @brief 调用方持锁时判断事件是否属于当前仍在运行的连接轮次。 */
bool WebRtcCloudSignalingClient::isCurrentAttemptLocked(uint64_t attemptId) const
{
    return running_ && attemptId != 0 && attemptId == connectionAttemptId_;
}

/** @brief 初始化新连接轮次，使用轮次编号隔离旧连接的迟到回调。 */
uint64_t WebRtcCloudSignalingClient::beginConnectionAttempt()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_) return 0;
    ++connectionAttemptId_;
    if (connectionAttemptId_ == 0) ++connectionAttemptId_;
    connection_.reset();
    connectionState_ = ConnectionState::Connecting;
    return connectionAttemptId_;
}

/**
 * @brief 幂等发布本轮连接对象。
 *
 * connect() 返回和 onOpen 回调来自不同执行时序，二者都可能先到达。本函数把
 * 两处赋值收敛成同一个状态操作：首次调用保存连接，后续同对象调用直接成功。
 */
bool WebRtcCloudSignalingClient::publishConnection(
    uint64_t attemptId,
    const std::shared_ptr<communication::WebSocketConnection> &connection)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!connection || !isCurrentAttemptLocked(attemptId) || connectionState_ == ConnectionState::Idle || connectionState_ == ConnectionState::Disconnected) 
    {
        LOG_WARN("""[WEBRTC][CLOUD] publish connection failed: invalid attempt=%llu state=%d",
                 static_cast<unsigned long long>(attemptId),
                 static_cast<int>(connectionState_));
        return false;
    }
    if (connection_ && connection_ != connection) {
        LOG_ERROR("[WEBRTC][CLOUD] publish connection failed: attempt=%llu"
                  " connection mismatch",
                  static_cast<unsigned long long>(attemptId));
        return false;
    }
    connection_ = connection;
    return true;
}

/** @brief WebSocket 打开后发布连接并发送本轮唯一的设备登记消息。 */
void WebRtcCloudSignalingClient::handleConnectionOpened(
    uint64_t attemptId,
    const std::shared_ptr<communication::WebSocketConnection> &connection)
{
    std::vector<std::string> streamIds;
    std::map<std::string, std::weak_ptr<WebRtcCloudStreamBinding>>::iterator item =
        std::map<std::string, std::weak_ptr<WebRtcCloudStreamBinding>>::iterator();
    std::string registration;

    /* onOpen 可能早于 connect() 返回，连接对象统一通过幂等入口发布。 */
    if (!publishConnection(attemptId, connection)) {
        LOG_WARN("[WEBRTC][CLOUD] onOpen ignored: invalid attempt=%llu",
                 static_cast<unsigned long long>(attemptId));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentAttemptLocked(attemptId) || connectionState_ != ConnectionState::Connecting || connection_ != connection) {
            LOG_WARN("[WEBRTC][CLOUD] onOpen ignored: stale attempt=%llu",
                     static_cast<unsigned long long>(attemptId));
            return;
        }

        /* 本轮只允许从 Connecting 进入 Registering，避免重复发送 register。 */
        connectionState_ = ConnectionState::Registering;
        for (item = streamBindings_.begin(); item != streamBindings_.end(); ++item) {
            streamIds.push_back(item->first);
        }
        /* 构建注册消息 */
        registration = cloud_signaling_make_registration(
            config_.deviceId,
            config_.token,
            streamIds);
    }

    /* 网络发送不能持有模块锁，避免底层同步回调反向进入本对象形成死锁。 */
    if (!connection->sendText(registration)) {
        std::lock_guard<std::mutex> lock(mutex_);

        LOG_ERROR("[WEBRTC][CLOUD] register send failed attempt=%llu",
                  static_cast<unsigned long long>(attemptId));
        if (isCurrentAttemptLocked(attemptId) && connection_ == connection) {
            connectionState_ = ConnectionState::Disconnected;
            condition_.notify_all();
        }
    }
}

/** @brief 当前轮次连接关闭时结束登记或运行等待。 */
void WebRtcCloudSignalingClient::handleConnectionClosed(
    uint64_t attemptId,
    const std::shared_ptr<communication::WebSocketConnection> &connection)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isCurrentAttemptLocked(attemptId) || connectionState_ == ConnectionState::Idle || (connection_ && connection_ != connection)) 
    {
        LOG_ERROR("[WEBRTC][CLOUD] onClosed ignored: stale attempt=%llu", static_cast<unsigned long long>(attemptId));
        return;
    }
    connectionState_ = ConnectionState::Disconnected;
    condition_.notify_all(); /* 通知主线程连接断开 */
}

/** @brief 记录当前轮次错误；onClose 或 worker 的 isOpen 检查负责推进断线状态。 */
void WebRtcCloudSignalingClient::handleConnectionError(
    uint64_t attemptId,
    const std::shared_ptr<communication::WebSocketConnection> &connection,
    const std::string &error)
{
    bool currentAttempt = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentAttempt = isCurrentAttemptLocked(attemptId) &&
                         connectionState_ != ConnectionState::Idle &&
                         (!connection_ || connection_ == connection);
    }
    if (!currentAttempt) return;
    LOG_ERROR("[WEBRTC][CLOUD] WebSocket error attempt=%llu: %s",
              static_cast<unsigned long long>(attemptId),
              error.c_str());
}

/** @brief 拒绝旧轮次消息后，把当前连接消息交给统一云端协议处理。 */
void WebRtcCloudSignalingClient::handleConnectionTextMessage(
    uint64_t attemptId,
    const std::shared_ptr<communication::WebSocketConnection> &connection,
    const std::string &message)
{
    bool accepted = false;
    /* 判断是否接受该消息 */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepted = isCurrentAttemptLocked(attemptId) && connection_ == connection &&
                   (connectionState_ == ConnectionState::Registering || connectionState_ == ConnectionState::Ready);
    }
    if (!accepted) 
    {
        LOG_WARN("[WEBRTC][CLOUD] onMessage ignored: stale attempt=%llu",
                 static_cast<unsigned long long>(attemptId));
        return;
    }

    handleMessage(connection, message);
}

/** @brief 等待本轮登记成功、失败或被 stop() 中断。 */
WebRtcCloudSignalingClient::RegistrationWaitResult
WebRtcCloudSignalingClient::waitForRegistration(uint64_t attemptId)
{
    std::unique_lock<std::mutex> lock(mutex_);
    bool stateChanged = false;

    /* 超时返回false；等待条件变为true，则返回true */
    stateChanged = condition_.wait_for(
        lock,
        std::chrono::seconds(15),
        [this, attemptId]() {
            return !running_ || attemptId != connectionAttemptId_ ||
                   connectionState_ == ConnectionState::Ready ||
                   connectionState_ == ConnectionState::Disconnected;
        });

    if (!running_ || attemptId != connectionAttemptId_) 
    {
        return RegistrationWaitResult::StopRequested;
    }

    if (!stateChanged)  /* 等待超时分支 */
    {
        connectionState_ = ConnectionState::Disconnected;
        LOG_ERROR("[WEBRTC][CLOUD] register timeout attempt=%llu",
                  static_cast<unsigned long long>(attemptId));
        return RegistrationWaitResult::Failed;
    }

    return connectionState_ == ConnectionState::Ready ? RegistrationWaitResult::Ready : RegistrationWaitResult::Failed;
}

/** @brief 已登记阶段等待断线，并每五秒补充检查底层连接状态。 */
WebRtcCloudSignalingClient::ConnectionAttemptResult
WebRtcCloudSignalingClient::waitUntilDisconnected(uint64_t attemptId)
{
    std::unique_lock<std::mutex> lock(mutex_);

    while (running_ && attemptId == connectionAttemptId_ && connectionState_ == ConnectionState::Ready) 
    {
        condition_.wait_for(lock, std::chrono::seconds(5));
        if (connectionState_ == ConnectionState::Ready && connection_ && !connection_->isOpen()) 
        {
            connectionState_ = ConnectionState::Disconnected;
            LOG_WARN("[WEBRTC][CLOUD] connection closed unexpectedly attempt=%llu",
                     static_cast<unsigned long long>(attemptId));
        }
    }
    if (!running_ || attemptId != connectionAttemptId_) 
    {
        return ConnectionAttemptResult::StopRequested;
    }
    return ConnectionAttemptResult::DisconnectedAfterReady;
}

/** @brief 执行一次完整的连接、登记和在线保持过程。 */
WebRtcCloudSignalingClient::ConnectionAttemptResult
WebRtcCloudSignalingClient::runConnectionAttempt(
    communication::WebSocketClient &webSocketClient,
    uint64_t attemptId)
{
    communication::WebSocketClientConfig clientConfig;
    communication::WebSocketClientCallbacks callbacks;
    std::shared_ptr<communication::WebSocketConnection> connection;
    RegistrationWaitResult registrationResult = RegistrationWaitResult::Failed;
    ConnectionAttemptResult attemptResult = ConnectionAttemptResult::FailedBeforeReady;
    bool stillRunning = false;

    clientConfig.url = config_.url;
    clientConfig.connectionTimeoutMs = 10000;
    clientConfig.pingIntervalMs = 20000;
    clientConfig.verifyTlsPeer = true;

    /* 回调只转入具名处理函数，连接状态机不再散落在匿名函数中。 */
    callbacks.onOpen = [this, attemptId](
        const std::shared_ptr<communication::WebSocketConnection> &current) {
        handleConnectionOpened(attemptId, current);
    };
    callbacks.onTextMessage = [this, attemptId](
        const std::shared_ptr<communication::WebSocketConnection> &current,
        const std::string &message) {
        handleConnectionTextMessage(attemptId, current, message);
    };
    callbacks.onClose = [this, attemptId](
        const std::shared_ptr<communication::WebSocketConnection> &current) {
        handleConnectionClosed(attemptId, current);
    };
    callbacks.onError = [this, attemptId](
        const std::shared_ptr<communication::WebSocketConnection> &current,
        const std::string &error) {
        handleConnectionError(attemptId, current, error);
    };

    /* WebSocketClient 只执行一次异步建连，重连仍由外层 worker 统一控制。 */
    connection = webSocketClient.connect(clientConfig, callbacks);
    if (!connection || !publishConnection(attemptId, connection)) {
        if (connection) connection->close();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stillRunning = isCurrentAttemptLocked(attemptId);
            if (stillRunning) connectionState_ = ConnectionState::Disconnected;
        }
        return stillRunning ? ConnectionAttemptResult::FailedBeforeReady
                            : ConnectionAttemptResult::StopRequested;
    }
    /* 等待收到 */
    registrationResult = waitForRegistration(attemptId);
    /* 如果收到了Ready，则在这里一直等待到断开 */
    if (registrationResult == RegistrationWaitResult::Ready) {
        attemptResult = waitUntilDisconnected(attemptId);
    } else if (registrationResult == RegistrationWaitResult::StopRequested) {
        attemptResult = ConnectionAttemptResult::StopRequested;
    }

    /* close 可能同步触发回调，必须在未持有 mutex_ 时调用。 */
    connection->close();
    return attemptResult;
}

/** @brief 清除本轮连接状态，使迟到回调在退避阶段被忽略。 */
void WebRtcCloudSignalingClient::finishConnectionAttempt(uint64_t attemptId)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (attemptId != connectionAttemptId_) return;
    connection_.reset();
    connectionState_ = ConnectionState::Idle;
}

/** @brief 等待重连时间；stop() 会通过 condition_ 立即中断等待。 */
bool WebRtcCloudSignalingClient::waitBeforeReconnect(int retrySeconds)
{
    std::unique_lock<std::mutex> lock(mutex_);

    condition_.wait_for(
        lock,
        std::chrono::seconds(retrySeconds),
        [this]() { return !running_; });
    return running_;
}

/**
 * @brief 建立云端连接、完成 register，并在断线后指数退避重试。
 *
 * 主循环只编排单轮连接、旧会话清理和重连节奏；具体连接状态变化由具名函数维护。
 */
void WebRtcCloudSignalingClient::workerMain()
{
    communication::WebSocketClient webSocketClient;
    ConnectionAttemptResult attemptResult = ConnectionAttemptResult::FailedBeforeReady;
    uint64_t attemptId = 0;
    int retrySeconds = 1;

    while (true) {
        attemptId = beginConnectionAttempt();
        if (attemptId == 0) 
        {
            break;
        }

        /* 一轮只负责建连、登记和在线等待；返回后统一清理旧会话。 */
        attemptResult = runConnectionAttempt(webSocketClient, attemptId);
        handleDisconnected();
        finishConnectionAttempt(attemptId);

        if (attemptResult == ConnectionAttemptResult::StopRequested) 
        {
            LOG_WARN("[WEBRTC][CLOUD] stop requested, exit worker");
            break;
        }

        if (attemptResult == ConnectionAttemptResult::DisconnectedAfterReady) {
            /* 曾经稳定进入 Ready，下一次断线恢复重新从一秒开始。 */
            retrySeconds = 1;
        }

        LOG_WARN("[WEBRTC][CLOUD] disconnected, reconnect in %d s", retrySeconds);
        if (!waitBeforeReconnect(retrySeconds)) 
        {
            break;
        }

        retrySeconds = std::min(retrySeconds * 2, 30);
    }
}

/**
 * @brief 解析云端消息，并根据消息类型分派给对应处理函数。
 * @param[in] source 收到消息的 WebSocket 连接对象。
 * @param[in] message 收到的完整 JSON 文本消息。
 * @note 本函数只负责协议入口和类型分派，不直接修改连接状态或会话路由。
 */
void WebRtcCloudSignalingClient::handleMessage(const std::shared_ptr<communication::WebSocketConnection> &source, const std::string &message)
{
    CloudSignalingMessage parsedMessage;
    std::string type;

    if (!cloud_signaling_parse_message(message, parsedMessage)) {
        LOG_ERROR("[WEBRTC][CLOUD] invalid server message bytes=%zu", message.size());
        return;
    }

    /* 控制消息和会话消息使用不同状态规则，入口处先按类型明确分流。 */
    type = parsedMessage.get("type");
    if (type == "registered") {
        handleRegisteredMessage(source, parsedMessage);
        return;
    }
    if (type == "error") {
        handleServerErrorMessage(source, parsedMessage);
        return;
    }
    if (type == "opened") {
        handleOpenedMessage(source, parsedMessage);
        return;
    }

    /* 其余消息必须属于已经建立的 session_id 路由。 */
    handleRoutedSessionMessage(source, parsedMessage, message);
}

/**
 * @brief 判断消息是否来自当前有效的共享连接。
 * @param[in] source 触发消息回调的 WebSocket 连接对象。
 * @retval true 模块正在运行，且 source 与当前连接对象相同。
 * @retval false 模块已停止或 source 属于已经失效的连接。
 * @pre 调用方必须已经持有 mutex_。
 */
bool WebRtcCloudSignalingClient::isCurrentConnectionLocked(
    const std::shared_ptr<communication::WebSocketConnection> &source) const
{
    return running_ && connection_ == source;
}

/**
 * @brief 校验设备登记确认，并将连接状态推进到 Ready。
 * @param[in] source 收到登记确认的 WebSocket 连接对象。
 * @param[in] message 已通过基础格式校验的登记确认消息。
 */
void WebRtcCloudSignalingClient::handleRegisteredMessage(const std::shared_ptr<communication::WebSocketConnection> &source,
    const CloudSignalingMessage &message)
{
    std::string registeredDeviceId;

    registeredDeviceId = message.get("device_id");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentConnectionLocked(source)) 
        {
            LOG_WARN("[WEBRTC][CLOUD] registered ignored: stale connection");
            return;
        }

        if (connectionState_ != ConnectionState::Registering) 
        {
            LOG_ERROR("[WEBRTC][CLOUD] unexpected registered message state=%d", static_cast<int>(connectionState_));
            return;
        }

        if (registeredDeviceId != config_.deviceId) {
            LOG_ERROR("[WEBRTC][CLOUD] registered device_id mismatch");
            connectionState_ = ConnectionState::Disconnected;
            condition_.notify_all();
            return;
        }

        /* 登记完成后唤醒 waitForRegistration()，开始承载会话消息。 */
        connectionState_ = ConnectionState::Ready;
        condition_.notify_all();
        LOG_INFO("[WEBRTC][CLOUD] device registered: %s streams=%zu",
                 config_.deviceId.c_str(), streamBindings_.size());
    }
}

/**
 * @brief 记录云端错误，并在登记失败时结束当前连接轮次。
 * @param[in] source 收到错误消息的 WebSocket 连接对象。
 * @param[in] message 已通过基础格式校验的错误消息。
 * @note Ready 状态下的会话错误只记录日志；登记阶段错误会唤醒 worker 重连。
 */
void WebRtcCloudSignalingClient::handleServerErrorMessage(const std::shared_ptr<communication::WebSocketConnection> &source,
    const CloudSignalingMessage &message)
{
    std::string errorCode;
    std::string sessionId;

    errorCode = message.get("code");
    sessionId = message.get("session_id");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentConnectionLocked(source)) {
            LOG_WARN("[WEBRTC][CLOUD] server error ignored: stale connection");
            return;
        }

        LOG_ERROR("[WEBRTC][CLOUD] server rejected message code=%s session=%s", errorCode.c_str(), sessionId.c_str());

        if (connectionState_ != ConnectionState::Ready) 
        {
            /* 注册失败时立即唤醒 worker 重连，不继续等待登记超时。 */
            connectionState_ = ConnectionState::Disconnected;
            condition_.notify_all();
        }
    }
}

/**
 * @brief 校验 opened 消息并为新的 session_id 建立媒体会话。
 * @param[in] source 收到 opened 消息的 WebSocket 连接对象。
 * @param[in] message 已通过基础格式校验的 opened 消息。
 * @note 创建会话期间不持有 mutex_；发布路由前会再次检查连接状态。
 */
void WebRtcCloudSignalingClient::handleOpenedMessage(const std::shared_ptr<communication::WebSocketConnection> &source,
    const CloudSignalingMessage &message)
{
    std::shared_ptr<WebRtcCloudStreamBinding> binding;
    std::function<bool(const std::string &)> openSessionCallback;
    WebRtcCloudSessionRoute route;
    std::map<std::string, std::weak_ptr<WebRtcCloudStreamBinding>>::iterator stream =
        std::map<std::string, std::weak_ptr<WebRtcCloudStreamBinding>>::iterator();
    std::string deviceId;
    std::string sessionId;
    std::string streamId;

    deviceId = message.get("device_id"); /* 设备id */
    sessionId = message.get("session_id"); /* 会话id，对应一个请求流的客户端 */
    streamId = message.get("stream_id"); /* 流id */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connectionState_ != ConnectionState::Ready) {
            LOG_ERROR("[WEBRTC][CLOUD] opened rejected before registration");
            return;
        }

        if (sessionId.empty()) {
            LOG_ERROR("[WEBRTC][CLOUD] opened rejected: missing session_id");
            return;
        }

        if (sessionId.size() > 128) {
            LOG_ERROR("[WEBRTC][CLOUD] opened rejected: session_id too long bytes=%zu",
                      sessionId.size());
            return;
        }

        /* 旧连接的迟到消息不能进入当前连接的会话路由表。 */
        if (!isCurrentConnectionLocked(source)) {
            LOG_WARN("[WEBRTC][CLOUD] opened ignored: stale connection session=%s",
                     sessionId.c_str());
            return;
        }

        if (deviceId != config_.deviceId) {
            LOG_ERROR("[WEBRTC][CLOUD] opened rejected: device_id mismatch session=%s",
                      sessionId.c_str());
            return;
        }

        stream = streamBindings_.find(streamId);
        if (stream == streamBindings_.end()) {
            LOG_ERROR("[WEBRTC][CLOUD] opened rejected: unknown stream session=%s stream=%s",
                      sessionId.c_str(), streamId.c_str());
            return;
        }

        if (routes_.count(sessionId) != 0) {
            LOG_ERROR("[WEBRTC][CLOUD] opened rejected: duplicate session=%s stream=%s",
                      sessionId.c_str(), streamId.c_str());
            return;
        }

        binding = stream->second.lock();
    }
    /* 如果找不到指定的流id，则关闭会话 */
    if (!binding) {
        LOG_ERROR("[WEBRTC][CLOUD] stream binding unavailable session=%s stream=%s",
                  sessionId.c_str(),
                  streamId.c_str());
        sendCloseMessage(sessionId, "stream_unavailable");
        return;
    }
    {
        std::lock_guard<std::mutex> bindingLock(binding->mutex);
        openSessionCallback = binding->openSession;
    }
    if (!openSessionCallback) {
        LOG_ERROR("[WEBRTC][CLOUD] session open failed: stream stopped session=%s stream=%s",
                  sessionId.c_str(),
                  streamId.c_str());
        sendCloseMessage(sessionId, "stream_unavailable");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        /* 创建期间连接可能断开，因此发布路由前必须再次核对连接和 session_id。 */
        if (!isCurrentConnectionLocked(source) ||
            connectionState_ != ConnectionState::Ready || routes_.count(sessionId)) {
            LOG_WARN("[WEBRTC][CLOUD] opened ignored after connection state changed session=%s",
                     sessionId.c_str());
            return;
        }
        route.streamId = streamId;
        route.binding = binding;
        routes_[sessionId] = route;
    }
    /* 根据 stream_id 找到媒体端点，并直接以云端 session_id 创建浏览器会话。 */
    if (!openSessionCallback(sessionId)) {
        LOG_ERROR("[WEBRTC][CLOUD] media session create failed session=%s stream=%s",
                  sessionId.c_str(),
                  streamId.c_str());
        closeSession(sessionId, "resource_limit");
    }
}

/**
 * @brief 查找已有 session_id，并在锁外交付媒体协商或关闭消息。
 * @param[in] source 收到消息的 WebSocket 连接对象。
 * @param[in] message 已通过基础格式校验的消息字段。
 * @param[in] rawMessage 需要原样交付给会话通道的完整 JSON 文本。
 */
void WebRtcCloudSignalingClient::handleRoutedSessionMessage(
    const std::shared_ptr<communication::WebSocketConnection> &source,
    const CloudSignalingMessage &message,
    const std::string &rawMessage)
{
    std::shared_ptr<WebRtcCloudStreamBinding> binding;
    std::map<std::string, WebRtcCloudSessionRoute>::iterator found =
        std::map<std::string, WebRtcCloudSessionRoute>::iterator();
    std::function<void(const std::string &, const std::string &)> deliverCallback;
    std::function<void(const std::string &)> closeCallback;
    std::string type;
    std::string sessionId;

    type = message.get("type");
    sessionId = message.get("session_id");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentConnectionLocked(source) ||
            connectionState_ != ConnectionState::Ready ||
            sessionId.empty() || sessionId.size() > 128) {
            LOG_ERROR("[WEBRTC][CLOUD] session message before registration or invalid session_id type=%s",
                      type.c_str());
            return;
        }

        found = routes_.find(sessionId);
        if (found == routes_.end()) {
            LOG_ERROR("[WEBRTC][CLOUD] unknown session=%s type=%s",
                      sessionId.c_str(), type.c_str());
            return;
        }
        binding = found->second.binding.lock();
        if (type == "close") routes_.erase(found);
    }

    if (!binding) {
        LOG_ERROR("[WEBRTC][CLOUD] session output unavailable session=%s",
                  sessionId.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> bindingLock(binding->mutex);
        deliverCallback = binding->deliverMessage;
        closeCallback = binding->closeSession;
    }
    if (type == "offer" || type == "candidate") {
        if (!deliverCallback) {
            LOG_ERROR("[WEBRTC][CLOUD] session message callback unavailable session=%s",
                      sessionId.c_str());
            return;
        }
        deliverCallback(sessionId, rawMessage);
        return;
    }
    if (type == "close") {
        /* 先撤销路由再通知媒体会话，避免关闭过程向云端重复发送 close。 */
        if (closeCallback) {
            closeCallback(sessionId);
        }
        return;
    }

    LOG_ERROR("[WEBRTC][CLOUD] unsupported server message type=%s", type.c_str());
}

} // namespace webrtc
} // namespace rkmedia
