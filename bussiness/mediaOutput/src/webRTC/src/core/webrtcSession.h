/**
 * @file webrtcSession.h
 * @brief 定义单个浏览器 PeerConnection 会话及其媒体收发状态。
 *
 * WebRtcSession 负责 SDP/ICE 协商、Track、DataChannel 和 RTP 媒体处理，通过
 * 构造时注入的发送、关闭回调收发信令，不直接依赖共享 WebSocket 客户端。
 */
#ifndef __WEBRTC_SESSION_H__
#define __WEBRTC_SESSION_H__

#include "webrtcAudioReceiver.h"
#include "webrtcConfig.h"
#include "webrtcMediaTypes.h"
#include "webrtcStats.h"

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

typedef std::function<bool(const std::string &)> WebRtcSessionSignalingSendCallback;
typedef std::function<void(const char *)> WebRtcSessionSignalingCloseCallback;
typedef std::function<void(const std::string &)> WebRtcSessionClosedCallback;
typedef std::function<void(const std::string &, WebRtcKeyframeRequestReason)>
    WebRtcSessionKeyframeRequestCallback;
typedef std::function<void(const WebRtcIncomingAudioPacket &)>
    WebRtcSessionIncomingAudioCallback;
/* 单个浏览器会话持有的 WebRTC 媒体传输对象。 */
struct WebRtcSessionTransport {
    std::shared_ptr<rtc::PeerConnection> pc; /* libdatachannel PeerConnection。 */
    std::shared_ptr<rtc::DataChannel> dc; /* 浏览器创建的 IPC DataChannel。 */
    std::shared_ptr<rtc::Track> videoTrack; /* H264 sendonly 视频 Track。 */
    std::shared_ptr<rtc::Track> audioTrack; /* 可按 Offer 协商为 sendonly/sendrecv 的音频 Track。 */
};

/* 单个浏览器会话的连接状态和可读状态文本，仅在 mutex_ 保护下读写。 */
struct WebRtcSessionRuntimeState {
    bool closed = false; /* 是否已进入关闭流程，防止重复释放资源。 */
    bool remoteDescriptionSet = false; /* 设置远端 SDP 后才能提交暂存的 ICE candidate。 */
    bool waitingForVideoKeyframe = false; /* video Track 打开后是否仍等待首个 IDR。 */
    bool pliTestSuppressingVideo = false; /* PLI 测试期间是否持续抑制该 session 的全部视频帧。 */
    std::chrono::steady_clock::time_point pliTestStartTime; /* PLI 测试开始时刻，用于超时恢复。 */
    std::string remoteAddress; /* 云端 session_id 的可读描述。 */
    std::string path; /* 云端设备信令逻辑路径。 */
    std::string peerState = "new"; /* PeerConnection 状态文本。 */
    std::string iceState = "new"; /* ICE 连接状态文本。 */
    std::string gatheringState = "new"; /* ICE candidate 收集状态文本。 */
};

