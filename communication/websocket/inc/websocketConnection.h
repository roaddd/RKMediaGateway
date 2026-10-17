/**
 * @file websocketConnection.h
 * @brief 定义与连接建立方式无关的 WebSocket 双向连接接口。
 *
 * WebSocketConnection 只表示一条已经建立或正在建立的连接，统一封装文本收发、
 * 连接状态和生命周期回调。当前由 WebSocketClient 创建主动连接；调用方不需要
 * 感知底层 libdatachannel 类型。
 */
#ifndef __WEBSOCKET_CONNECTION_H__
#define __WEBSOCKET_CONNECTION_H__

#include <functional>
#include <memory>
#include <string>

namespace rkmedia {
namespace communication {

class WebSocketConnectionFactory;

typedef std::function<void()> WebSocketOpenCallback;
typedef std::function<void()> WebSocketCloseCallback;
typedef std::function<void(const std::string &)> WebSocketErrorCallback;
typedef std::function<void(const std::string &)> WebSocketTextMessageCallback;

/**
 * @brief 一条通用 WebSocket 双向连接。
 *
 * 本类不解析 SDP、ICE 或任何业务协议。底层 WebSocket 实现通过 PImpl 隐藏，
 * 从而避免 communication 模块的调用方依赖 libdatachannel 头文件。
 */
class WebSocketConnection {
public:
    ~WebSocketConnection();

    WebSocketConnection(const WebSocketConnection &) = delete;
    WebSocketConnection &operator=(const WebSocketConnection &) = delete;

    /** @brief 查询底层 WebSocket 是否已经完成握手并处于可发送状态。 */
    bool isOpen() const;

    /**
     * @brief 发送一条 WebSocket 文本消息。
     * @param[in] message 待发送的完整文本消息。
     * @return 消息成功提交到底层连接时返回 true，否则返回 false。
     */
    bool sendText(const std::string &message);

    /** @brief 主动关闭连接；允许重复调用。 */
    void close();
    /** @brief 返回服务端接受连接时携带的请求路径，主动连接可能返回空字符串。 */
    std::string path() const;
    /** @brief 返回对端地址的可读文本，仅用于日志和调试。 */
    std::string remoteAddress() const;

    /** @brief 注册连接打开回调；后一次注册替换前一次注册。 */
    void registerOpenCallback(const WebSocketOpenCallback &callback);
    /** @brief 注册连接关闭回调；后一次注册替换前一次注册。 */
    void registerCloseCallback(const WebSocketCloseCallback &callback);
    /** @brief 注册连接错误回调；后一次注册替换前一次注册。 */
    void registerErrorCallback(const WebSocketErrorCallback &callback);
    /** @brief 注册文本消息回调；二进制消息不由该回调处理。 */
    void registerTextMessageCallback(const WebSocketTextMessageCallback &callback);

private:
    class Impl;

    /** @brief 仅供模块内部工厂使用，外部不能直接注入底层连接对象。 */
    explicit WebSocketConnection(const std::shared_ptr<Impl> &impl);

    friend class WebSocketConnectionFactory;

    std::shared_ptr<Impl> impl_; /* 隐藏底层库类型和连接操作实现。 */
};

} // namespace communication
} // namespace rkmedia

#endif
