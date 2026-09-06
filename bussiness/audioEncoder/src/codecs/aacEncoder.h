#ifndef __AAC_ENCODER_H__
#define __AAC_ENCODER_H__

#include <stddef.h>
#include <stdint.h>

#include "mediaPacket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;             /* 输入 PCM 采样率。 */
    int channels;                /* 输入声道数，支持 mono/stereo。 */
    int bitrate;                 /* AAC 目标码率，单位 bit/s。 */
    int profile;                 /* AAC object type，2 表示 AAC-LC。 */
    int max_samples_per_frame;   /* 输入侧单次喂入的每声道采样数，用于校验。 */
} AacEncoderConfig;

typedef struct {
    AacEncoderConfig config;     /* 归一化后的编码配置。 */
    void *handle;                /* 具体 AAC 后端句柄，当前支持 FDK-AAC。 */
    uint8_t *out_buffer;         /* 热路径复用输出缓冲。 */
    size_t out_capacity;         /* out_buffer 容量。 */
    int encoder_frame_samples;   /* AAC 编码器每帧每声道采样数，AAC-LC 通常为 1024。 */
    uint64_t pending_pts_us;     /* FDK 内部缓存样本对应的首个 PTS。 */
    int has_pending_pts;         /* pending_pts_us 是否有效。 */
    int initialized;             /* 编码器是否已初始化。 */
} AacEncoderCtx;

/**
 * @description: 初始化 AAC 编码器。当前真实编码后端为 FDK-AAC，输出 raw AAC access unit，供 RTSP MPEG4-GENERIC 发送。
 */
int aac_encoder_init(AacEncoderCtx *ctx, const AacEncoderConfig *config);

/**
 * @description: 输入一帧 PCM S16LE，编码为 AAC。FDK 会缓存不足 1024 samples 的输入，可能返回 out_size=0。
 */
int aac_encoder_encode_s16le(AacEncoderCtx *ctx,
                             const int16_t *pcm,
                             int samples_per_channel,
                             uint64_t pts_us,
                             const uint8_t **out_data,
                             size_t *out_size,
                             uint64_t *out_pts_us,
                             MediaCodecType *out_codec);

/**
 * @description: 释放 AAC 编码器资源。
 */
void aac_encoder_deinit(AacEncoderCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