/* 单个浏览器会话的累计收发与异常统计，仅在 mutex_ 保护下读写。 */
struct WebRtcSessionCounters {
    uint64_t signalingRxMessages = 0; /* 收到的会话信令消息数量。 */
    uint64_t signalingTxMessages = 0; /* 发出的会话信令消息数量。 */
    uint64_t localCandidates = 0; /* 发给浏览器的本地 ICE candidate 数量。 */
    uint64_t remoteCandidates = 0; /* 收到并加入 PeerConnection 的远端 ICE candidate 数量。 */
    uint64_t videoFrames = 0; /* 成功发送到 video Track 的编码帧数量。 */
    uint64_t videoBytes = 0; /* 成功发送到 video Track 的原始帧字节数。 */
    uint64_t videoNotReady = 0; /* video Track 未打开导致丢帧的次数。 */
    uint64_t videoWaitingKeyframeDrops = 0; /* 等待首个 IDR 时丢弃非关键帧的次数。 */
    uint64_t videoPliReceived = 0; /* 浏览器通过 RTCP PLI/FIR 请求视频关键帧的次数。 */
    uint64_t pliTestStarts = 0; /* 启动 PLI 测试的次数。 */
    uint64_t pliTestSuppressedVideoFrames = 0; /* PLI 测试中故意丢弃的视频帧数量。 */
    uint64_t pliTestPliRecovered = 0; /* PLI/FIR 到达后自动解除视频抑制的次数。 */
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

/**
 * @brief 单个浏览器 WebRTC 会话。
 *
 * 每个会话独立管理 PeerConnection、DataChannel 与媒体 Track。构造时注入的信令
 * 回调由云端路由层提供，类内不感知共享 WebSocket 的连接管理。
 */
class WebRtcSession : public std::enable_shared_from_this<WebRtcSession> {
public:
    /**
     * @brief 创建一个由云端 session_id 标识的 WebRTC 浏览会话。
     * @param[in] sessionId 云端分配的非空会话 ID，用于路由、日志和统计。
     * @param[in] config 当前码流端点的配置快照，包含媒体格式和 ICE 参数。
     * @param[in] signalingSendCallback 向云端发送本会话 SDP/ICE 消息的回调。
     * @param[in] signalingCloseCallback 注销本会话云端路由并发送关闭通知的回调。
     * @param[in] closedCallback 会话关闭后通知端点移除本会话的回调。
     * @param[in] keyframeRequestCallback 向端点提交新会话或 PLI/FIR 关键帧请求的回调。
     * @param[in] incomingAudioCallback 向端点交付浏览器上行音频 RTP 负载的回调。
     * @note 对象必须由 std::shared_ptr 管理，异步回调通过 shared_from_this() 获取会话。
     */
    WebRtcSession(const std::string &sessionId,
                  const WebRtcStreamEndpointConfig &config,
                  const WebRtcSessionSignalingSendCallback &signalingSendCallback,
                  const WebRtcSessionSignalingCloseCallback &signalingCloseCallback,
                  const WebRtcSessionClosedCallback &closedCallback,
                  const WebRtcSessionKeyframeRequestCallback &keyframeRequestCallback,
                  const WebRtcSessionIncomingAudioCallback &incomingAudioCallback);

    /** @brief 销毁会话；析构时会幂等执行 close() 释放底层传输对象。 */
    ~WebRtcSession();

    /**
     * @brief 关闭云端会话路由、PeerConnection、Track 和 DataChannel。
     * @note 本函数允许重复调用；只有第一次调用会执行关闭回调和资源释放。
     */
    void close();

    /**
     * @brief 获取云端分配的唯一 session_id。
     * @return 会话 ID 的只读引用，其有效期不超过当前 WebRtcSession 对象。
     */
    const std::string &sessionId() const;

    /**
     * @brief 处理云端已按 session_id 路由到本会话的信令消息。
     * @param[in] message 完整的 UTF-8 JSON 信令消息。
     * @details 支持 offer、candidate 和 close；解析失败或未知类型会记录日志并忽略。
     */
    void handleSignalingMessage(const std::string &message);

    /**
     * @brief 处理云端通知的远端会话关闭。
     * @details 先清除信令关闭回调再调用 close()，避免向云端重复发送 close 消息。
     */
    void handleSignalingClosed();

    /**
     * @brief 查询视频 Track 当前是否可发送 RTP。
     * @retval true 视频 Track 存在且已经打开。
     * @retval false Track 尚未创建、尚未打开或已经关闭。
     */
    bool isVideoReady() const;

    /**
     * @brief 向浏览器发送一帧 H.264 编码视频。
     * @param[in] frame 待发送的视频帧及其格式、PTS 和关键帧标记。
     * @retval true 视频帧已经成功提交给 Track。
     * @retval false Track 未就绪、正在等待关键帧、测试抑制、帧格式非法或发送失败。
     */
    bool sendVideoFrame(const WebRtcVideoFrame &frame);

    /**
     * @brief 查询音频 Track 当前是否可发送 RTP。
     * @retval true 音频 Track 存在且已经打开。
     * @retval false Track 尚未创建、尚未打开或已经关闭。
     */
    bool isAudioReady() const;

