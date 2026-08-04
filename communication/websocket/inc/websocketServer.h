#ifndef __WEBSOCKET_SERVER_H__
#define __WEBSOCKET_SERVER_H__

#include <functional>
#include <memory>
#include <string>

namespace rkmedia {
namespace communication {

class WebSocketConnection;

typedef std::function<void(const std::shared_ptr<WebSocketConnection> &)> WebSocketClientCallback;
typedef std::function<void()> WebSocketOpenCallback;
typedef std::function<void()> WebSocketCloseCallback;
typedef std::function<void(const std::string &)> WebSocketErrorCallback;
typedef std::function<void(const std::string &)> WebSocketTextCallback;

/*
 * WebSocket 单连接封装。
 * 该类只提供文本收发和连接状态回调，不理解 SDP、ICE 或 WebRTC 业务含义。
 */
class WebSocketConnection {
public:
    explicit WebSocketConnection(const std::shared_ptr<void> &nativeConnection);
    ~WebSocketConnection();

    bool isOpen() const;
    void sendText(const std::string &message);
    void close();

    std::string path() const;
    std::string remoteAddress() const;

    void onOpen(const WebSocketOpenCallback &callback);
    void onClosed(const WebSocketCloseCallback &callback);
    void onError(const WebSocketErrorCallback &callback);
    void onText(const WebSocketTextCallback &callback);

private:
    std::shared_ptr<void> nativeConnection_;
};

/*
 * WebSocket 服务封装。
 * 上层通过 onClient 注册新连接回调，具体消息语义由业务层处理。
 */
class WebSocketServer {
public:
    WebSocketServer();
    ~WebSocketServer();

    bool start(const std::string &bindAddress, uint16_t port);
    void stop();
    uint16_t port() const;

    void onClient(const WebSocketClientCallback &callback);

private:
    std::shared_ptr<void> nativeServer_;
    WebSocketClientCallback clientCallback_;
};

} // namespace communication
} // namespace rkmedia

#endif
