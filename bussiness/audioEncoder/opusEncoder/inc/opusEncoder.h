#ifndef __OPUS_ENCODER_H__
#define __OPUS_ENCODER_H__

#include <stddef.h>
#include <stdint.h>

#include "mediaPacket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;       /* Opus 支持 8/12/16/24/48 kHz，WebRTC 推荐 48 kHz。 */
    int channels;          /* 1=mono，2=stereo。 */
    int bitrate;           /* 目标码率，bit/s。 */
    int complexity;        /* 编码复杂度 0..10。 */
    int enable_vbr;        /* 是否启用可变码率。 */
    int enable_fec;        /* 是否启用带内 FEC。 */
    int enable_dtx;        /* 是否启用 DTX。 */
    int packet_loss_percent; /* 预期丢包率 0..100，供 FEC 决策使用。 */
    int application;       /* OPUS_APPLICATION_*；0 时默认 VOIP。 */
    int max_packet_bytes;  /* 编码输出缓冲容量。 */
} OpusEncoderConfig;

typedef struct {
    OpusEncoderConfig config;
    void *handle;
    int initialized;
} OpusEncoderCtx;

/**
 * @brief 创建并配置一个 Opus 编码器实例。
 * @param ctx C 调用方持有的编码器上下文。
 * @param config 编码参数；允许为 NULL，此时使用 WebRTC 语音默认值。
 * @return 0 成功，-1 参数或 libopus 初始化失败。
 */
int opus_audio_encoder_init(OpusEncoderCtx *ctx, const OpusEncoderConfig *config);

/**
 * @brief 将一帧交错排列的 PCM S16LE 编码成一个裸 Opus packet。
 * @note samples_per_channel 必须对应 2.5/5/10/20/40/60ms；返回数据由实例持有，
 *       下一次 encode 或 deinit 前有效，调用方应及时复制到 MediaBuffer。
 */
int opus_audio_encoder_encode_s16le(OpusEncoderCtx *ctx,
                                    const int16_t *pcm,
                                    int samples_per_channel,
                                    const uint8_t **out_data,
                                    size_t *out_size,
                                    MediaCodecType *out_codec);

/** @brief 销毁 C++ Opus 编码器对象并清空上下文。 */
void opus_audio_encoder_deinit(OpusEncoderCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
