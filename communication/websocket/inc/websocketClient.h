/**
 * @file websocketClient.h
 * @brief 定义主动建立 WebSocket 连接的通用客户端。
 */
#ifndef __WEBSOCKET_CLIENT_H__
#define __WEBSOCKET_CLIENT_H__

#include "websocketConfig.h"
#include "websocketConnection.h"

#include <memory>

namespace rkmedia {
namespace communication {

/** @brief 主动连接建立前一次性注册的连接事件集合。 */
struct WebSocketClientCallbacks {
    std::function<void(const std::shared_ptr<WebSocketConnection> &)> onOpen; /* 握手成功。 */
    std::function<void(const std::shared_ptr<WebSocketConnection> &)> onClose; /* 连接关闭。 */
    std::function<void(const std::shared_ptr<WebSocketConnection> &,
                       const std::string &)> onError; /* 底层连接错误。 */
    std::function<void(const std::shared_ptr<WebSocketConnection> &,
                       const std::string &)> onTextMessage; /* 收到文本消息。 */
};

/**
 * @brief 通用 WebSocket 主动连接器。
 *
 * 本类只负责创建一次连接并提交握手。断线重连、鉴权和业务状态恢复必须由调用方
 * 决定，避免通信层绑定某一种业务恢复策略。
 */
class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient &) = delete;
    WebSocketClient &operator=(const WebSocketClient &) = delete;

    /**
     * @brief 创建连接、注册回调并发起 WebSocket 握手。
     * @param[in] config 连接地址、超时、保活和 TLS 校验参数。
     * @param[in] callbacks 必须在握手前生效的连接事件回调。
     * @return 成功提交连接请求时返回连接对象，参数非法或创建失败时返回空指针。
     */
    std::shared_ptr<WebSocketConnection> connect(
        const WebSocketClientConfig &config,
        const WebSocketClientCallbacks &callbacks) const;
};

} // namespace communication
} // namespace rkmedia

#endif
