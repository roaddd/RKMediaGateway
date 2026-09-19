#ifndef __WEBSOCKET_CONNECTION_H__
#define __WEBSOCKET_CONNECTION_H__

#include <functional>
#include <memory>
#include <string>

namespace rtc {
class WebSocket;
}

namespace rkmedia {
namespace communication {

typedef std::function<void()> WebSocketOpenCallback;
typedef std::function<void()> WebSocketCloseCallback;
typedef std::function<void(const std::string &)> WebSocketErrorCallback;
typedef std::function<void(const std::string &)> WebSocketTextMessageCallback;

/*
 * WebSocket 单连接封装。
 * 该类只提供文本收发和连接状态回调，不理解 SDP、ICE 或 WebRTC 业务含义。
 */
class WebSocketConnection {
public:
    explicit WebSocketConnection(const std::shared_ptr<rtc::WebSocket> &connection);
    ~WebSocketConnection();

    bool isOpen() const;
    void sendText(const std::string &message);
    void close();

    std::string path() const;
    std::string remoteAddress() const;

    void registerOpenCallback(const WebSocketOpenCallback &callback);
    void registerCloseCallback(const WebSocketCloseCallback &callback);
    void registerErrorCallback(const WebSocketErrorCallback &callback);
    void registerTextMessageCallback(const WebSocketTextMessageCallback &callback);

private:
    std::shared_ptr<rtc::WebSocket> connection_; /* libdatachannel 的单条 WebSocket 连接。 */
};

} // namespace communication
} // namespace rkmedia

#endif
