/**
 * @file websocketConfig.h
 * @brief 定义通用 WebSocket 客户端传输配置。
 *
 * 配置只描述 WebSocket 连接本身，不包含设备标识、鉴权 Token、SDP、ICE 或其它
 * 业务字段，确保 communication 模块可以被不同上层协议复用。
 */
#ifndef __WEBSOCKET_CONFIG_H__
#define __WEBSOCKET_CONFIG_H__

#include <cstdint>
#include <string>

namespace rkmedia {
namespace communication {

/** @brief WebSocket 客户端连接参数。 */
struct WebSocketClientConfig {
    std::string url; /* 完整的 ws:// 或 wss:// 地址。 */
    uint32_t connectionTimeoutMs = 10000; /* 建立 TCP/TLS/WebSocket 连接的超时时间。 */
    uint32_t pingIntervalMs = 20000; /* WebSocket 保活 ping 的发送间隔。 */
    bool verifyTlsPeer = true; /* wss:// 是否校验服务端证书和主机名。 */
};

} // namespace communication
} // namespace rkmedia

#endif
