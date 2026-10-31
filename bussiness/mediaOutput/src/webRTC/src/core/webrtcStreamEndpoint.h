/**
 * @file webrtcStreamEndpoint.h
 * @brief 定义单路 WebRTC 媒体端点及其内部运行状态。
 *
 * 本文件负责浏览器会话、PeerConnection、音视频 Track、媒体广播、上行音频
 * 汇总以及 PLI/FIR 关键帧调度。它不负责建立云端设备长连接；共享信令客户端
 * 按 stream_id 找到本端点，再按 session_id 创建和路由媒体会话。
 */

#ifndef __WEBRTC_STREAM_ENDPOINT_H__
#define __WEBRTC_STREAM_ENDPOINT_H__

#include "webrtcSession.h"
#include "webrtcVideoDiagnostics.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace rkmedia {
namespace webrtc {

/** @brief 单类媒体从 mediaOutput 进入端点的累计统计。 */
struct WebRtcMediaInputCounters {
    uint64_t frames = 0; /* 输入帧或编码包数量。 */
    uint64_t bytes = 0; /* 输入编码数据总字节数。 */
};

/** @brief 单类媒体向浏览器会话广播的累计结果。 */
struct WebRtcMediaBroadcastCounters {
    uint64_t targets = 0; /* 成功发送到的 session 总次数，一帧扇出给多个 session 会累计多次。 */
    uint64_t noReadySession = 0; /* 没有任何可发送 Track，或所有 Track 都未就绪的次数。 */
};

/** @brief 浏览器到设备方向的有效音频 RTP 负载统计。 */
struct WebRtcIncomingAudioCounters {
    uint64_t packets = 0; /* 所有 session 收到的有效音频 RTP 包数。 */
    uint64_t payloadBytes = 0; /* 去除 RTP 头后的音频编码负载总字节数。 */
};

/** @brief WebRtcStreamEndpoint 的媒体累计统计，仅在 mutex_ 保护下读写。 */
struct WebRtcStreamMediaCounters {
    WebRtcMediaInputCounters videoInput; /* 视频编码帧输入统计。 */
    WebRtcMediaInputCounters audioInput; /* 音频编码包输入统计。 */
    WebRtcMediaBroadcastCounters videoBroadcast; /* 视频广播结果。 */
    WebRtcMediaBroadcastCounters audioBroadcast; /* 音频广播结果。 */
    WebRtcIncomingAudioCounters incomingAudio; /* 浏览器上行音频统计。 */
};

/** @brief 尚未完成的 IDR 请求及其等待状态。 */
struct WebRtcKeyframePendingState {
    bool immediateRequest = false; /* 新会话或未限频 PLI 触发、等待上游立即消费的 IDR 请求。 */
    bool deferredPliRequest = false; /* 被限频合并、等待时间窗口结束的 PLI IDR 请求。 */
    bool waitingIdrInput = false; /* 请求已被上游消费，正在等待编码后的 IDR 输入端点。 */
};

/** @brief PLI、IDR 请求和 IDR 输入之间的时间点及阶段耗时。 */
struct WebRtcKeyframeTimingState {
    bool hasLastIdrRequestTime = false; /* lastIdrRequestTime 是否有效。 */
    bool hasLastPliTime = false; /* lastPliTime 是否有效。 */
    bool hasLastIdrInputTime = false; /* lastIdrInputTime 是否有效。 */
    bool hasPendingPliTriggerTime = false; /* pendingPliTriggerTime 是否有效。 */
    bool hasLastPliToIdrRequestDelay = false; /* lastPliToIdrRequestMs 是否有效。 */
    bool hasLastIdrRequestToInputDelay = false; /* lastIdrRequestToInputMs 是否有效。 */
    std::chrono::steady_clock::time_point lastIdrRequestTime; /* 最近一次请求 MPP 输出 IDR 的时刻。 */
    std::chrono::steady_clock::time_point lastPliTime; /* 最近一次收到 PLI/FIR 的时刻。 */
    std::chrono::steady_clock::time_point lastIdrInputTime; /* 最近一次编码 IDR 输入端点的时刻。 */
    std::chrono::steady_clock::time_point pendingPliTriggerTime; /* 当前请求关联的首个 PLI/FIR 时刻。 */
    std::chrono::steady_clock::time_point waitingIdrRequestTime; /* 当前等待编码 IDR 的请求时刻。 */
    uint64_t lastPliToIdrRequestMs = 0; /* 最近一次 PLI/FIR 到 IDR 请求的耗时。 */
    uint64_t lastIdrRequestToInputMs = 0; /* 最近一次 IDR 请求到编码 IDR 输入的耗时。 */
};

/** @brief IDR 调度与浏览器 PLI/FIR 的累计计数。 */
struct WebRtcKeyframeCounters {
    uint64_t requests = 0; /* 已进入立即请求队列的 IDR 请求次数。 */
    uint64_t pliReceived = 0; /* 所有浏览器 session 上报的 PLI/FIR 总次数。 */
    uint64_t pliAccepted = 0; /* 未命中限频、可立即请求 IDR 的 PLI/FIR 次数。 */
    uint64_t pliDeferred = 0; /* 被已有请求或限频窗口合并、延迟的 PLI/FIR 次数。 */
};

/*
 * WebRtcStreamEndpoint 的 IDR 请求合并和 PLI 限频状态。
 * 该结构不直接调用编码器，只保存等待 mediaGateway 消费的请求标记与统计。
 */
struct WebRtcKeyframeRequestState {
    WebRtcKeyframePendingState pending; /* 当前待处理与等待完成的请求。 */
    WebRtcKeyframeTimingState timing; /* 关键链路的时间点和耗时。 */
    WebRtcKeyframeCounters counters; /* IDR 与 PLI/FIR 累计次数。 */
};

/*
 * WebRTC 单路媒体端点，每路 WebRTC 输出一个。
 *
 * 本类只管理浏览器会话、PeerConnection、媒体广播和关键帧请求，不直接管理
 * WebSocket。共享云端客户端通过绑定回调调用 openSession() 进入本模块。
 * 
 * 职责：（1）媒体管线进入 WebRTC 的终点，从mediaOutput 的视角看，它就是 WebRTC 媒体输出的终点
 *           编码器 → mediaOutput → WebRtcStreamEndpoint → WebRtcSession；
 *      （2）云端浏览器会话进入媒体模块的统一入口，通过 openSession() 创建会话；
 *      （3）同时承担下行音视频的出口、浏览器会话的入口、浏览器上行音频的汇总入口、PLI/FIR 请求的汇总入口
 */
class WebRtcStreamEndpoint {
public:
    /** @brief 创建尚未启动的单路媒体端点。 */
    WebRtcStreamEndpoint();
    /** @brief 停止端点并关闭遗留会话。 */
    ~WebRtcStreamEndpoint();

