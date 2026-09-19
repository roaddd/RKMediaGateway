#include "websocketListener.h"

#include "websocketConnection.h"

#include <exception>
#include <rtc/websocket.hpp>
#include <rtc/websocketserver.hpp>

#include "logger.h"

namespace rkmedia {
namespace communication {

WebSocketListener::WebSocketListener()
{
}

WebSocketListener::~WebSocketListener()
{
    stop();
}

/* 启动 WebSocket 监听，仅负责接收连接，不解析业务消息。 */
bool WebSocketListener::start(const std::string &bindAddress, uint16_t port)
{
    rtc::WebSocketServer::Configuration config;
    std::shared_ptr<rtc::WebSocketServer> server;

    config.bindAddress = bindAddress;
    config.port = port;
    try {
        server = std::make_shared<rtc::WebSocketServer>(config);
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBSOCKET] server start failed bind=%s port=%u error=%s",
                  bindAddress.c_str(),
                  port,
                  e.what());
        return false;
    }

    /* 向底层 WebSocketServer 注册新客户端连接回调。 */
    server->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
        std::shared_ptr<WebSocketConnection> connection;

        try {
            connection = std::make_shared<WebSocketConnection>(ws);
        } catch (const std::exception &e) {
            LOG_ERROR("[WEBSOCKET] client create failed: %s", e.what());
            return;
        }
        if (clientCallback_) {
            clientCallback_(connection);
        } else {
            LOG_WARN("[WEBSOCKET] client ignored: no client callback registered");
        }
    });

    listener_ = server;
    return true;
}

/* 停止监听，新连接不再进入，已有连接由上层会话清理。 */
void WebSocketListener::stop()
{
    std::shared_ptr<rtc::WebSocketServer> server;

    server = listener_;
    if (server) {
        server->stop();
    }
    listener_.reset();
}

/* 返回实际监听端口，port=0 自动分配时可用于查询。 */
uint16_t WebSocketListener::port() const
{
    std::shared_ptr<rtc::WebSocketServer> server;

    server = listener_;
    if (!server) {
        return 0;
    }
    return server->port();
}

/* 注册新客户端连接回调；重复注册时以最后一次提供的回调为准。 */
void WebSocketListener::registerClientCallback(const WebSocketClientCallback &callback)
{
    clientCallback_ = callback;
}

} // namespace communication
} // namespace rkmedia
