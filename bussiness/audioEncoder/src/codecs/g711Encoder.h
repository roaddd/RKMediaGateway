#ifndef __G711_ENCODER_H__
#define __G711_ENCODER_H__

#include <stddef.h>
#include <stdint.h>

#include "mediaPacket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    G711_ENCODER_MODE_ALAW = 0, /* G.711 A-law，GB28181 国内平台最常见。 */
    G711_ENCODER_MODE_ULAW = 1  /* G.711 mu-law，部分海外/兼容场景使用。 */
} G711EncoderMode;

typedef struct {
    G711EncoderMode mode;       /* 编码律。 */
    int sample_rate;            /* 输入 PCM 采样率。 */
    int channels;               /* 输入声道数，当前只支持 mono。 */
    int max_samples_per_frame;  /* 单帧最大采样数，用于预分配输出缓冲。 */
} G711EncoderConfig;

typedef struct {
    G711EncoderConfig config;   /* 归一化后的编码配置。 */
    uint8_t *out_buffer;        /* 热路径复用输出缓冲，G711 每个采样输出 1 字节。 */
    size_t out_capacity;        /* out_buffer 容量。 */
    int initialized;            /* 编码器是否已初始化。 */
} G711EncoderCtx;

/**
 * @description: 初始化 G711 编码器并预分配输出缓冲。
 */
int g711_encoder_init(G711EncoderCtx *ctx, const G711EncoderConfig *config);

/**
 * @description: 将 mono PCM S16LE 编码为 G711A/G711U。
 */
int g711_encoder_encode_s16le(G711EncoderCtx *ctx,
                              const int16_t *pcm,
                              int samples,
                              const uint8_t **out_data,
                              size_t *out_size,
                              MediaCodecType *out_codec);

/**
 * @description: 释放 G711 编码器资源。
 */
void g711_encoder_deinit(G711EncoderCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
