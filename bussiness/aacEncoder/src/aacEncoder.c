#include "aacEncoder.h"

#include "logger.h"

#include <stdlib.h>
#include <string.h>

#if defined(ENABLE_FDK_AAC)
#include <aacenc_lib.h>
#endif

#define AAC_DEFAULT_SAMPLE_RATE 8000
#define AAC_DEFAULT_CHANNELS 1
#define AAC_DEFAULT_BITRATE 32000
#define AAC_DEFAULT_PROFILE 2
#define AAC_DEFAULT_MAX_SAMPLES 1024
#define AAC_DEFAULT_OUT_CAPACITY 4096

static void normalize_aac_config(AacEncoderConfig *dst, const AacEncoderConfig *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src)
        *dst = *src;
    if (dst->sample_rate <= 0)
        dst->sample_rate = AAC_DEFAULT_SAMPLE_RATE;
    if (dst->channels <= 0)
        dst->channels = AAC_DEFAULT_CHANNELS;
    if (dst->bitrate <= 0)
        dst->bitrate = AAC_DEFAULT_BITRATE;
    if (dst->profile <= 0)
        dst->profile = AAC_DEFAULT_PROFILE;
    if (dst->max_samples_per_frame <= 0)
        dst->max_samples_per_frame = AAC_DEFAULT_MAX_SAMPLES;
}

#if defined(ENABLE_FDK_AAC)
static CHANNEL_MODE aac_channel_mode(int channels)
{
    return (channels == 2) ? MODE_2 : MODE_1;
}

static int aac_check_fdk(const char *step, AACENC_ERROR err)
{
    if (err != AACENC_OK)
    {
        LOG_ERROR("aac_encoder_init failed: %s err=0x%x", step, (unsigned)err);
        return -1;
    }
    return 0;
}
#endif