    /**
     * @brief 向浏览器发送一帧已经编码的 G711 或 Opus 音频。
     * @param[in] frame 待发送的音频编码包、编解码格式和时间信息。
     * @retval true 音频帧已经成功提交给 Track。
     * @retval false Track 未就绪、编码格式不匹配、输入非法或发送失败。
     */
    bool sendAudioFrame(const WebRtcAudioFrame &frame);

    /**
     * @brief 启用或取消当前会话的 PLI 测试视频抑制。
     * @param[in] enabled true 表示停止发送视频并等待浏览器 PLI/FIR；false 表示取消抑制。
     * @retval true 状态设置成功。
     * @retval false 会话已关闭，或启用测试时视频 Track 尚未打开。
     * @note 抑制只影响视频帧，音频、RTCP 和信令仍然正常处理。
     */
    bool setPliTestVideoSuppression(bool enabled);

    /**
     * @brief 生成当前会话的线程安全统计快照。
     * @param[out] stats 接收连接状态、媒体计数、DataChannel 和入站音频统计。
     */
    void getStats(WebRtcSessionStats &stats) const;

private:
    /**
     * @brief 解析浏览器 Offer，创建媒体 Track 并生成 Answer。
     * @param[in] message 包含 offer SDP 的完整 JSON 信令消息。
     * @details 协商失败会记录错误；媒体参数不兼容时关闭当前会话。
     */
    void handleOffer(const std::string &message);

    /**
     * @brief 解析并提交浏览器的 trickle ICE candidate。
     * @param[in] message 包含 candidate 和 mid 的完整 JSON 信令消息。
     * @details Offer 尚未设置时最多暂存 128 条消息，远端 SDP 设置后按到达顺序补交。
     */
    void handleCandidate(const std::string &message);

    /**
     * @brief 创建 PeerConnection，并注册 SDP、ICE、DataChannel 和状态回调。
     * @details 创建失败时记录错误并保持 transport_.pc 为空；成功后才发布该对象。
     */
    void createPeerConnection();

    /**
     * @brief 保存浏览器创建的 DataChannel 并注册生命周期和文本消息回调。
     * @param[in] dc libdatachannel 交付的 DataChannel 对象。
     * @pre dc 非空，且当前 WebRtcSession 由 std::shared_ptr 管理。
     */
    void bindDataChannel(const std::shared_ptr<rtc::DataChannel> &dc);

    /**
     * @brief 按 Offer 协商结果创建 H.264 sendonly Track 及 RTP/RTCP 处理链。
     * @param[in] mid Offer 中 video m-line 的媒体标识。
     * @param[in] payloadType Offer 选出的 H.264 RTP Payload Type。
     */
    void addH264VideoTrack(const std::string &mid, uint8_t payloadType);

    /**
     * @brief 按 Offer 协商结果创建 G711/Opus 音频 Track 及 RTP/RTCP 处理链。
     * @param[in] mid Offer 中 audio m-line 的媒体标识。
     * @param[in] payloadType Offer 选出的音频 RTP Payload Type。
     * @param[in] codec 当前输出使用的音频编码格式。
     * @param[in] receiveRemoteAudio true 创建 sendrecv Track 并接收浏览器音频；
     *                                false 创建 sendonly Track。
     */
    void addAudioTrack(const std::string &mid,
                       uint8_t payloadType,
                       WebRtcAudioCodec codec,
                       bool receiveRemoteAudio);

    /**
     * @brief 解析 audio Track 收到的 RTP/RTCP，并把有效编码负载交给媒体端点。
     * @param[in] message libdatachannel 交付的完整 RTP 或 RTCP 字节串。
     * @details 到达时间使用 steady_clock，仅用于同机时延和抖动统计。
     */
    void handleIncomingAudioMessage(rtc::binary message);

    /**
     * @brief 将本地 SDP 描述封装为信令 JSON 并发往云端。
     * @param[in] description libdatachannel 生成的本地 Answer 描述。
     */
    void sendDescription(const rtc::Description &description);

    /**
     * @brief 将一个本地 ICE candidate 封装为信令 JSON 并发往云端。
     * @param[in] candidate libdatachannel 收集到的本地候选。
     */
    void sendCandidate(const rtc::Candidate &candidate);

