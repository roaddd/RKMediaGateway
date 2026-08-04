#include "../inc/webrtcServer.h"

#include <exception>
#include <rtc/global.hpp>
#include <vector>

#include "logger.h"

namespace rkmedia {
namespace webrtc {

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
    size_t i;
    int sentCount;

    sentCount = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            LOG_WARN("[WEBRTC] send video ignored: server not running");
            return 0;
        }
        for (iter = sessions_.begin(); iter != sessions_.end(); ++iter) {
            sessions.push_back(iter->second);
        }
    }

    if (sessions.empty()) {
        LOG_DEBUG("[WEBRTC] send video ignored: no browser session");
        return 0;
    }

    for (i = 0; i < sessions.size(); ++i) {
        if (sessions[i]->sendVideoFrame(frame)) {
            ++sentCount;
        }
    }

    return sentCount;
}

/* 返回当前 WebRTC 会话数量。 */
/*
 * 广播一帧 G711 音频到所有已就绪的浏览器会话。
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
        for (iter = sessions_.begin(); iter != sessions_.end(); ++iter) {
            sessions.push_back(iter->second);
        }
    }

    if (sessions.empty()) {
        LOG_DEBUG("[WEBRTC] send audio ignored: no browser session");
        return 0;
    }

    for (i = 0; i < sessions.size(); ++i) {
        if (sessions[i]->sendAudioFrame(frame)) {
            ++sentCount;
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
            });
        sessions_[id] = session;
    }

    LOG_INFO("[WEBRTC] new browser session=%d remote=%s path=%s",
             id,
             connection->remoteAddress().c_str(),
             connection->path().c_str());
    session->start();
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
