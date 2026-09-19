#include "websocketConnection.h"

#include <rtc/websocket.hpp>

#include "logger.h"

namespace rkmedia {
namespace communication {

WebSocketConnection::WebSocketConnection(const std::shared_ptr<rtc::WebSocket> &connection)
{
    connection_ = connection;
}

WebSocketConnection::~WebSocketConnection()
{
}

/* 返回底层 WebSocket 是否仍处于 open 状态。 */
bool WebSocketConnection::isOpen() const
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = connection_;
    return ws && ws->isOpen();
}

/* 发送 WebSocket 文本消息，信令层用于传输 SDP/ICE JSON。 */
void WebSocketConnection::sendText(const std::string &message)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = connection_;
    if (ws && ws->isOpen()) {
        ws->send(message);
    } else {
        LOG_ERROR("[WEBSOCKET] send text ignored: connection not open size=%zu", message.size());
    }
}

/* 主动关闭当前 WebSocket 连接。 */
void WebSocketConnection::close()
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = connection_;
    if (ws) {
        ws->close();
    }
}

/* 返回浏览器连接时请求的 WebSocket path，例如 /browser。 */
std::string WebSocketConnection::path() const
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = connection_;
    if (!ws || !ws->path()) {
        return "";
    }
    return *ws->path();
}

/* 返回浏览器端地址，仅用于日志和调试。 */
std::string WebSocketConnection::remoteAddress() const
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = connection_;
    if (!ws || !ws->remoteAddress()) {
        return "";
    }
    return *ws->remoteAddress();
}

/* 注册连接打开回调，底层 WebSocket 握手完成后触发。 */
void WebSocketConnection::registerOpenCallback(const WebSocketOpenCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = connection_;
    if (ws) {
        ws->onOpen(callback);
    } else {
        LOG_WARN("[WEBSOCKET] register open callback ignored: native connection is NULL");
    }
}

/* 注册连接关闭回调，浏览器断开或服务端 close 时触发。 */
void WebSocketConnection::registerCloseCallback(const WebSocketCloseCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = connection_;
    if (ws) {
        ws->onClosed(callback);
    } else {
        LOG_WARN("[WEBSOCKET] register close callback ignored: native connection is NULL");
    }
}

/* 注册连接错误回调，底层 WebSocket 出错时触发。 */
void WebSocketConnection::registerErrorCallback(const WebSocketErrorCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = connection_;
    if (ws) {
        ws->onError(callback);
    } else {
        LOG_WARN("[WEBSOCKET] register error callback ignored: native connection is NULL");
    }
}

/* 注册文本消息回调，业务层在这里接收 SDP/ICE JSON。 */
void WebSocketConnection::registerTextMessageCallback(
    const WebSocketTextMessageCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> ws;

    ws = connection_;
    if (ws) {
        ws->onMessage(nullptr, callback);
    } else {
        LOG_WARN("[WEBSOCKET] register text message callback ignored: native connection is NULL");
    }
}

} // namespace communication
} // namespace rkmedia
