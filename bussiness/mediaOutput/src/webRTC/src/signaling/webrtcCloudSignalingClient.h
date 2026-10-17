/**
 * @file webrtcCloudSignalingClient.h
 * @brief 定义网关级云端信令客户端、输出流绑定和会话路由。
 *
 * WebRtcCloudSignalingClient 使用一条设备 WebSocket 长连接承载全部输出流和浏览器
 * 会话，负责注册、重连和 session_id 路由。每路输出通过 WebRtcCloudStreamBinding
 * 登记回调；本客户端不管理 PeerConnection 或媒体 Track。
 */
#ifndef __WEBRTC_CLOUD_SIGNALING_CLIENT_H__
#define __WEBRTC_CLOUD_SIGNALING_CLIENT_H__

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rkmedia {
namespace communication {
class WebSocketClient;
class WebSocketConnection;
}
namespace webrtc {

struct CloudSignalingMessage;

/** @brief 网关持有的云端连接参数；Token 只用于传输，禁止写入日志。 */
struct WebRtcCloudConnectionConfig {
    std::string url; /* 云端 /device WebSocket 地址。 */
    std::string deviceId; /* 云端登记使用的稳定设备标识。 */
    std::string token; /* 从私有文件读取的设备凭据。 */
};

/** @brief 单路输出与其会话创建回调之间的生命周期绑定。 */
struct WebRtcCloudStreamBinding {
    std::mutex mutex; /* 串行化消息分发和输出停止时的回调清理。 */
    std::function<bool(const std::string &)> openSession; /* 按云端 session_id 创建媒体会话。 */
    std::function<void(const std::string &, const std::string &)> deliverMessage; /* 交付会话信令。 */
    std::function<void(const std::string &)> closeSession; /* 关闭远端会话。 */
};

/** @brief 一个云端 session_id 对应的输出流路由。 */
struct WebRtcCloudSessionRoute {
    std::string streamId; /* 该会话绑定的输出流标识。 */
    std::weak_ptr<WebRtcCloudStreamBinding> binding; /* 向对应媒体端点交付信令的弱引用。 */
};

/**
 * @brief 网关级云端信令客户端。
 *
 * 所有云端输出先调用 addStream()，随后网关调用 start() 建立唯一设备连接。模块
 * 内部完成断线重连、设备重新登记，以及云端 session_id 到逻辑通道的路由。
 */
class WebRtcCloudSignalingClient
    : public std::enable_shared_from_this<WebRtcCloudSignalingClient> {
public:
    explicit WebRtcCloudSignalingClient(const WebRtcCloudConnectionConfig &config);
    ~WebRtcCloudSignalingClient();
    WebRtcCloudSignalingClient(const WebRtcCloudSignalingClient &) = delete;
    WebRtcCloudSignalingClient &operator=(const WebRtcCloudSignalingClient &) = delete;

    /** @brief 启动前登记一路输出；相同 stream_id 或启动后登记均失败。 */
    bool addStream(const std::string &streamId,
                   const std::shared_ptr<WebRtcCloudStreamBinding> &binding);
    /** @brief 在至少登记一路输出后建立共享连接和重连 worker。 */
    bool start();
    /** @brief 停止 worker、关闭连接并使全部云端会话通道失效。 */
    void stop();
    /** @brief 撤销一路输出及其关联会话路由。 */
    void removeStream(const std::string &streamId);
    /** @brief 为会话消息添加 session_id 后通过共享连接发送。 */
    bool sendSessionMessage(const std::string &sessionId, const std::string &message);
    /** @brief 主动关闭一个云端会话，不影响共享连接和其它会话。 */
    void closeSession(const std::string &sessionId, const char *reason);
    /** @brief 查询共享连接是否打开且已经完成设备登记。 */
    bool isReady() const;

private:
    /** @brief 一轮云端连接在 worker 中所处的明确阶段。 */
    enum class ConnectionState {
        Idle,         /* 当前没有正在执行的连接轮次。 */
        Connecting,   /* 已开始建连，尚未收到 WebSocket onOpen。 */
        Registering,  /* WebSocket 已打开，正在等待云端确认 register。 */
        Ready,        /* 云端已确认设备和码流登记，可以承载会话信令。 */
        Disconnected  /* 本轮建连、登记或运行期间已经失败或断开。 */
    };

    /** @brief 单轮连接结束原因，用于决定退出还是按何种节奏重连。 */
    enum class ConnectionAttemptResult {
        StopRequested,         /* stop() 已请求 worker 退出。 */
        FailedBeforeReady,     /* 建连或登记阶段失败，尚未进入 Ready。 */
        DisconnectedAfterReady /* 已进入 Ready，随后连接断开。 */
    };

    /** @brief 等待云端登记确认的结果。 */
    enum class RegistrationWaitResult {
        Ready,         /* 收到合法 registered 消息。 */
        Failed,        /* 断线、登记被拒绝或等待超时。 */
        StopRequested  /* stop() 中断了等待。 */
    };

    /**
     * @brief 云端信令工作线程入口。
     *
     * 循环执行连接、设备登记、在线等待和指数退避重连，直到 stop() 请求退出。
     */
    void workerMain();
    /**
     * @brief 初始化一个新的连接轮次。
     * @return 返回非零的连接轮次编号；模块停止运行时返回 0。
     */
    uint64_t beginConnectionAttempt();
    /**
     * @brief 执行一次建连、登记和在线保持过程。
     * @param[in,out] webSocketClient 通用 WebSocket 主动连接器。
     * @param[in] attemptId 当前连接轮次编号。
     * @return 返回本轮停止、登记前失败或登记后断线的结束原因。
     */
    ConnectionAttemptResult runConnectionAttempt(
        communication::WebSocketClient &webSocketClient,
        uint64_t attemptId);
    /**
     * @brief 幂等发布当前轮次创建的连接对象。
     * @param[in] attemptId 产生该连接的轮次编号。
     * @param[in] connection 待发布的 WebSocket 连接对象。
     * @retval true 连接属于当前轮次，且已经成功发布或此前已发布同一对象。
     * @retval false 连接为空、轮次失效、状态无效或本轮已发布其他连接对象。
     */
    bool publishConnection(
        uint64_t attemptId,
        const std::shared_ptr<communication::WebSocketConnection> &connection);
    /**
     * @brief 处理 WebSocket 打开事件并发送设备登记消息。
     * @param[in] attemptId 触发事件的连接轮次编号。
     * @param[in] connection 触发打开事件的连接对象。
     */
    void handleConnectionOpened(
        uint64_t attemptId,
        const std::shared_ptr<communication::WebSocketConnection> &connection);
    /**
     * @brief 处理 WebSocket 关闭事件并唤醒等待中的工作线程。
     * @param[in] attemptId 触发事件的连接轮次编号。
     * @param[in] connection 触发关闭事件的连接对象。
     */
    void handleConnectionClosed(
        uint64_t attemptId,
        const std::shared_ptr<communication::WebSocketConnection> &connection);
    /**
     * @brief 记录当前轮次发生的 WebSocket 错误。
     * @param[in] attemptId 触发事件的连接轮次编号。
     * @param[in] connection 发生错误的连接对象。
     * @param[in] error 底层 WebSocket 返回的错误说明。
     */
    void handleConnectionError(
        uint64_t attemptId,
        const std::shared_ptr<communication::WebSocketConnection> &connection,
        const std::string &error);
    /**
     * @brief 校验文本消息的连接归属并交给云端协议入口处理。
     * @param[in] attemptId 触发事件的连接轮次编号。
     * @param[in] connection 收到消息的连接对象。
     * @param[in] message 收到的完整 WebSocket 文本消息。
     */
    void handleConnectionTextMessage(
        uint64_t attemptId,
        const std::shared_ptr<communication::WebSocketConnection> &connection,
        const std::string &message);
    /**
     * @brief 等待当前轮次的设备登记结果。
     * @param[in] attemptId 当前连接轮次编号。
     * @return 返回登记成功、登记失败或停止请求。
     */
    RegistrationWaitResult waitForRegistration(uint64_t attemptId);
    /**
     * @brief 登记成功后等待连接断开或模块停止。
     * @param[in] attemptId 当前连接轮次编号。
     * @return 返回登记后断线或停止请求。
     */
    ConnectionAttemptResult waitUntilDisconnected(uint64_t attemptId);
    /**
     * @brief 清理指定连接轮次发布的连接和临时状态。
     * @param[in] attemptId 待结束的连接轮次编号。
     */
    void finishConnectionAttempt(uint64_t attemptId);
    /**
     * @brief 等待下一次重连，并允许 stop() 提前中断等待。
     * @param[in] retrySeconds 本次退避等待秒数。
     * @retval true 等待结束后模块仍应继续运行。
     * @retval false stop() 已请求退出。
     */
    bool waitBeforeReconnect(int retrySeconds);
    /**
     * @brief 判断回调是否属于当前有效连接轮次。
     * @param[in] attemptId 回调携带的连接轮次编号。
     * @retval true 模块正在运行，且编号与当前轮次一致。
     * @retval false 模块已停止、编号为零或编号已经过期。
     * @pre 调用方必须已经持有 mutex_。
     */
    bool isCurrentAttemptLocked(uint64_t attemptId) const;
    /**
     * @brief 解析云端文本消息并按照 type 分发处理。
     * @param[in] source 收到消息的 WebSocket 连接对象。
     * @param[in] message 收到的完整 JSON 文本消息。
     */
    void handleMessage(const std::shared_ptr<communication::WebSocketConnection> &source,
                       const std::string &message);
    /**
     * @brief 处理设备登记确认，并唤醒等待登记结果的工作线程。
     * @param[in] source 收到登记确认的 WebSocket 连接对象。
     * @param[in] message 已通过基础格式校验的登记确认消息。
     */
    void handleRegisteredMessage(
        const std::shared_ptr<communication::WebSocketConnection> &source,
        const CloudSignalingMessage &message);
    /**
     * @brief 处理云端错误消息。
     * @param[in] source 收到错误消息的 WebSocket 连接对象。
     * @param[in] message 已通过基础格式校验的错误消息。
     */
    void handleServerErrorMessage(
        const std::shared_ptr<communication::WebSocketConnection> &source,
        const CloudSignalingMessage &message);
    /**
     * @brief 校验 opened 路由并创建、发布对应的媒体会话。
     * @param[in] source 收到 opened 消息的 WebSocket 连接对象。
     * @param[in] message 已通过基础格式校验的 opened 消息。
     */
    void handleOpenedMessage(
        const std::shared_ptr<communication::WebSocketConnection> &source,
        const CloudSignalingMessage &message);
    /**
     * @brief 将已有会话的 offer、candidate 或 close 交给所属媒体端点。
     * @param[in] source 收到消息的 WebSocket 连接对象。
     * @param[in] message 已通过基础格式校验的消息字段。
     * @param[in] rawMessage 需要原样交付给会话通道的完整 JSON 文本。
     */
    void handleRoutedSessionMessage(
        const std::shared_ptr<communication::WebSocketConnection> &source,
        const CloudSignalingMessage &message,
        const std::string &rawMessage);
    /**
     * @brief 判断消息是否来自当前有效的共享连接。
     * @param[in] source 触发回调的 WebSocket 连接对象。
     * @retval true 模块正在运行，且 source 与当前连接对象相同。
     * @retval false 模块已停止或消息来自已经失效的连接。
     * @pre 调用方必须已经持有 mutex_。
     */
    bool isCurrentConnectionLocked(
        const std::shared_ptr<communication::WebSocketConnection> &source) const;
    /**
     * @brief 清空旧 session_id 路由，并通知关联媒体端点远端已关闭。
     */
    void handleDisconnected();
    /**
     * @brief 发送不修改本地路由表的会话关闭消息。
     * @param[in] sessionId 待关闭的云端会话标识。
     * @param[in] reason 关闭原因；允许传入空指针，此时使用 client_closed。
     */
    void sendCloseMessage(const std::string &sessionId, const char *reason);

    /* 构造后保持不变的设备连接配置。 */
    WebRtcCloudConnectionConfig config_;

    /* 连接 worker 的同步对象与生命周期状态。 */
    mutable std::mutex mutex_; /* 保护连接、流表、路由表和线程状态。 */
    std::condition_variable condition_; /* 唤醒重连 worker 或结束等待。 */
    std::thread worker_; /* 建连、登记和退避重连线程。 */
    bool started_; /* start() 是否已经成功调用。 */
    bool running_; /* worker 是否应继续运行。 */

    /* 当前共享设备连接及云端登记状态。 */
    std::shared_ptr<communication::WebSocketConnection> connection_;
    ConnectionState connectionState_; /* 当前连接轮次所处的唯一状态。 */
    uint64_t connectionAttemptId_; /* 当前有效连接轮次编号，用于拒绝迟到回调。 */

    /* 输出流绑定表，stream_id -> WebRtcCloudStreamBinding。 */
    std::map<std::string, std::weak_ptr<WebRtcCloudStreamBinding>> streamBindings_;
    /* 单会话路由表 */
    std::map<std::string, WebRtcCloudSessionRoute> routes_;
};

} // namespace webrtc
} // namespace rkmedia

#endif
