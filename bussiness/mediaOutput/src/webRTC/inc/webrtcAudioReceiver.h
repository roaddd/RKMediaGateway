#ifndef __WEBRTC_AUDIO_RECEIVER_H__
#define __WEBRTC_AUDIO_RECEIVER_H__

#include "commonDef.h"
#include "webrtcTypes.h"

#include <functional>
#include <mutex>

namespace rkmedia {
namespace webrtc {

/** 入站音频 RTP 接收器的固定协商参数。 */
struct WebRtcAudioReceiverConfig {
    int sessionId = 0;                       /* WebRtcServer 分配的会话 ID。 */
    WebRtcAudioCodec codec = WEBRTC_AUDIO_CODEC_NONE; /* SDP 协商出的编码格式。 */
    uint8_t payloadType = 0;                 /* SDP 协商出的 RTP PT。 */
};

/** 入站音频 RTP 接收与基础序号检查的累计统计。 */
struct WebRtcAudioReceiverStats {
    uint64_t rtpPackets = 0;                 /* 成功解析并交付上层的 RTP 包数。 */
    uint64_t rtpBytes = 0;                   /* 成功接收的完整 RTP 包字节数。 */
    uint64_t payloadBytes = 0;               /* 去除 RTP 头部后的编码负载字节数。 */
    uint64_t malformedPackets = 0;           /* RTP 版本、头长、扩展或 padding 非法的包数。 */
    uint64_t unexpectedPayloadTypePackets = 0; /* PT 与 SDP 协商结果不一致的包数。 */
    uint64_t rtcpPackets = 0;                /* audio Track 回调中收到并忽略的 RTCP 包数。 */
    uint64_t sequenceGapPackets = 0;         /* 按序号跳变估算的缺失 RTP 包数。 */
    uint64_t duplicatePackets = 0;           /* 与最近接收包序号相同的重复包数。 */
    uint64_t outOfOrderPackets = 0;          /* 晚于后续包抵达的乱序包数。 */
    uint64_t ssrcChanges = 0;                /* 会话存续期间浏览器更换音频 SSRC 的次数。 */
    bool hasRtpPacket = false;               /* 是否已经收到至少一个有效 RTP 包。 */
    uint32_t lastSsrc = 0;                   /* 最近有效 RTP 包的 SSRC。 */
    uint16_t lastSequenceNumber = 0;         /* 最近有效 RTP 包的序号。 */
    uint32_t lastRtpTimestamp = 0;           /* 最近有效 RTP 包的媒体时间戳。 */
};

typedef std::function<void(const WebRtcIncomingAudioPacket &)>
    WebRtcIncomingAudioPacketCallback;

/* 实现文件内部使用的 RTP 只读视图；对外只保留不完整类型声明。 */
struct WebRtcAudioRtpPacketView;

/**
 * @brief 把 libdatachannel 交付的裸 RTP/RTCP 字节转换为类型安全的音频包。
 *
 * 类内部只负责 RTP 边界校验、负载复制和轻量序号统计，不负责排序、解码或播放。
 * 因而网络接收线程不会被后续音频处理策略反向耦合。
 */
class WebRtcAudioReceiver {
public:
    /**
     * @brief 创建一个会话级 RTP 音频接收器。
     * @param config 当前 audio m-line 的会话、编码和 PT 配置。
     * @param callback 有效 RTP 包的同步交付回调；可为空，仅做统计。
     */
    WebRtcAudioReceiver(const WebRtcAudioReceiverConfig &config,
                        const WebRtcIncomingAudioPacketCallback &callback);

    /**
     * @brief 替换接收配置和回调，并清空原发送源的序号状态与统计。
     * @return MEDIA_OK 表示成功；配置非法时返回 MEDIA_ERR_INVALID_CONFIG。
     */
    MediaResult reset(const WebRtcAudioReceiverConfig &config,
                      const WebRtcIncomingAudioPacketCallback &callback);

    /**
     * @brief 处理一条解密后的 RTP 或 RTCP 原始消息。
     * @param data 原始包首地址。
     * @param size 原始包总字节数。
     * @param arrivalTimeUs 单调时钟到达时间，单位微秒。
     * @return 有效 RTP 或 RTCP 返回 MEDIA_OK；非法 RTP 或 PT 不匹配返回对应错误码。
     */
    MediaResult handlePacket(const uint8_t *data, size_t size, uint64_t arrivalTimeUs);

    /** @brief 返回当前接收器累计统计的线程安全快照。 */
    void getStats(WebRtcAudioReceiverStats &stats) const;

private:
    /* 记录一个无法形成合法 RTP 视图的输入包。 */
    void recordMalformedPacket();

    /* 记录 audio Track 上收到的 RTCP 控制包。 */
    void recordRtcpPacket();

    /* 校验协商参数、更新序号统计，并构造拥有 payload 所有权的上层音频包。 */
    MediaResult acceptRtpPacket(const WebRtcAudioRtpPacketView &view,
                                size_t packetSize,
                                uint64_t arrivalTimeUs,
                                WebRtcIncomingAudioPacket &packet,
                                WebRtcIncomingAudioPacketCallback &callback);

    WebRtcAudioReceiverConfig config_;
    WebRtcIncomingAudioPacketCallback callback_;
    mutable std::mutex mutex_;
    WebRtcAudioReceiverStats stats_;
    uint16_t expectedSequenceNumber_ = 0;
};

} // namespace webrtc
} // namespace rkmedia

#endif
