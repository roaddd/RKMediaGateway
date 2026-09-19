#ifndef __WEBSOCKET_LISTENER_H__
#define __WEBSOCKET_LISTENER_H__

#include <functional>
#include <memory>
#include <stdint.h>
#include <string>

namespace rtc {
class WebSocketServer;
}

namespace rkmedia {
namespace communication {

class WebSocketConnection;

typedef std::function<void(const std::shared_ptr<WebSocketConnection> &)> WebSocketClientCallback;

/*
 * WebSocket 监听封装。
 * 负责接收新连接；上层注册回调后获取连接，具体消息语义由业务层处理。
 * 与底层 libdatachannel 的 rtc::WebSocketServer 区分。
 */
class WebSocketListener {
public:
    WebSocketListener();
    ~WebSocketListener();

    bool start(const std::string &bindAddress, uint16_t port);
    void stop();
    uint16_t port() const;

    /* Register the new-client callback; a later registration replaces the previous one. */
    void registerClientCallback(const WebSocketClientCallback &callback);

private:
    std::shared_ptr<rtc::WebSocketServer> listener_; /* libdatachannel WebSocket 监听实例。 */
    WebSocketClientCallback clientCallback_;
};

} // namespace communication
} // namespace rkmedia

#endif