int aac_encoder_init(AacEncoderCtx *ctx, const AacEncoderConfig *config)
{
    AacEncoderConfig normalized;

    if (!ctx)
    {
        LOG_ERROR("aac_encoder_init failed: ctx is NULL");
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    normalize_aac_config(&normalized, config);

    if (normalized.channels != 1 && normalized.channels != 2)
    {
        LOG_ERROR("aac_encoder_init failed: unsupported channels=%d", normalized.channels);
        return -1;
    }

#if defined(ENABLE_FDK_AAC)
    {
        HANDLE_AACENCODER handle = NULL;
        AACENC_InfoStruct info;

        if (aac_check_fdk("aacEncOpen", aacEncOpen(&handle, 0, (UINT)normalized.channels)) != 0)
            return -1;
        ctx->handle = handle;

        /*
         * RTSP server 使用 MPEG4-GENERIC/RFC3640 发送 AAC，RTP 负载应是 raw AAC access unit。
         * 因此这里使用 TT_MP4_RAW，不额外封装 ADTS 头；SDP 的 config 由 sessionAddAudio
         * 根据 profile/sample_rate/channels 生成。
         */
        if (aac_check_fdk("AACENC_AOT", aacEncoder_SetParam(handle, AACENC_AOT, (UINT)normalized.profile)) != 0 ||
            aac_check_fdk("AACENC_SAMPLERATE", aacEncoder_SetParam(handle, AACENC_SAMPLERATE, (UINT)normalized.sample_rate)) != 0 ||
            aac_check_fdk("AACENC_CHANNELMODE", aacEncoder_SetParam(handle, AACENC_CHANNELMODE, (UINT)aac_channel_mode(normalized.channels))) != 0 ||
            aac_check_fdk("AACENC_CHANNELORDER", aacEncoder_SetParam(handle, AACENC_CHANNELORDER, 1)) != 0 ||
            aac_check_fdk("AACENC_BITRATE", aacEncoder_SetParam(handle, AACENC_BITRATE, (UINT)normalized.bitrate)) != 0 ||
            aac_check_fdk("AACENC_TRANSMUX", aacEncoder_SetParam(handle, AACENC_TRANSMUX, TT_MP4_RAW)) != 0 ||
            aac_check_fdk("aacEncEncode init", aacEncEncode(handle, NULL, NULL, NULL, NULL)) != 0)
        {
            aac_encoder_deinit(ctx);
            return -1;
        }

        memset(&info, 0, sizeof(info));
        if (aac_check_fdk("aacEncInfo", aacEncInfo(handle, &info)) != 0)
        {
            aac_encoder_deinit(ctx);
            return -1;
        }

        ctx->out_capacity = (info.maxOutBufBytes > 0) ? (size_t)info.maxOutBufBytes : AAC_DEFAULT_OUT_CAPACITY;
        ctx->out_buffer = (uint8_t *)malloc(ctx->out_capacity);
        if (!ctx->out_buffer)
        {
            LOG_ERROR("aac_encoder_init failed: output buffer alloc size=%zu", ctx->out_capacity);
            aac_encoder_deinit(ctx);
            return -1;
        }

        ctx->config = normalized;
        ctx->encoder_frame_samples = (info.frameLength > 0) ? (int)info.frameLength : 1024;
        ctx->initialized = 1;
        LOG_INFO("aac encoder init success: rate=%d channels=%d bitrate=%d profile=%d frame_samples=%d",
                 ctx->config.sample_rate,
                 ctx->config.channels,
                 ctx->config.bitrate,
                 ctx->config.profile,
                 ctx->encoder_frame_samples);
        return 0;
    }
#else
    (void)normalized;
    LOG_ERROR("aac_encoder_init failed: FDK-AAC backend is not compiled in");
    return -1;
#endif
}

int aac_encoder_encode_s16le(AacEncoderCtx *ctx,
                             const int16_t *pcm,
                             int samples_per_channel,
                             uint64_t pts_us,
                             const uint8_t **out_data,
                             size_t *out_size,
                             uint64_t *out_pts_us,
                             MediaCodecType *out_codec)
{
    if (!ctx || !ctx->initialized || !pcm || samples_per_channel <= 0 ||
        !out_data || !out_size || !out_pts_us || !out_codec)
    {
        LOG_ERROR("aac_encoder_encode_s16le failed: invalid args ctx=%p initialized=%d pcm=%p samples=%d",
                  (void *)ctx,
                  ctx ? ctx->initialized : 0,
                  (const void *)pcm,
                  samples_per_channel);
        return -1;
    }
    if (samples_per_channel > ctx->config.max_samples_per_frame)
    {
        LOG_ERROR("aac_encoder_encode_s16le failed: samples exceed max samples=%d max=%d",
                  samples_per_channel,
                  ctx->config.max_samples_per_frame);
        return -1;
    }

    *out_data = NULL;
    *out_size = 0;
    *out_pts_us = pts_us;
    *out_codec = MEDIA_CODEC_AAC;
    if (!ctx->has_pending_pts)
    {
        ctx->pending_pts_us = pts_us;
        ctx->has_pending_pts = 1;
    }

#if defined(ENABLE_FDK_AAC)
    {
        AACENC_BufDesc in_desc;
        AACENC_BufDesc out_desc;
        AACENC_InArgs in_args;
        AACENC_OutArgs out_args;
        AACENC_ERROR err;
        void *in_ptr;
        void *out_ptr;
        INT in_id = IN_AUDIO_DATA;
        INT out_id = OUT_BITSTREAM_DATA;
        INT in_size = samples_per_channel * ctx->config.channels * (int)sizeof(int16_t);
        INT in_elem_size = (INT)sizeof(int16_t);
        INT out_size_int = (INT)ctx->out_capacity;
        INT out_elem_size = 1;

        memset(&in_desc, 0, sizeof(in_desc));
        memset(&out_desc, 0, sizeof(out_desc));
        memset(&in_args, 0, sizeof(in_args));
        memset(&out_args, 0, sizeof(out_args));

        in_ptr = (void *)pcm;
        in_desc.numBufs = 1;
        in_desc.bufs = &in_ptr;
        in_desc.bufferIdentifiers = &in_id;
        in_desc.bufSizes = &in_size;
        in_desc.bufElSizes = &in_elem_size;
        in_args.numInSamples = samples_per_channel * ctx->config.channels;

        out_ptr = ctx->out_buffer;
        out_desc.numBufs = 1;
        out_desc.bufs = &out_ptr;
        out_desc.bufferIdentifiers = &out_id;
        out_desc.bufSizes = &out_size_int;
        out_desc.bufElSizes = &out_elem_size;

        err = aacEncEncode((HANDLE_AACENCODER)ctx->handle, &in_desc, &out_desc, &in_args, &out_args);
        if (err != AACENC_OK)
        {
            LOG_ERROR("aac_encoder_encode_s16le failed: aacEncEncode err=0x%x", (unsigned)err);
            return -1;
        }

        if (out_args.numOutBytes <= 0)
            return 0;

        *out_data = ctx->out_buffer;
        *out_size = (size_t)out_args.numOutBytes;
        *out_pts_us = ctx->pending_pts_us;
        ctx->pending_pts_us += (uint64_t)ctx->encoder_frame_samples * 1000000ULL / (uint64_t)ctx->config.sample_rate;
        return 0;
    }
#else
    (void)pcm;
    (void)samples_per_channel;
    LOG_ERROR("aac_encoder_encode_s16le failed: FDK-AAC backend is not compiled in");
    return -1;
#endif
}

void aac_encoder_deinit(AacEncoderCtx *ctx)
{
    if (!ctx)
        return;
#if defined(ENABLE_FDK_AAC)
    if (ctx->handle)
    {
        HANDLE_AACENCODER handle = (HANDLE_AACENCODER)ctx->handle;
        aacEncClose(&handle);
    }
#endif
    free(ctx->out_buffer);
    memset(ctx, 0, sizeof(*ctx));
}
