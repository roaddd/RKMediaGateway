#include "websocketServer.h"

#include <exception>
#include <rtc/websocket.hpp>
#include <rtc/websocketserver.hpp>

#include "logger.h"

namespace rkmedia {
namespace communication {

WebSocketConnection::WebSocketConnection(const std::shared_ptr<void> &nativeConnection)
{
    nativeConnection_ = nativeConnection;
}

WebSocketConnection::~WebSocketConnection()
{
}

/* 返回底层 WebSocket 是否仍处于 open 状态。 */
bool WebSocketConnection::isOpen() const
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = std::static_pointer_cast<rtc::WebSocket>(nativeConnection_);
    return ws && ws->isOpen();
}

/* 发送 WebSocket 文本消息，信令层用于传输 SDP/ICE JSON。 */
void WebSocketConnection::sendText(const std::string &message)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = std::static_pointer_cast<rtc::WebSocket>(nativeConnection_);
    if (ws && ws->isOpen()) {
        ws->send(message);
    } else {
        LOG_WARN("[WEBSOCKET] send text ignored: connection not open size=%zu", message.size());
    }
}

/* 主动关闭当前 WebSocket 连接。 */
void WebSocketConnection::close()
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = std::static_pointer_cast<rtc::WebSocket>(nativeConnection_);
    if (ws) {
        ws->close();
    }
}

/* 返回浏览器连接时请求的 WebSocket path，例如 /browser。 */
std::string WebSocketConnection::path() const
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = std::static_pointer_cast<rtc::WebSocket>(nativeConnection_);
    if (!ws || !ws->path()) {
        return "";
    }
    return *ws->path();
}

/* 返回浏览器端地址，仅用于日志和调试。 */
std::string WebSocketConnection::remoteAddress() const
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = std::static_pointer_cast<rtc::WebSocket>(nativeConnection_);
    if (!ws || !ws->remoteAddress()) {
        return "";
    }
    return *ws->remoteAddress();
}

/* 注册连接打开回调，底层 WebSocket 握手完成后触发。 */
void WebSocketConnection::onOpen(const WebSocketOpenCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = std::static_pointer_cast<rtc::WebSocket>(nativeConnection_);
    if (ws) {
        ws->onOpen(callback);
    } else {
        LOG_WARN("[WEBSOCKET] onOpen ignored: native connection is NULL");
    }
}

/* 注册连接关闭回调，浏览器断开或服务端 close 时触发。 */
void WebSocketConnection::onClosed(const WebSocketCloseCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = std::static_pointer_cast<rtc::WebSocket>(nativeConnection_);
    if (ws) {
        ws->onClosed(callback);
    } else {
        LOG_WARN("[WEBSOCKET] onClosed ignored: native connection is NULL");
    }
}

/* 注册连接错误回调，底层 WebSocket 出错时触发。 */
void WebSocketConnection::onError(const WebSocketErrorCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = std::static_pointer_cast<rtc::WebSocket>(nativeConnection_);
    if (ws) {
        ws->onError(callback);
    } else {
        LOG_WARN("[WEBSOCKET] onError ignored: native connection is NULL");
    }
}

/* 注册文本消息回调，业务层在这里接收 SDP/ICE JSON。 */
void WebSocketConnection::onText(const WebSocketTextCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = std::static_pointer_cast<rtc::WebSocket>(nativeConnection_);
    if (ws) {
        ws->onMessage(nullptr, callback);
    } else {
        LOG_WARN("[WEBSOCKET] onText ignored: native connection is NULL");
    }
}

WebSocketServer::WebSocketServer()
{
}

WebSocketServer::~WebSocketServer()
{
    stop();
}

/* 启动 WebSocket 监听，仅负责接收连接，不解析业务消息。 */
bool WebSocketServer::start(const std::string &bindAddress, uint16_t port)
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
    server->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
        std::shared_ptr<WebSocketConnection> connection;

        try {
            connection = std::make_shared<WebSocketConnection>(std::static_pointer_cast<void>(ws));
        } catch (const std::exception &e) {
            LOG_ERROR("[WEBSOCKET] client create failed: %s", e.what());
            return;
        }
        if (clientCallback_) {
            clientCallback_(connection);
        } else {
            LOG_WARN("[WEBSOCKET] client ignored: no onClient callback registered");
        }
    });

    nativeServer_ = std::static_pointer_cast<void>(server);
    return true;
}

/* 停止监听，新连接不再进入，已有连接由上层会话清理。 */
void WebSocketServer::stop()
{
    std::shared_ptr<rtc::WebSocketServer> server;

    server = std::static_pointer_cast<rtc::WebSocketServer>(nativeServer_);
    if (server) {
        server->stop();
    }
    nativeServer_.reset();
}

/* 返回实际监听端口，port=0 自动分配时可用于查询。 */
uint16_t WebSocketServer::port() const
{
    std::shared_ptr<rtc::WebSocketServer> server;

    server = std::static_pointer_cast<rtc::WebSocketServer>(nativeServer_);
    if (!server) {
        return 0;
    }
    return server->port();
}

/* 注册新客户端连接回调。 */
void WebSocketServer::onClient(const WebSocketClientCallback &callback)
{
    clientCallback_ = callback;
}

} // namespace communication
} // namespace rkmedia
