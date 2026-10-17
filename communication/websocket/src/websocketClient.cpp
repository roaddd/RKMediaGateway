/**
 * @file websocketClient.cpp
 * @brief 实现通用 WebSocket 主动连接器。
 */
#include "websocketClient.h"

#include "websocketConnectionInternal.h"

#include <chrono>
#include <exception>
#include <rtc/websocket.hpp>

#include "logger.h"

namespace rkmedia {
namespace communication {

WebSocketClient::WebSocketClient()
{
}

WebSocketClient::~WebSocketClient()
{
}

std::shared_ptr<WebSocketConnection> WebSocketClient::connect(
    const WebSocketClientConfig &config,
    const WebSocketClientCallbacks &callbacks) const
{
    rtc::WebSocket::Configuration nativeConfig;
    std::shared_ptr<rtc::WebSocket> nativeConnection;
    std::shared_ptr<WebSocketConnection> connection;
    std::weak_ptr<WebSocketConnection> weakConnection;

    if ((config.url.compare(0, 5, "ws://") != 0 &&
         config.url.compare(0, 6, "wss://") != 0) ||
        config.connectionTimeoutMs == 0 || config.pingIntervalMs == 0) {
        LOG_ERROR("[WEBSOCKET] client connect failed: invalid config url=%s timeout=%u ping=%u",
                  config.url.c_str(),
                  config.connectionTimeoutMs,
                  config.pingIntervalMs);
        return nullptr;
    }

    nativeConfig.disableTlsVerification = !config.verifyTlsPeer;
    nativeConfig.connectionTimeout = std::chrono::milliseconds(config.connectionTimeoutMs);
    nativeConfig.pingInterval = std::chrono::milliseconds(config.pingIntervalMs);

    try {
        nativeConnection = std::make_shared<rtc::WebSocket>(nativeConfig);
        connection = WebSocketConnectionFactory::create(nativeConnection);
        if (!connection) {
            LOG_ERROR("[WEBSOCKET] client connect failed: connection wrapper create failed");
            return nullptr;
        }

        weakConnection = connection;
        /*
         * 底层 WebSocket 持有这些回调，而回调又需要访问上层连接包装对象。
         * 这里捕获 weak_ptr，既能在事件到达时取得有效连接，又不会形成
         * WebSocketConnection 与底层 WebSocket 之间的循环引用。
         *
         * 必须先注册全部回调再调用 open，防止快速失败或快速建连事件丢失。
         */

        /*
         * onOpen：异步建链全部完成后触发。对于 ws，表示 TCP 连接和 HTTP
         * Upgrade 握手成功；对于 wss，还要求 TLS 握手成功。只有进入该回调后，
         * 连接才可以正常调用 sendText() 发送信令消息。
         */
        connection->registerOpenCallback([weakConnection, callbacks]() {
            std::shared_ptr<WebSocketConnection> current = weakConnection.lock();
            if (current && callbacks.onOpen) callbacks.onOpen(current);
        });

        /*
         * onClose：连接进入关闭状态后触发，既可能由对端关闭引起，也可能由本地
         * close() 或传输异常后的底层清理引起。上层通常在这里清除当前连接，并由
         * 自己的重连策略决定是否重新调用 connect()。
         */
        connection->registerCloseCallback([weakConnection, callbacks]() {
            std::shared_ptr<WebSocketConnection> current = weakConnection.lock();
            if (current && callbacks.onClose) callbacks.onClose(current);
        });

        /*
         * onError：建链阶段或已连接阶段发生 DNS、TCP、TLS、HTTP Upgrade、
         * WebSocket 协议或底层收发错误时触发。错误可能发生在 onOpen 之前；
         * onError 用于记录原因，后续连接释放仍以 onClose 或上层主动清理为准。
         */
        connection->registerErrorCallback([weakConnection, callbacks](const std::string &error) {
            std::shared_ptr<WebSocketConnection> current = weakConnection.lock();
            if (current && callbacks.onError) callbacks.onError(current, error);
        });

        /*
         * onTextMessage：收到一条完整的 WebSocket 文本消息后触发。底层已经完成
         * WebSocket 分帧重组，此处拿到的是完整字符串。当前客户端只承载 JSON
         * 信令，因此没有注册二进制消息回调，二进制消息不会进入该回调。
         */
        connection->registerTextMessageCallback(
            [weakConnection, callbacks](const std::string &message) {
                std::shared_ptr<WebSocketConnection> current = weakConnection.lock();
                if (current && callbacks.onTextMessage) {
                    callbacks.onTextMessage(current, message);
                }
            });

        /*
         * 根据 ws:// 或 wss:// URL 异步发起连接：解析 URL，建立 TCP 连接，
         * wss 模式下继续完成 TLS 握手，最后发送 HTTP Upgrade 请求并切换为
         * WebSocket 协议。open() 返回只表示连接流程已经启动，不表示已经连接
         * 成功；最终结果分别通过 onOpen、onError 和 onClose 回调通知。
         */
        nativeConnection->open(config.url);
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBSOCKET] client connect failed: url=%s error=%s",
                  config.url.c_str(),
                  e.what());
        return nullptr;
    }
    return connection;
}

} // namespace communication
} // namespace rkmedia
