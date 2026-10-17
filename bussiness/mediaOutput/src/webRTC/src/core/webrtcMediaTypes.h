/**
 * @file webrtcMediaTypes.h
 * @brief 定义 WebRTC 核心模块交换的音视频帧、入站 RTP 包和相关枚举。
 */
#ifndef __WEBRTC_MEDIA_TYPES_H__
#define __WEBRTC_MEDIA_TYPES_H__

#include <cstddef>
#include <cstdint>
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
    WEBRTC_AUDIO_CODEC_PCMU = 2, /* G711 mu-law，对应 SDP 中的 PCMU/8000。 */
    WEBRTC_AUDIO_CODEC_OPUS = 3  /* Opus，对应 SDP 中的 opus/48000/2。 */
};

enum WebRtcKeyframeRequestReason {
    WEBRTC_KEYFRAME_REQUEST_NEW_SESSION = 0, /* 新 video Track 打开，需要从 IDR 开始解码。 */
    WEBRTC_KEYFRAME_REQUEST_PLI = 1, /* 浏览器通过 RTCP PLI/FIR 请求关键帧。 */
    WEBRTC_KEYFRAME_REQUEST_TEST_TIMEOUT = 2 /* PLI 测试超时后请求恢复用 IDR。 */
};

struct WebRtcVideoFrame {
    const uint8_t *data; /* 编码帧数据地址。 */
    size_t size; /* 编码帧字节数。 */
    uint64_t ptsUs; /* 媒体时间戳，单位微秒。 */
    bool keyFrame; /* 是否为关键帧。 */
    H264FrameFormat format; /* H264 输入格式。 */
};

struct WebRtcAudioFrame {
    const uint8_t *data; /* 编码后的 G711 或裸 Opus packet。 */
    size_t size; /* 编码音频字节数。 */
    uint64_t ptsUs; /* 媒体时间戳，单位微秒。 */
    WebRtcAudioCodec codec; /* 必须与协商出的音频 Track 编码一致。 */
    uint64_t frameId; /* gateway 音频帧序号，用于链路诊断。 */
    bool traceSample; /* 是否记录本帧发送耗时。 */
};

/** @brief 从浏览器收到并完成 RTP 头解析的单个音频包。 */
struct WebRtcIncomingAudioPacket {
    std::string sessionId; /* 云端分配的唯一会话 ID。 */
    WebRtcAudioCodec codec; /* SDP 已协商的音频编码。 */
    uint8_t payloadType; /* RTP Payload Type。 */
    uint32_t ssrc; /* 浏览器音频发送源 SSRC。 */
    uint16_t sequenceNumber; /* RTP 序号。 */
    uint32_t rtpTimestamp; /* RTP 媒体时间戳。 */
    bool marker; /* RTP Marker 位。 */
    uint64_t arrivalTimeUs; /* 设备接收时的单调时间，单位微秒。 */
    std::vector<uint8_t> payload; /* 去除 RTP 头、扩展和 padding 后的独立负载。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
