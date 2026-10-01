/**
 * @file audioTalkbackModule.h
 * @brief WebRTC 入站语音抖动缓冲、解码和播放模块的 C 接口。
 */

#ifndef __AUDIO_TALKBACK_MODULE_H__
#define __AUDIO_TALKBACK_MODULE_H__

#include "commonDef.h"
#include "mediaPacket.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ALSA Playback 路由配置。 */
typedef struct {
    int enabled;              /* 是否在打开 PCM 前自动设置 Playback mixer 路由。 */
    const char *card_name;    /* ALSA 控制卡名称，例如 hw:0。 */
    const char *control_name; /* 枚举控件名称，例如 Playback Path。 */
    const char *value_name;   /* 目标枚举值，例如 HP 或 SPK。 */
} AudioTalkbackMixerConfig;

/** @brief 双向语音下行链路配置，所有字段必须由调用方显式填写。 */
typedef struct {
    const char *playback_device;       /* ALSA Playback PCM 设备，例如 hw:0,0。 */
    int sample_rate;                   /* Opus 解码及播放采样率，当前必须为 48000 Hz。 */
    int decoder_channels;              /* Opus 解码输出声道数，当前双向语音使用单声道。 */
    int playback_channels;             /* ALSA 硬件播放声道数，RK809 数字侧使用双声道。 */
    int frame_samples_per_channel;     /* 每个 20 ms 音频帧每声道采样数，48 kHz 时为 960。 */
    int playback_buffer_periods;       /* ALSA 环形缓冲区包含的 period 数。 */
    int playback_start_periods;        /* ALSA 开始播放前至少累计的 period 数。 */
    int jitter_prebuffer_packets;      /* 开始播放前预缓存的连续时序窗口包数。 */
    int jitter_max_packets;            /* 抖动缓冲允许保存的最大 RTP 包数。 */
    int talker_timeout_ms;             /* 活动说话人无新包后允许切换或重置的超时时间。 */
    AudioTalkbackMixerConfig mixer;    /* Playback mixer 自动路由配置。 */
} AudioTalkbackConfig;

/** @brief WebRTC 层提交给双向语音模块的一包 Opus RTP 负载。 */
typedef struct {
    int session_id;             /* WebRTC 服务端分配的会话 ID。 */
    MediaCodecType codec;       /* RTP 负载编码，当前只接受 MEDIA_CODEC_OPUS。 */
    uint8_t payload_type;       /* SDP 协商得到的 RTP Payload Type。 */
    uint32_t ssrc;              /* 浏览器音频发送源 SSRC。 */
    uint16_t sequence_number;   /* RTP 16 位序号，用于排序、重复包及丢包判断。 */
    uint32_t rtp_timestamp;     /* 48 kHz RTP 时钟时间戳。 */
    uint64_t arrival_time_us;   /* 包到达设备时的单调时钟时间，单位微秒。 */
    const uint8_t *payload;     /* 已去除 RTP 头、扩展头和 padding 的 Opus 数据。 */
    size_t payload_size;        /* Opus 数据字节数。 */
} AudioTalkbackPacket;

/** @brief 双向语音模块累计运行统计。 */
typedef struct {
    uint64_t submitted_packets;       /* WebRTC 成功提交的有效 Opus 包数。 */
    uint64_t decoded_packets;         /* 完成普通解码并写入播放设备的包数。 */
    uint64_t fec_recovered_frames;    /* 使用后续包带内 FEC 恢复的丢失帧数。 */
    uint64_t plc_concealed_frames;    /* 无可用 FEC 时由 PLC 补偿的丢失帧数。 */
    uint64_t duplicate_packets;       /* 因 RTP 序号重复而丢弃的包数。 */
    uint64_t late_packets;            /* 晚于当前播放点到达而丢弃的包数。 */
    uint64_t overflow_drops;          /* 抖动缓冲达到容量上限时丢弃的包数。 */
    uint64_t foreign_talker_drops;    /* 活动说话人尚未超时时丢弃的其他会话包数。 */
    uint64_t decode_errors;           /* 解码、FEC/PLC 或输出帧校验失败次数。 */
    uint64_t playback_errors;         /* ALSA 写入失败次数。 */
    uint64_t stream_resets;           /* SSRC 切换、说话人切换或超时触发的流重置次数。 */
} AudioTalkbackStats;

/**
 * @description: 校验配置并初始化进程内唯一的双向语音实例。
 * @param config 双向语音完整配置；所有字段必须由调用方显式填写。
 * @return MEDIA_OK 初始化并启动成功；已初始化时返回 MEDIA_ERR_BUSY。
 */
MediaResult audio_talkback_init(const AudioTalkbackConfig *config);

/**
 * @description: 向内部唯一实例复制并提交一包 Opus 负载。
 * @param packet 入站 RTP 元数据和 Opus 负载；返回后调用方可立即释放原始内存。
 * @return MEDIA_OK 提交成功；模块未初始化时返回 MEDIA_ERR_NOT_READY。
 */
MediaResult audio_talkback_submit(const AudioTalkbackPacket *packet);

/**
 * @description: 获取进程内唯一双向语音实例的累计统计快照。
 * @return MEDIA_OK 获取成功；模块未初始化时返回 MEDIA_ERR_NOT_READY。
 */
MediaResult audio_talkback_get_stats(AudioTalkbackStats *stats);

/** @description: 停止并销毁内部唯一实例；未初始化时直接返回成功。 */
MediaResult audio_talkback_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_TALKBACK_MODULE_H__ */