    /**
     * @brief 通过构造时注入的会话信令回调发送一条消息。
     * @param[in] message 完整的 UTF-8 JSON 信令消息。
     * @retval true 消息已由云端信令客户端接受。
     * @retval false 发送回调不可用或云端客户端拒绝发送。
     */
    bool sendSignalingMessage(const std::string &message);

    /**
     * @brief 解析并处理浏览器通过 DataChannel 发送的 IPC 命令。
     * @param[in] message 完整的 DataChannel 文本消息。
     * @details 当前支持 ping 和 get_status，并通过同一 DataChannel 返回响应。
     */
    void handleIpcMessage(const std::string &message);

    /**
     * @brief 将本会话的关键帧需求上报给所属媒体端点。
     * @param[in] reason 新会话、PLI/FIR 或测试超时等请求原因。
     * @details 端点负责对多会话请求进行合并和限频。
     */
    void requestVideoKeyframe(WebRtcKeyframeRequestReason reason);

    /**
     * @brief 把 Annex-B H.264 帧转换为四字节大端长度前缀格式。
     * @param[in] data Annex-B 帧首地址。
     * @param[in] size Annex-B 帧字节数。
     * @param[out] frame 接收适用于 H264RtpPacketizer 的帧数据。
     * @retval true 至少成功转换一个 NALU。
     * @retval false 输入为空或未找到有效 Annex-B 起始码。
     */
    bool convertAnnexBToLengthPrefixed(const uint8_t *data, size_t size, rtc::binary &frame);

    /**
     * @brief 复制已经采用四字节长度前缀格式的 H.264 帧。
     * @param[in] data 输入帧首地址。
     * @param[in] size 输入帧字节数。
     * @param[out] frame 接收复制后的 libdatachannel 二进制帧。
     * @retval true 复制成功。
     * @retval false 输入地址为空或长度为零。
     */
    bool copyLengthPrefixedFrame(const uint8_t *data, size_t size, rtc::binary &frame);

private:
    /* 会话身份与不可变配置：构造完成后只读，不需要 mutex_ 保护。 */
    std::string sessionId_; /* 云端分配的唯一 session_id，同时用于路由、日志和统计。 */
    WebRtcStreamEndpointConfig config_; /* 创建会话时的服务端配置副本。 */

    /* 并发同步：保护回调、运行状态、媒体对象和候选缓存。 */
    mutable std::mutex mutex_;

    /* 上层回调：连接会话与云端路由、媒体端点；访问或修改时须持有 mutex_。 */
    WebRtcSessionSignalingSendCallback signalingSendCallback_; /* 发送本会话的 SDP 和 candidate。 */
    WebRtcSessionSignalingCloseCallback signalingCloseCallback_; /* 注销本会话的云端路由。 */
    WebRtcSessionClosedCallback closedCallback_; /* 会话关闭后通知 WebRtcStreamEndpoint 从会话表移除。 */
    WebRtcSessionKeyframeRequestCallback keyframeRequestCallback_; /* 上报新会话或 PLI/FIR 的 IDR 请求。 */
    WebRtcSessionIncomingAudioCallback incomingAudioCallback_; /* 向媒体端点交付浏览器音频包。 */

    /* 会话运行状态与统计：所有读写均须持有 mutex_。 */
    WebRtcSessionRuntimeState state_; /* 会话生命周期、协商和关键帧门控状态。 */
    WebRtcSessionCounters counters_; /* 信令、媒体和 DataChannel 的累计计数。 */
    std::vector<std::string> pendingRemoteCandidates_; /* SDP 到达前暂存的 candidate，最多 128 条。 */

    /* WebRTC 媒体对象：所有权和指针替换由 mutex_ 保护，底层调用在锁外执行。 */
    WebRtcSessionTransport transport_; /* 本会话 PeerConnection、DataChannel 和 Track 的所有权。 */
    std::shared_ptr<WebRtcAudioReceiver> audioReceiver_; /* audio Track 的 RTP 解析与统计对象。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
