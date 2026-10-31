/**
 * @file webrtcStats.h
 * @brief 定义 WebRTC 会话、媒体端点和信令调试统计快照。
 */
#ifndef __WEBRTC_STATS_H__
#define __WEBRTC_STATS_H__

#include "webrtcMediaTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rkmedia {
namespace webrtc {

/** @brief 单个浏览器 WebRTC 会话的只读调试快照。 */
struct WebRtcSessionStats {
    std::string sessionId;                       /* 云端分配的唯一会话 ID。 */
    std::string remoteAddress;                   /* 信令对端地址或云端 session_id。 */
    std::string path;                            /* 本地 WebSocket 路径或云端逻辑路径。 */
    std::string peerState;                       /* PeerConnection 状态文本。 */
    std::string iceState;                        /* ICE 连接状态文本。 */
    std::string gatheringState;                  /* ICE Candidate 收集状态文本。 */
    bool closed;                                 /* 会话是否已经进入关闭状态。 */
    bool signalingReady;                         /* 会话逻辑信令通道当前是否可发送。 */
    bool dataChannelOpen;                        /* DataChannel 当前是否打开。 */
    bool videoTrackReady;                        /* 视频 Track 是否可发送。 */
    bool waitingForVideoKeyframe;                /* 是否等待首个可解码 IDR。 */
    bool pliTestSuppressingVideo;                /* PLI 测试是否正在抑制视频发送。 */
    bool audioTrackReady;                        /* 音频 Track 是否可发送。 */
    uint64_t signalingRxMessages;                /* 收到的会话信令消息数。 */
    uint64_t signalingTxMessages;                /* 发出的会话信令消息数。 */
    uint64_t localCandidates;                    /* 发给远端的本地 ICE Candidate 数。 */
    uint64_t remoteCandidates;                   /* 加入 PeerConnection 的远端 Candidate 数。 */
    uint64_t videoFrames;                        /* 成功发送的视频帧数。 */
    uint64_t videoBytes;                         /* 成功发送的视频原始字节数。 */
    uint64_t videoNotReady;                      /* 视频 Track 未就绪导致的丢帧数。 */
    uint64_t videoWaitingKeyframeDrops;          /* 等待 IDR 时丢弃的非关键帧数。 */
    uint64_t videoPliReceived;                   /* 收到的 PLI/FIR 请求数。 */
    uint64_t pliTestStarts;                      /* PLI 测试启动次数。 */
    uint64_t pliTestSuppressedVideoFrames;       /* PLI 测试主动抑制的视频帧数。 */
    uint64_t pliTestPliRecovered;                /* 收到 PLI/FIR 后恢复发送的次数。 */
    uint64_t pliTestTimeouts;                    /* PLI 测试超时恢复次数。 */
    uint64_t videoSendFail;                      /* 视频转换或发送失败次数。 */
    uint64_t videoSendCallAverageUs;             /* video Track sendFrame 平均调用耗时。 */
    uint64_t videoSendCallMaxUs;                 /* video Track sendFrame 最大调用耗时。 */
    uint64_t videoBufferedAmount;                /* 最近一次发送后的 Track 待发送字节数。 */
    uint64_t videoBufferedAmountMax;             /* Track 待发送字节数历史高水位。 */
    uint64_t audioFrames;                        /* 成功发送的音频编码包数。 */
    uint64_t audioBytes;                         /* 成功发送的音频编码字节数。 */
    uint64_t audioNotReady;                      /* 音频 Track 未就绪导致的丢包数。 */
    uint64_t audioSendFail;                      /* 音频校验或发送失败次数。 */
    uint64_t incomingAudioPackets;               /* 成功接收的上行音频 RTP 包数。 */
    uint64_t incomingAudioRtpBytes;              /* 上行音频完整 RTP 包字节数。 */
    uint64_t incomingAudioPayloadBytes;          /* 上行音频有效负载字节数。 */
    uint64_t incomingAudioMalformed;             /* 格式非法的上行 RTP 包数。 */
    uint64_t incomingAudioPtMismatch;            /* PT 与协商结果不一致的包数。 */
    uint64_t incomingAudioRtcpPackets;           /* 音频 Track 收到的 RTCP 包数。 */
    uint64_t incomingAudioSequenceGaps;          /* 根据序号跳变估算的丢包数。 */
    uint64_t incomingAudioDuplicates;            /* 重复 RTP 包数。 */
    uint64_t incomingAudioOutOfOrder;            /* 乱序 RTP 包数。 */
    uint64_t incomingAudioSsrcChanges;           /* 上行音频 SSRC 变化次数。 */
    bool hasIncomingAudioPacket;                 /* 是否收到过有效上行音频 RTP。 */
    uint32_t incomingAudioLastSsrc;              /* 最近有效上行音频包的 SSRC。 */
    uint16_t incomingAudioLastSequence;          /* 最近有效上行音频包的序号。 */
    uint32_t incomingAudioLastTimestamp;         /* 最近有效上行音频包的 RTP 时间戳。 */
    uint64_t dataChannelRxMessages;              /* 收到的 DataChannel 消息数。 */
    uint64_t dataChannelTxMessages;              /* 发出的 DataChannel 消息数。 */
    uint64_t dataChannelRxBytes;                 /* 收到的 DataChannel 消息字节数。 */
    uint64_t dataChannelTxBytes;                 /* 发出的 DataChannel 消息字节数。 */
};

/** @brief 单路 WebRtcStreamEndpoint 的只读调试快照。 */
struct WebRtcStreamEndpointStats {
    std::string name;                            /* WebRTC 输出名称。 */
    std::string streamId;                        /* 云端路由使用的码流标识。 */
    std::string signalingEndpoint;               /* 本地监听地址或云端服务器 URL。 */
    bool signalingConnected;                     /* 信令入口是否处于可用状态。 */
    bool running;                                /* 媒体端点是否正在运行。 */
    uint32_t videoFps;                           /* 视频 RTP 时间戳使用的帧率。 */
    WebRtcAudioCodec audioCodec;                 /* 当前音频 Track 编码格式。 */
    uint32_t audioSampleRate;                    /* 音频采样率。 */
    uint32_t audioChannels;                      /* 音频编码通道数。 */
    uint64_t inputVideoFrames;                   /* 输入端点的视频帧数。 */
    uint64_t inputVideoBytes;                    /* 输入端点的视频字节数。 */
    bool videoWindowValid;                       /* 最近500 ms视频窗口统计是否有效。 */
    uint64_t videoWindowDurationMs;              /* 最近视频窗口实际时长。 */
    uint64_t videoWindowFrames;                  /* 最近视频窗口帧数。 */
    uint64_t videoWindowBytes;                   /* 最近视频窗口字节数。 */
    uint64_t videoWindowBitrateBps;              /* 最近视频窗口瞬时码率。 */
    uint64_t videoWindowKeyFrames;               /* 最近视频窗口关键帧数。 */
    uint64_t videoWindowMaxFrameBytes;           /* 最近窗口最大单帧字节数。 */
    uint64_t videoWindowMaxKeyFrameBytes;        /* 最近窗口最大关键帧字节数。 */
    uint64_t videoMaxFrameBytes;                 /* 端点运行期间最大单帧字节数。 */
    uint64_t videoMaxKeyFrameBytes;              /* 端点运行期间最大关键帧字节数。 */
    uint64_t videoPeakBitrateBps;                /* 端点运行期间最高窗口瞬时码率。 */
    uint64_t inputAudioFrames;                   /* 输入端点的音频包数。 */
    uint64_t inputAudioBytes;                    /* 输入端点的音频字节数。 */
    uint64_t videoBroadcastTargets;              /* 视频成功投递到会话的累计次数。 */
    uint64_t audioBroadcastTargets;              /* 音频成功投递到会话的累计次数。 */
    uint64_t videoNoReadySession;                /* 没有可用视频会话的累计次数。 */
    uint64_t audioNoReadySession;                /* 没有可用音频会话的累计次数。 */
    uint64_t incomingAudioPackets;               /* 所有会话有效上行音频包总数。 */
    uint64_t incomingAudioPayloadBytes;          /* 所有会话上行音频负载总字节数。 */
    bool pendingVideoKeyframeRequest;            /* 是否有立即执行的 IDR 请求。 */
    bool pendingPliKeyframeRequest;              /* 是否有被限频延迟的 PLI 请求。 */
    uint64_t videoKeyframeRequests;              /* 已接受的 IDR 请求数。 */
    uint64_t videoPliReceived;                   /* 所有会话收到的 PLI/FIR 总数。 */
    uint64_t videoPliAccepted;                   /* 可立即触发 IDR 的 PLI/FIR 数。 */
    uint64_t videoPliDeferred;                   /* 被合并或限频的 PLI/FIR 数。 */
    bool hasLastVideoPliTime;                    /* 最近 PLI 时间是否有效。 */
    bool hasLastVideoIdrRequestTime;             /* 最近 IDR 请求时间是否有效。 */
    bool hasLastVideoIdrInputTime;               /* 最近 IDR 输入时间是否有效。 */
    uint64_t lastVideoPliAgeMs;                  /* 最近 PLI 距当前的毫秒数。 */
    uint64_t lastVideoIdrRequestAgeMs;           /* 最近 IDR 请求距当前的毫秒数。 */
    uint64_t lastVideoIdrInputAgeMs;             /* 最近 IDR 输入距当前的毫秒数。 */
    bool hasLastPliToIdrRequestDelay;            /* PLI 到 IDR 请求耗时是否有效。 */
    bool hasLastIdrRequestToInputDelay;          /* IDR 请求到 IDR 输入耗时是否有效。 */
    uint64_t lastPliToIdrRequestMs;              /* 最近 PLI 到 IDR 请求耗时。 */
    uint64_t lastIdrRequestToInputMs;            /* 最近 IDR 请求到 IDR 输入耗时。 */
    std::vector<WebRtcSessionStats> sessions;    /* 当前会话统计快照集合。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
