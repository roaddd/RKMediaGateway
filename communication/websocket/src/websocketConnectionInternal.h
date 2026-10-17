/**
 * @file websocketConnectionInternal.h
 * @brief 声明 WebSocket 模块内部使用的底层连接包装工厂。
 *
 * 本文件只能由 communication/websocket 的实现文件包含，用于在不向公共头文件暴露
 * libdatachannel 类型的前提下，让客户端和服务端创建统一的 WebSocketConnection。
 */
#ifndef __WEBSOCKET_CONNECTION_INTERNAL_H__
#define __WEBSOCKET_CONNECTION_INTERNAL_H__

#include <memory>

namespace rtc {
class WebSocket;
}

namespace rkmedia {
namespace communication {

class WebSocketConnection;

class WebSocketConnectionFactory {
public:
    /** @brief 把底层连接包装为 communication 模块的通用连接对象。 */
    static std::shared_ptr<WebSocketConnection> create(
        const std::shared_ptr<rtc::WebSocket> &nativeConnection);
};

} // namespace communication
} // namespace rkmedia

#endif