    /** @brief 保存媒体配置并开放会话创建入口。 */
    bool start(const WebRtcStreamEndpointConfig &config);
    /** @brief 禁止新会话并关闭当前端点管理的全部会话。 */
    void stop();
    /** @brief 把一帧 H264 广播给所有已就绪的视频 Track。 */
    int sendVideoFrame(const WebRtcVideoFrame &frame);
    /** @brief 把一个编码音频包广播给所有已就绪的音频 Track。 */
    int sendAudioFrame(const WebRtcAudioFrame &frame);
    /** @brief 返回当前仍由端点管理的浏览器会话数量。 */
    int sessionCount() const;
    /** @brief 消费一次合并后的上游 IDR 请求。 */
    bool consumeVideoKeyframeRequest();
    /** @brief 设置指定会话的 PLI 测试视频抑制状态。 */
    bool setSessionPliTestVideoSuppression(const std::string &sessionId, bool enabled);
    /** @brief 生成媒体、关键帧和会话统计快照。 */
    void getStats(WebRtcStreamEndpointStats &stats) const;
    /**
     * @brief 为一个已经建立云端路由的 session_id 创建独立媒体会话。
     * @param[in] sessionId 云端分配的唯一会话标识。
     * @param[in] signalingSendCallback 向该 session_id 发送信令的回调。
     * @param[in] signalingCloseCallback 主动注销该 session_id 路由的回调。
     * @return 会话创建并成功绑定信令回调时返回 true，否则返回 false。
     */
    bool openSession(const std::string &sessionId,
                     const WebRtcSessionSignalingSendCallback &signalingSendCallback,
                     const WebRtcSessionSignalingCloseCallback &signalingCloseCallback);
    /** @brief 将云端已经按 session_id 路由的 Offer 或 candidate 交给会话。 */
    void handleSessionSignalingMessage(const std::string &sessionId,
                                       const std::string &message);
    /** @brief 关闭云端已经撤销路由的会话，不重复发送 close。 */
    void handleSessionSignalingClosed(const std::string &sessionId);

private:
    /** @brief 会话关闭后从内部会话表中删除对应对象。 */
    void removeSession(const std::string &sessionId);
    /** @brief 合并会话上报的首帧、PLI/FIR 或测试恢复关键帧请求。 */
    void requestVideoKeyframe(const std::string &sessionId,
                              WebRtcKeyframeRequestReason reason);
    /** @brief 汇总并向业务层交付浏览器上行音频包。 */
    void handleIncomingAudioPacket(const WebRtcIncomingAudioPacket &packet);
    /** @brief 记录一次 IDR 请求被上游编码流程消费的时刻和阶段耗时。 */
    void recordVideoIdrRequestConsumed(const std::chrono::steady_clock::time_point &now);

private:
    /* 同步与生命周期状态。 */
    mutable std::mutex mutex_; /* 保护运行状态、会话表、统计和关键帧调度状态。 */
    WebRtcStreamEndpointConfig config_; /* 启动时保存的媒体格式、ICE 和业务回调配置。 */
    bool running_; /* 媒体端点是否已经启动且尚未停止。 */

    /* 浏览器媒体会话。 */
    std::map<std::string, std::shared_ptr<WebRtcSession>> sessions_; /* 按云端 session_id 管理会话。 */

    /* 媒体收发统计。 */
    WebRtcStreamMediaCounters mediaCounters_; /* 视频/音频输入和广播的内部累计统计。 */
    WebRtcVideoDiagnostics videoDiagnostics_; /* 编码视频500 ms窗口与峰值诊断。 */

    /* PLI/FIR 与 IDR 请求调度状态。 */
    WebRtcKeyframeRequestState keyframeState_; /* 新客户端与 PLI/FIR 共用的 IDR 调度状态。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
