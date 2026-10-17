/**
 * @file websocketConnection.cpp
 * @brief 实现通用 WebSocket 双向连接及底层对象工厂。
 */
#include "websocketConnection.h"

#include "websocketConnectionInternal.h"

#include <exception>
#include <rtc/websocket.hpp>

#include "logger.h"

namespace rkmedia {
namespace communication {

/** @brief 保存 libdatachannel 连接对象，避免其类型泄漏到公共头文件。 */
class WebSocketConnection::Impl {
public:
    explicit Impl(const std::shared_ptr<rtc::WebSocket> &nativeConnection)
        : nativeConnection_(nativeConnection)
    {
    }

    std::shared_ptr<rtc::WebSocket> nativeConnection_; /* 实际执行收发的底层连接。 */
};

WebSocketConnection::WebSocketConnection(const std::shared_ptr<Impl> &impl)
    : impl_(impl)
{
}

WebSocketConnection::~WebSocketConnection()
{
}

/**
 * @brief 把模块内部创建或接受的底层连接包装为通用连接。
 * @param[in] nativeConnection 底层 WebSocket 连接。
 * @return 成功返回连接对象，入参为空或分配失败时返回空指针。
 */
std::shared_ptr<WebSocketConnection> WebSocketConnectionFactory::create(
    const std::shared_ptr<rtc::WebSocket> &nativeConnection)
{
    std::shared_ptr<WebSocketConnection::Impl> impl;
    std::shared_ptr<WebSocketConnection> connection;

    if (!nativeConnection) {
        LOG_ERROR("[WEBSOCKET] create connection failed: native connection is NULL");
        return nullptr;
    }

    try {
        impl = std::make_shared<WebSocketConnection::Impl>(nativeConnection);
        connection = std::shared_ptr<WebSocketConnection>(new WebSocketConnection(impl));
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBSOCKET] create connection failed: %s", e.what());
        return nullptr;
    }
    return connection;
}

bool WebSocketConnection::isOpen() const
{
    std::shared_ptr<rtc::WebSocket> nativeConnection;

    nativeConnection = impl_ ? impl_->nativeConnection_ : nullptr;
    return nativeConnection && nativeConnection->isOpen();
}

bool WebSocketConnection::sendText(const std::string &message)
{
    std::shared_ptr<rtc::WebSocket> nativeConnection;
    bool submitted = false;

    nativeConnection = impl_ ? impl_->nativeConnection_ : nullptr;
    if (!nativeConnection || !nativeConnection->isOpen()) {
        LOG_ERROR("[WEBSOCKET] send text failed: connection not open size=%zu", message.size());
        return false;
    }

    try {
        submitted = nativeConnection->send(message);
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBSOCKET] send text failed: size=%zu error=%s", message.size(), e.what());
        return false;
    }
    if (!submitted) {
        LOG_ERROR("[WEBSOCKET] send text failed: send buffer full size=%zu", message.size());
        return false;
    }
    return true;
}

void WebSocketConnection::close()
{
    std::shared_ptr<rtc::WebSocket> nativeConnection;

    nativeConnection = impl_ ? impl_->nativeConnection_ : nullptr;
    if (!nativeConnection) {
        return;
    }

    try {
        nativeConnection->close();
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBSOCKET] close failed: %s", e.what());
    }
}

std::string WebSocketConnection::path() const
{
    std::shared_ptr<rtc::WebSocket> nativeConnection;

    nativeConnection = impl_ ? impl_->nativeConnection_ : nullptr;
    if (!nativeConnection || !nativeConnection->path()) {
        return "";
    }
    return *nativeConnection->path();
}

std::string WebSocketConnection::remoteAddress() const
{
    std::shared_ptr<rtc::WebSocket> nativeConnection;

    nativeConnection = impl_ ? impl_->nativeConnection_ : nullptr;
    if (!nativeConnection || !nativeConnection->remoteAddress()) {
        return "";
    }
    return *nativeConnection->remoteAddress();
}

void WebSocketConnection::registerOpenCallback(const WebSocketOpenCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> nativeConnection;

    nativeConnection = impl_ ? impl_->nativeConnection_ : nullptr;
    if (!nativeConnection) {
        LOG_ERROR("[WEBSOCKET] register open callback failed: native connection is NULL");
        return;
    }
    nativeConnection->onOpen(callback);
}

void WebSocketConnection::registerCloseCallback(const WebSocketCloseCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> nativeConnection;

    nativeConnection = impl_ ? impl_->nativeConnection_ : nullptr;
    if (!nativeConnection) {
        LOG_ERROR("[WEBSOCKET] register close callback failed: native connection is NULL");
        return;
    }
    nativeConnection->onClosed(callback);
}

void WebSocketConnection::registerErrorCallback(const WebSocketErrorCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> nativeConnection;

    nativeConnection = impl_ ? impl_->nativeConnection_ : nullptr;
    if (!nativeConnection) {
        LOG_ERROR("[WEBSOCKET] register error callback failed: native connection is NULL");
        return;
    }
    nativeConnection->onError(callback);
}

void WebSocketConnection::registerTextMessageCallback(
    const WebSocketTextMessageCallback &callback)
{
    std::shared_ptr<rtc::WebSocket> nativeConnection;

    nativeConnection = impl_ ? impl_->nativeConnection_ : nullptr;
    if (!nativeConnection) {
        LOG_ERROR("[WEBSOCKET] register text callback failed: native connection is NULL");
        return;
    }
    nativeConnection->onMessage(nullptr, callback);
}

} // namespace communication
} // namespace rkmedia
