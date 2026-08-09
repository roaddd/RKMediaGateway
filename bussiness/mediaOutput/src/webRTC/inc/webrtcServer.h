#ifndef __WEBRTC_SERVER_H__
#define __WEBRTC_SERVER_H__

#include "websocketServer.h"
#include "webrtcSession.h"
#include "webrtcTypes.h"

#include <map>
#include <memory>
#include <mutex>
#include <chrono>

namespace rkmedia {
namespace webrtc {

/* WebRtcServer 接收媒体帧及广播结果的内部累计统计，仅在 mutex_ 保护下读写。 */
struct WebRtcServerMediaCounters {
    uint64_t inputVideoFrames = 0;       /* 从 mediaOutput 接收到的视频帧数量。 */
    uint64_t inputVideoBytes = 0;        /* 从 mediaOutput 接收到的视频原始字节数。 */
    uint64_t inputAudioFrames = 0;       /* 从 mediaOutput 接收到的音频包数量。 */
    uint64_t inputAudioBytes = 0;        /* 从 mediaOutput 接收到的音频原始字节数。 */
    uint64_t videoBroadcastTargets = 0;  /* 视频帧成功发送到的 session 总次数，一帧广播给多个 session 会累计多次。 */
    uint64_t audioBroadcastTargets = 0;  /* 音频包成功发送到的 session 总次数，一包广播给多个 session 会累计多次。 */
    uint64_t videoNoReadySession = 0;    /* 视频帧到达时没有任何可发送 video Track 的次数。 */
    uint64_t audioNoReadySession = 0;    /* 音频包到达时没有任何可发送 audio Track 的次数。 */
};

/*
 * WebRtcServer 的 IDR 请求合并和 PLI 限频状态。
 * 该结构不直接调用编码器，只保存等待 mediaGateway 消费的请求标记与统计。
 */
struct WebRtcKeyframeRequestState {
    bool pendingVideoKeyframeRequest = false; /* 新客户端或未限频 PLI 触发的立即 IDR 请求。 */
    bool pendingPliKeyframeRequest = false;   /* 被 PLI 限频合并后，等待限频窗口结束的 IDR 请求。 */
    bool hasLastVideoIdrRequestTime = false;  /* lastVideoIdrRequestTime 是否已经有有效记录。 */
    std::chrono::steady_clock::time_point lastVideoIdrRequestTime; /* 最近一次请求 MPP 输出 IDR 的单调时钟时刻。 */
    uint64_t videoKeyframeRequests = 0;       /* 已进入立即请求队列的 IDR 请求次数。 */
    uint64_t videoPliReceived = 0;            /* 所有浏览器 session 上报的 PLI/FIR 总次数。 */
    uint64_t videoPliAccepted = 0;            /* 未命中限频、可立即请求 IDR 的 PLI/FIR 次数。 */
    uint64_t videoPliDeferred = 0;            /* 被已有 IDR 或限频窗口合并/延迟的 PLI/FIR 次数。 */
};

/*
 * WebRTC 输出服务。
 * 负责启动 WebSocket 信令服务、创建浏览器会话，并把编码帧广播到所有已就绪的视频 Track。
 */
class WebRtcServer {
public:
    WebRtcServer();
    ~WebRtcServer();

    bool start(const WebRtcServerConfig &config);
    void stop();
    int sendVideoFrame(const WebRtcVideoFrame &frame);
    int sendAudioFrame(const WebRtcAudioFrame &frame);
    int sessionCount() const;
    bool consumeVideoKeyframeRequest();
    void getStats(WebRtcServerStats &stats) const;

private:
    void handleClient(const std::shared_ptr<communication::WebSocketConnection> &connection);
    void removeSession(int id);
    void requestVideoKeyframe(int sessionId, WebRtcKeyframeRequestReason reason);

private:
    WebRtcServerConfig config_; /* 启动时保存的监听、音视频格式等配置。 */
    std::shared_ptr<communication::WebSocketServer> wsServer_; /* WebSocket 信令监听服务。 */
    std::map<int, std::shared_ptr<WebRtcSession>> sessions_; /* 按内部 session ID 管理的浏览器会话表。 */
    mutable std::mutex mutex_; /* 保护运行状态、会话表、统计和关键帧调度状态。 */
    int nextSessionId_; /* 下一个分配给新浏览器连接的内部 session ID。 */
    bool running_; /* WebSocket 服务是否已成功启动且尚未停止。 */
    WebRtcServerMediaCounters mediaCounters_; /* 视频/音频输入和广播的内部累计统计。 */
    WebRtcKeyframeRequestState keyframeState_; /* 新客户端与 PLI/FIR 共用的 IDR 调度状态。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
