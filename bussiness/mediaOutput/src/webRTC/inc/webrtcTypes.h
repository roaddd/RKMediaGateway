#ifndef __WEBRTC_TYPES_H__
#define __WEBRTC_TYPES_H__

#include <stddef.h>
#include <stdint.h>
#include <string>

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

} // namespace webrtc
} // namespace rkmedia

#endif
