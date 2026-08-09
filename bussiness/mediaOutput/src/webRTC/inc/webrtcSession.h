#ifndef __WEBRTC_SESSION_H__
#define __WEBRTC_SESSION_H__

#include "websocketServer.h"
#include "webrtcTypes.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <functional>
#include <rtc/datachannel.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/track.hpp>
#include <string>
#include <vector>

namespace rkmedia {
namespace webrtc {

typedef std::function<void(int)> WebRtcSessionClosedCallback;
typedef std::function<void(int, WebRtcKeyframeRequestReason)> WebRtcSessionKeyframeRequestCallback;

/* 单个浏览器会话持有的 WebRTC 信令和媒体传输对象。 */
struct WebRtcSessionTransport {
    std::shared_ptr<communication::WebSocketConnection> connection; /* WebSocket 信令连接。 */
    std::shared_ptr<rtc::PeerConnection> pc; /* libdatachannel PeerConnection。 */
    std::shared_ptr<rtc::DataChannel> dc; /* 浏览器创建的 IPC DataChannel。 */
    std::shared_ptr<rtc::Track> videoTrack; /* H264 sendonly 视频 Track。 */
    std::shared_ptr<rtc::Track> audioTrack; /* G711 sendonly 音频 Track。 */
};

/* 单个浏览器会话的连接状态和可读状态文本，仅在 mutex_ 保护下读写。 */
struct WebRtcSessionRuntimeState {
    bool closed = false; /* 是否已进入关闭流程，防止重复释放资源。 */
    bool waitingForVideoKeyframe = false; /* video Track 打开后是否仍等待首个 IDR。 */
    bool pliTestSuppressingIdr = false; /* PLI 测试期间是否持续抑制该 session 的 IDR。 */
    std::chrono::steady_clock::time_point pliTestStartTime; /* PLI 测试开始时刻，用于超时恢复。 */
    std::string remoteAddress; /* 浏览器 WebSocket 对端地址。 */
    std::string path; /* 浏览器连接时使用的 WebSocket 路径。 */
    std::string peerState = "new"; /* PeerConnection 状态文本。 */
    std::string iceState = "new"; /* ICE 连接状态文本。 */
    std::string gatheringState = "new"; /* ICE candidate 收集状态文本。 */
};

/* 单个浏览器会话的累计收发与异常统计，仅在 mutex_ 保护下读写。 */
struct WebRtcSessionCounters {
    uint64_t signalingRxMessages = 0; /* 收到的 WebSocket 信令消息数量。 */
    uint64_t signalingTxMessages = 0; /* 发出的 WebSocket 信令消息数量。 */
    uint64_t localCandidates = 0; /* 发给浏览器的本地 ICE candidate 数量。 */
    uint64_t remoteCandidates = 0; /* 收到并加入 PeerConnection 的远端 ICE candidate 数量。 */
    uint64_t videoFrames = 0; /* 成功发送到 video Track 的编码帧数量。 */
    uint64_t videoBytes = 0; /* 成功发送到 video Track 的原始帧字节数。 */
    uint64_t videoNotReady = 0; /* video Track 未打开导致丢帧的次数。 */
    uint64_t videoWaitingKeyframeDrops = 0; /* 等待首个 IDR 时丢弃非关键帧的次数。 */
    uint64_t videoPliReceived = 0; /* 浏览器通过 RTCP PLI/FIR 请求视频关键帧的次数。 */
    uint64_t pliTestStarts = 0; /* 启动 PLI 测试的次数。 */
    uint64_t pliTestSuppressedIdr = 0; /* PLI 测试中故意丢弃的 IDR 数量。 */
    uint64_t pliTestPliRecovered = 0; /* PLI/FIR 到达后自动解除 IDR 抑制的次数。 */
    uint64_t pliTestTimeouts = 0; /* PLI 测试超时并恢复发送的次数。 */
    uint64_t videoSendFail = 0; /* 视频帧转换或发送失败次数。 */
    uint64_t audioFrames = 0; /* 成功发送到 audio Track 的音频包数量。 */
    uint64_t audioBytes = 0; /* 成功发送到 audio Track 的原始包字节数。 */
    uint64_t audioNotReady = 0; /* audio Track 未打开导致丢包的次数。 */
    uint64_t audioSendFail = 0; /* 音频包检查或发送失败次数。 */
    uint64_t dataChannelRxMessages = 0; /* 收到的 DataChannel 消息数量。 */
    uint64_t dataChannelTxMessages = 0; /* 发出的 DataChannel 消息数量。 */
    uint64_t dataChannelRxBytes = 0; /* 收到的 DataChannel 消息字节数。 */
    uint64_t dataChannelTxBytes = 0; /* 发出的 DataChannel 消息字节数。 */
};

/*
 * 单个浏览器 WebRTC 会话。
 * 一个 WebSocket 连接对应一个 WebRtcSession，内部管理 PeerConnection、DataChannel 和视频 Track。
 */
class WebRtcSession : public std::enable_shared_from_this<WebRtcSession> {
public:
    WebRtcSession(int id,
                  const WebRtcServerConfig &config,
                  const std::shared_ptr<communication::WebSocketConnection> &connection,
                  const WebRtcSessionClosedCallback &closedCallback,
                  const WebRtcSessionKeyframeRequestCallback &keyframeRequestCallback);
    ~WebRtcSession();

    void start();
    void close();
    int id() const;
    bool isVideoReady() const;
    bool sendVideoFrame(const WebRtcVideoFrame &frame);
    bool isAudioReady() const;
    bool sendAudioFrame(const WebRtcAudioFrame &frame);
    bool setPliTestIdrSuppression(bool enabled);
    void getStats(WebRtcSessionStats &stats) const;

private:
    void handleWsText(const std::string &message);
    void handleOffer(const std::string &message);
    void handleCandidate(const std::string &message);
    void createPeerConnection();
    void bindDataChannel(const std::shared_ptr<rtc::DataChannel> &dc);
    void addH264VideoTrack(const std::string &mid, uint8_t payloadType);
    void addG711AudioTrack(const std::string &mid, uint8_t payloadType, WebRtcAudioCodec codec);
    void sendDescription(const rtc::Description &description);
    void sendCandidate(const rtc::Candidate &candidate);
    void handleIpcMessage(const std::string &message);
    void requestVideoKeyframe(WebRtcKeyframeRequestReason reason);
    bool convertAnnexBToLengthPrefixed(const uint8_t *data, size_t size, rtc::binary &frame);
    bool copyLengthPrefixedFrame(const uint8_t *data, size_t size, rtc::binary &frame);

private:
    int id_; /* WebRtcServer 分配的内部会话 ID。 */
    WebRtcServerConfig config_; /* 创建会话时的服务端配置副本。 */
    mutable std::mutex mutex_; /* 保护 transport_、state_ 和 counters_。 */
    WebRtcSessionClosedCallback closedCallback_; /* 会话关闭后通知 WebRtcServer 从会话表移除。 */
    WebRtcSessionKeyframeRequestCallback keyframeRequestCallback_; /* 上报新会话或 PLI/FIR 的 IDR 请求。 */
    WebRtcSessionTransport transport_; /* WebSocket、PeerConnection、Track 等资源所有权。 */
    WebRtcSessionRuntimeState state_; /* 会话生命周期、协商和关键帧门控状态。 */
    WebRtcSessionCounters counters_; /* 信令、媒体和 DataChannel 的累计计数。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
