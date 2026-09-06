#include "g711Encoder.h"

#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G711_DEFAULT_SAMPLE_RATE 8000
#define G711_DEFAULT_CHANNELS 1
#define G711_DEFAULT_MAX_SAMPLES 320

/* PCM 线性采样转 G.711 A-law。实现为纯 CPU 查算，避免引入额外编解码库依赖。 */
static uint8_t linear_to_alaw(int pcm_val) {
    int mask;
    int seg;
    uint8_t aval;
    static const int seg_end[8] = {
        0x000000FF, 0x000001FF, 0x000003FF, 0x000007FF,
        0x00000FFF, 0x00001FFF, 0x00003FFF, 0x00007FFF
    };

    pcm_val >>= 3;
    if (pcm_val >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        pcm_val = -pcm_val - 1;
    }

    for (seg = 0; seg < 8; ++seg) {
        if (pcm_val <= seg_end[seg]) {
            break;
        }
    }

    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);
    }
    aval = (uint8_t)(seg << 4);
    if (seg < 2) {
        aval |= (uint8_t)((pcm_val >> 1) & 0x0F);
    } else {
        aval |= (uint8_t)((pcm_val >> seg) & 0x0F);
    }
    return (uint8_t)(aval ^ mask);
}

/* PCM 线性采样转 G.711 mu-law。 */
static uint8_t linear_to_ulaw(int pcm_val) {
    int mask;
    int seg;
    uint8_t uval;
    static const int seg_end[8] = {
        0x0000003F, 0x0000007F, 0x000000FF, 0x000001FF,
        0x000003FF, 0x000007FF, 0x00000FFF, 0x00001FFF
    };

    pcm_val >>= 2;
    if (pcm_val < 0) {
        pcm_val = -pcm_val;
        mask = 0x7F;
    } else {
        mask = 0xFF;
    }
    if (pcm_val > 8159) {
        pcm_val = 8159;
    }
    pcm_val += 33;

    for (seg = 0; seg < 8; ++seg) {
        if (pcm_val <= seg_end[seg]) {
            break;
        }
    }
    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);
    }
    uval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 1)) & 0x0F));
    return (uint8_t)(uval ^ mask);
}

/*
 * 初始化 G711 编码器。
 * G711 是逐采样无状态编码，这里主要负责参数校验和输出缓冲预分配。
 */
int g711_encoder_init(G711EncoderCtx *ctx, const G711EncoderConfig *config) {
    G711EncoderConfig normalized;

    if (!ctx) {
        LOG_ERROR("g711_encoder_init failed: ctx is NULL");
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(&normalized, 0, sizeof(normalized));
    if (config) {
        normalized = *config;
    }
    if (normalized.sample_rate <= 0) normalized.sample_rate = G711_DEFAULT_SAMPLE_RATE;
    if (normalized.channels <= 0) normalized.channels = G711_DEFAULT_CHANNELS;
    if (normalized.max_samples_per_frame <= 0) normalized.max_samples_per_frame = G711_DEFAULT_MAX_SAMPLES;
    if (normalized.channels != 1) {
        LOG_ERROR("g711_encoder_init failed: only mono input is supported channels=%d", normalized.channels);
        return -1;
    }

    ctx->out_capacity = (size_t)normalized.max_samples_per_frame;
    ctx->out_buffer = (uint8_t *)malloc(ctx->out_capacity);
    if (!ctx->out_buffer) {
        LOG_ERROR("g711_encoder_init failed: output buffer alloc size=%zu", ctx->out_capacity);
        return -1;
    }
    ctx->config = normalized;
    ctx->initialized = 1;
    return 0;
}

/* 编码一帧 mono PCM S16LE。输出数据指向 ctx->out_buffer，下次编码前有效。 */
int g711_encoder_encode_s16le(G711EncoderCtx *ctx,
                              const int16_t *pcm,
                              int samples,
                              const uint8_t **out_data,
                              size_t *out_size,
                              MediaCodecType *out_codec) {
    int i;

    if (!ctx || !ctx->initialized || !pcm || samples <= 0 || !out_data || !out_size || !out_codec) {
        LOG_ERROR("g711_encoder_encode_s16le failed: invalid args ctx=%p initialized=%d pcm=%p samples=%d out_data=%p out_size=%p out_codec=%p",
                  (void *)ctx,
                  ctx ? ctx->initialized : 0,
                  (const void *)pcm,
                  samples,
                  (void *)out_data,
                  (void *)out_size,
                  (void *)out_codec);
        return -1;
    }
    if ((size_t)samples > ctx->out_capacity) {
        LOG_ERROR("g711_encoder_encode_s16le failed: samples exceed capacity samples=%d capacity=%zu",
                  samples,
                  ctx->out_capacity);
        return -1;
    }

    /* G711 每个 16bit PCM 采样压缩为 1 字节，码率固定为 sample_rate * 8。 */
    if (ctx->config.mode == G711_ENCODER_MODE_ULAW) {
        for (i = 0; i < samples; ++i) {
            ctx->out_buffer[i] = linear_to_ulaw(pcm[i]);
        }
        *out_codec = MEDIA_CODEC_G711U;
    } else {
        for (i = 0; i < samples; ++i) {
            ctx->out_buffer[i] = linear_to_alaw(pcm[i]);
        }
        *out_codec = MEDIA_CODEC_G711A;
    }

    *out_data = ctx->out_buffer;
    *out_size = (size_t)samples;
    return 0;
}

/* 释放预分配输出缓冲。 */
void g711_encoder_deinit(G711EncoderCtx *ctx) {
    if (!ctx) {
        return;
    }
    free(ctx->out_buffer);
    memset(ctx, 0, sizeof(*ctx));
}
