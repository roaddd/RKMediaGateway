#ifndef __WEBRTC_TYPES_H__
#define __WEBRTC_TYPES_H__

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace rkmedia {
namespace webrtc {

enum H264FrameFormat {
    H264_FRAME_FORMAT_ANNEX_B = 0,      /* 00 00 00 01 起始码格式。 */
    H264_FRAME_FORMAT_LENGTH_PREFIX = 1 /* 4 字节长度前缀格式。 */
};

enum WebRtcAudioCodec {
    WEBRTC_AUDIO_CODEC_NONE = 0, /* 不启用 WebRTC 音频 Track。 */
    WEBRTC_AUDIO_CODEC_PCMA = 1, /* G711 A-law，对应 SDP 中的 PCMA/8000。 */
    WEBRTC_AUDIO_CODEC_PCMU = 2  /* G711 mu-law，对应 SDP 中的 PCMU/8000。 */
};

/* WebRTC 视频关键帧请求的来源，用于区分新会话首帧与浏览器解码恢复。 */
enum WebRtcKeyframeRequestReason {
    WEBRTC_KEYFRAME_REQUEST_NEW_SESSION = 0, /* 新 video Track 打开，需要从 IDR 开始解码。 */
    WEBRTC_KEYFRAME_REQUEST_PLI = 1,         /* 浏览器通过 RTCP PLI/FIR 请求关键帧。 */
    WEBRTC_KEYFRAME_REQUEST_TEST_TIMEOUT = 2 /* PLI 测试超时后请求恢复用 IDR。 */
};

struct WebRtcServerConfig {
    std::string name;        /* 输出通道名称，用于日志。 */
    std::string bindAddress; /* WebSocket 信令监听地址。 */
    uint16_t port;           /* WebSocket 信令监听端口。 */
    uint32_t videoFps;       /* 视频帧率，用于缺省 RTP 媒体时间递增。 */
    WebRtcAudioCodec audioCodec; /* 浏览器音频 Track 使用的 G711 编码类型。 */
    uint32_t audioSampleRate;     /* 音频采样率，PCMA/PCMU 固定使用 8000Hz。 */
    uint32_t audioChannels;       /* 音频声道数，PCMA/PCMU 当前只支持单声道。 */
};

struct WebRtcVideoFrame {
    const uint8_t *data;       /* 编码帧数据地址。 */
    size_t size;               /* 编码帧字节数。 */
    uint64_t ptsUs;            /* 媒体时间戳，单位微秒。 */
    bool keyFrame;             /* 是否关键帧。 */
    H264FrameFormat format;    /* H264 输入格式。 */
};

struct WebRtcAudioFrame {
    const uint8_t *data;    /* 编码后的 G711 音频数据地址。 */
    size_t size;            /* 编码后音频数据字节数。 */
    uint64_t ptsUs;         /* 媒体时间戳，单位微秒。 */
    WebRtcAudioCodec codec; /* 当前音频帧编码类型，必须和协商出的 Track 一致。 */
};

/*
 * 单个浏览器 WebRTC 会话的运行统计快照。
 * 该结构只用于调试命令输出，调用方拿到的是某一时刻的副本，不直接持有 WebRtcSession 内部锁。
 */
struct WebRtcSessionStats {
    int id;                         /* WebRtcServer 内部分配的会话编号。 */
    std::string remoteAddress;      /* 浏览器 WebSocket 连接的对端地址。 */
    std::string path;               /* 浏览器连接 WebSocket 时使用的路径。 */
    std::string peerState;          /* PeerConnection 当前状态。 */
    std::string iceState;           /* ICE 当前状态。 */
    std::string gatheringState;     /* ICE candidate 收集状态。 */
    bool closed;                    /* 会话是否已经进入关闭流程。 */
    bool websocketOpen;             /* 信令 WebSocket 是否仍处于打开状态。 */
    bool dataChannelOpen;           /* DataChannel 是否已经打开。 */
    bool videoTrackReady;           /* 视频 Track 是否已经可以发送 RTP。 */
    bool waitingForVideoKeyframe;   /* 视频 Track 打开后是否仍在等待首个关键帧。 */
    bool pliTestSuppressingVideo;   /* PLI 测试是否正在抑制该 session 的全部视频帧。 */
    bool audioTrackReady;           /* 音频 Track 是否已经可以发送 RTP。 */
    uint64_t signalingRxMessages;   /* 收到的 WebSocket 信令消息数量。 */
    uint64_t signalingTxMessages;   /* 发出的 WebSocket 信令消息数量。 */
    uint64_t localCandidates;       /* 已发送给浏览器的本地 ICE candidate 数量。 */
    uint64_t remoteCandidates;      /* 已收到并添加的浏览器 ICE candidate 数量。 */
    uint64_t videoFrames;           /* 成功发送到视频 Track 的帧数。 */
    uint64_t videoBytes;            /* 成功发送到视频 Track 的原始帧字节数。 */
    uint64_t videoNotReady;         /* 视频 Track 未就绪导致丢弃的次数。 */
    uint64_t videoWaitingKeyframeDrops; /* 等待首个关键帧期间丢弃的非关键帧次数。 */
    uint64_t videoPliReceived;           /* 当前浏览器 session 发出的 PLI/FIR 请求次数。 */
    uint64_t pliTestStarts;              /* 对当前 session 启动 PLI 测试的次数。 */
    uint64_t pliTestSuppressedVideoFrames; /* PLI 测试期间故意不发送的视频帧数量。 */
    uint64_t pliTestPliRecovered;        /* PLI/FIR 到达后自动解除视频抑制的次数。 */
    uint64_t pliTestTimeouts;            /* PLI 测试超时并自动恢复的次数。 */
    uint64_t videoSendFail;         /* 视频帧转换或发送失败次数。 */
    uint64_t audioFrames;           /* 成功发送到音频 Track 的帧数。 */
    uint64_t audioBytes;            /* 成功发送到音频 Track 的音频包字节数。 */
    uint64_t audioNotReady;         /* 音频 Track 未就绪导致丢弃的次数。 */
    uint64_t audioSendFail;         /* 音频包检查或发送失败次数。 */
    uint64_t dataChannelRxMessages; /* DataChannel 收到的 IPC 消息数量。 */
    uint64_t dataChannelTxMessages; /* DataChannel 发出的 IPC 回复数量。 */
    uint64_t dataChannelRxBytes;    /* DataChannel 收到的 IPC 消息字节数。 */
    uint64_t dataChannelTxBytes;    /* DataChannel 发出的 IPC 回复字节数。 */
};

/*
 * 单个 WebRtcServer 的运行统计快照。
 * server 统计用于观察整体输入和广播情况，sessions 保存每个浏览器连接的明细。
 */
struct WebRtcServerStats {
    std::string name;                       /* WebRTC 输出通道名称。 */
    std::string bindAddress;                /* WebSocket 信令监听地址。 */
    uint16_t port;                          /* WebSocket 信令监听端口。 */
    bool running;                           /* WebRtcServer 是否处于运行状态。 */
    uint32_t videoFps;                      /* WebRTC 视频 RTP 时间戳使用的帧率配置。 */
    WebRtcAudioCodec audioCodec;            /* 当前 WebRTC 音频 Track 编码配置。 */
    uint32_t audioSampleRate;               /* 当前音频采样率配置。 */
    uint32_t audioChannels;                 /* 当前音频声道数配置。 */
    uint64_t inputVideoFrames;              /* mediaOutput 输入到 WebRTC server 的视频帧数。 */
    uint64_t inputVideoBytes;               /* mediaOutput 输入到 WebRTC server 的视频字节数。 */
    uint64_t inputAudioFrames;              /* mediaOutput 输入到 WebRTC server 的音频包数。 */
    uint64_t inputAudioBytes;               /* mediaOutput 输入到 WebRTC server 的音频字节数。 */
    uint64_t videoBroadcastTargets;         /* 视频帧累计成功发送到多少个 session。 */
    uint64_t audioBroadcastTargets;         /* 音频包累计成功发送到多少个 session。 */
    uint64_t videoNoReadySession;           /* 输入视频帧没有任何可发送 session 的次数。 */
    uint64_t audioNoReadySession;           /* 输入音频包没有任何可发送 session 的次数。 */
    bool pendingVideoKeyframeRequest;       /* 是否有新就绪 WebRTC 会话正在等待编码器输出 IDR。 */
    bool pendingPliKeyframeRequest;         /* 是否有因 PLI 限频而延迟执行的 IDR 请求。 */
    uint64_t videoKeyframeRequests;         /* 已进入编码器请求队列的 IDR 请求次数，同一时刻多个请求会合并。 */
    uint64_t videoPliReceived;              /* 所有浏览器 session 收到的 PLI/FIR 总次数。 */
    uint64_t videoPliAccepted;              /* 未被限频、可立即触发 IDR 的 PLI/FIR 次数。 */
    uint64_t videoPliDeferred;              /* 因已有或限频 IDR 而合并/延迟的 PLI/FIR 次数。 */
    bool hasLastVideoPliTime;               /* 是否已记录最近一次浏览器 PLI/FIR 到达时刻。 */
    bool hasLastVideoIdrRequestTime;        /* 是否已记录最近一次向上游消费的 IDR 请求时刻。 */
    bool hasLastVideoIdrInputTime;          /* 是否已记录最近一次编码完成并输入 WebRTC 的 IDR 时刻。 */
    uint64_t lastVideoPliAgeMs;             /* 最近一次 PLI/FIR 到当前查询时刻的间隔，单位毫秒。 */
    uint64_t lastVideoIdrRequestAgeMs;      /* 最近一次 IDR 请求到当前查询时刻的间隔，单位毫秒。 */
    uint64_t lastVideoIdrInputAgeMs;        /* 最近一次编码 IDR 输入到当前查询时刻的间隔，单位毫秒。 */
    bool hasLastPliToIdrRequestDelay;       /* 是否已完成一次 PLI/FIR 到 IDR 请求的同链路耗时统计。 */
    bool hasLastIdrRequestToInputDelay;     /* 是否已完成一次 IDR 请求到编码 IDR 输入的同链路耗时统计。 */
    uint64_t lastPliToIdrRequestMs;         /* 最近一次同链路 PLI/FIR 到 IDR 请求的间隔，单位毫秒。 */
    uint64_t lastIdrRequestToInputMs;       /* 最近一次同链路 IDR 请求到编码 IDR 输入的间隔，单位毫秒。 */
    std::vector<WebRtcSessionStats> sessions; /* 当前仍在 WebRtcServer 管理中的会话明细。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
