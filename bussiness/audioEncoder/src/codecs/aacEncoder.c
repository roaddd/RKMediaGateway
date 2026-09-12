#include "aacEncoder.h"

#include "logger.h"

#include <stdlib.h>
#include <string.h>

#include <aacenc_lib.h>

#define AAC_DEFAULT_SAMPLE_RATE 8000
#define AAC_DEFAULT_CHANNELS 1
#define AAC_DEFAULT_BITRATE 32000
#define AAC_DEFAULT_OBJECT_TYPE AUDIO_ENCODER_AAC_OBJECT_TYPE_LC
#define AAC_DEFAULT_MAX_SAMPLES 1024
#define AAC_DEFAULT_OUT_CAPACITY 4096

/**
 * @description: 归一化 AAC 配置，为调用方未填写或填写为非正数的字段补齐默认值。
 * @param dst 输出的完整配置，函数返回后每个字段都有明确值。
 * @param src 调用方配置；允许为 NULL，此时全部使用默认值。
 */
static void normalize_aac_config(AacEncoderConfig *dst, const AacEncoderConfig *src)
{
    /* 先清零再复制，保证 src 为空时也不会残留未初始化数据。 */
    memset(dst, 0, sizeof(*dst));
    if (src)
        *dst = *src;

    /* 每个字段独立补默认值，调用方只需要设置关心的编码参数。 */
    if (dst->sample_rate <= 0)
        dst->sample_rate = AAC_DEFAULT_SAMPLE_RATE;
    if (dst->channels <= 0)
        dst->channels = AAC_DEFAULT_CHANNELS;
    if (dst->bitrate <= 0)
        dst->bitrate = AAC_DEFAULT_BITRATE;
    if (dst->object_type == AUDIO_ENCODER_AAC_OBJECT_TYPE_INVALID)
        dst->object_type = AAC_DEFAULT_OBJECT_TYPE;
    if (dst->max_samples_per_frame <= 0)
        dst->max_samples_per_frame = AAC_DEFAULT_MAX_SAMPLES;
}

/**
 * @description: 将业务层声道数转换成 FDK-AAC 的声道模式。
 * @param channels 已校验的声道数，只允许 1 或 2。
 * @return 单声道返回 MODE_1，双声道返回 MODE_2。
 */
static CHANNEL_MODE aac_channel_mode(int channels)
{
    return (channels == 2) ? MODE_2 : MODE_1;
}

/**
 * @description: 校验项目枚举是否属于当前 AAC 编码模块支持的 Audio Object Type。
 */
static int aac_object_type_is_supported(AudioEncoderAacObjectType object_type)
{
    return object_type == AUDIO_ENCODER_AAC_OBJECT_TYPE_LC ||
           object_type == AUDIO_ENCODER_AAC_OBJECT_TYPE_HE ||
           object_type == AUDIO_ENCODER_AAC_OBJECT_TYPE_LD ||
           object_type == AUDIO_ENCODER_AAC_OBJECT_TYPE_HE_V2 ||
           object_type == AUDIO_ENCODER_AAC_OBJECT_TYPE_ELD;
}

/**
 * @description: 统一检查 FDK-AAC 初始化阶段的返回值并记录失败步骤。
 * @param step 当前执行的 FDK-AAC 操作名称，用于定位初始化失败位置。
 * @param err FDK-AAC 返回的错误码。
 * @return AACENC_OK 返回 0，否则打印错误并返回 -1。
 */
static int aac_check_fdk(const char *step, AACENC_ERROR err)
{
    if (err != AACENC_OK)
    {
        LOG_ERROR("aac_encoder_init failed: %s err=0x%x", step, (unsigned)err);
        return -1;
    }
    return 0;
}

/**
 * @description: 创建并配置 FDK-AAC 编码器，准备可复用的编码输出缓冲区。
 */
int aac_encoder_init(AacEncoderCtx *ctx, const AacEncoderConfig *config)
{
    AacEncoderConfig normalized = {0};
    HANDLE_AACENCODER handle = NULL;
    AACENC_InfoStruct info = {0};

    /* 1. 校验上下文并归一化调用方配置。 */
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
    if (!aac_object_type_is_supported(normalized.object_type))
    {
        LOG_ERROR("aac_encoder_init failed: unsupported object_type=%d", (int)normalized.object_type);
        return -1;
    }

    /* 2. 创建指定声道数的 FDK-AAC 编码器实例，并立即交由 ctx 管理失败清理。 */
    if (aac_check_fdk("aacEncOpen", aacEncOpen(&handle, 0, (UINT)normalized.channels)) != 0)
        return -1;
    ctx->handle = handle;

    /*
     * 3. 配置编码格式和码流参数。
     * RTSP 使用 MPEG4-GENERIC/RFC3640 发送 AAC，RTP 负载应是 raw AAC access unit，
     * 因此选择 TT_MP4_RAW，不附加 ADTS 头；SDP config 由输出模块根据
     * object_type/sample_rate/channels 生成。
     */
    if (aac_check_fdk("AACENC_AOT", aacEncoder_SetParam(handle, AACENC_AOT, (UINT)normalized.object_type)) != 0 ||
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

    /* 4. 查询真实帧长和最大输出包大小，避免使用固定缓冲区猜测编码结果上限。 */
    if (aac_check_fdk("aacEncInfo", aacEncInfo(handle, &info)) != 0)
    {
        aac_encoder_deinit(ctx);
        return -1;
    }

    /* 5. 输出缓冲区由编码器上下文长期持有，在编码热路径中反复复用。 */
    ctx->out_capacity = (info.maxOutBufBytes > 0) ? (size_t)info.maxOutBufBytes : AAC_DEFAULT_OUT_CAPACITY;
    ctx->out_buffer = (uint8_t *)malloc(ctx->out_capacity);
    if (!ctx->out_buffer)
    {
        LOG_ERROR("aac_encoder_init failed: output buffer alloc size=%zu", ctx->out_capacity);
        aac_encoder_deinit(ctx);
        return -1;
    }

    /* 6. 所有资源准备完成后再置 initialized，避免半初始化上下文被误用。 */
    ctx->config = normalized;
    ctx->encoder_frame_samples = (info.frameLength > 0) ? (int)info.frameLength : 1024;
    ctx->initialized = 1;
    LOG_INFO("aac encoder init success: rate=%d channels=%d bitrate=%d object_type=%d frame_samples=%d",
             ctx->config.sample_rate,
             ctx->config.channels,
             ctx->config.bitrate,
             (int)ctx->config.object_type,
             ctx->encoder_frame_samples);
    return 0;
}

/**
 * @description: 将一批交错排列的 S16LE PCM 送入 FDK-AAC，并返回一个 raw AAC access unit。
 */
int aac_encoder_encode_s16le(AacEncoderCtx *ctx,
                             const int16_t *pcm,
                             int samples_per_channel,
                             uint64_t pts_us,
                             const uint8_t **out_data,
                             size_t *out_size,
                             uint64_t *out_pts_us,
                             MediaCodecType *out_codec)
{
    AACENC_BufDesc in_desc = {0};
    AACENC_BufDesc out_desc = {0};
    AACENC_InArgs in_args = {0};
    AACENC_OutArgs out_args = {0};
    AACENC_ERROR err = AACENC_OK;
    void *in_ptr = NULL;
    void *out_ptr = NULL;
    INT in_id = IN_AUDIO_DATA;
    INT out_id = OUT_BITSTREAM_DATA;
    INT in_size = 0;
    INT in_elem_size = (INT)sizeof(int16_t);
    INT out_size_int = 0;
    INT out_elem_size = 1;

    /* 1. 校验上下文状态、PCM 输入以及所有输出参数。 */
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

    /*
     * 2. 先把输出设置成“本次尚未产生编码包”。FDK 内部累计样本不足一帧时，
     * 函数会成功返回，但 out_size 保持为 0，调用方不应把它当作编码失败。
     */
    *out_data = NULL;
    *out_size = 0;
    *out_pts_us = pts_us;
    *out_codec = MEDIA_CODEC_AAC;
    if (!ctx->has_pending_pts)
    {
        ctx->pending_pts_us = pts_us;
        ctx->has_pending_pts = 1;
    }

    /*
     * 3. 描述输入 PCM。numInSamples 是所有声道的样本总数；双声道场景下，
     * 一帧左右声道样本在内存中按 L、R、L、R 顺序交错排列。
     */
    in_size = samples_per_channel * ctx->config.channels * (int)sizeof(int16_t);
    in_ptr = (void *)pcm;
    in_desc.numBufs = 1;
    in_desc.bufs = &in_ptr;
    in_desc.bufferIdentifiers = &in_id;
    in_desc.bufSizes = &in_size;
    in_desc.bufElSizes = &in_elem_size;
    in_args.numInSamples = samples_per_channel * ctx->config.channels;

    /* 4. 描述可复用的输出缓冲区，FDK 通过 out_args 返回实际编码字节数。 */
    out_size_int = (INT)ctx->out_capacity;
    out_ptr = ctx->out_buffer;
    out_desc.numBufs = 1;
    out_desc.bufs = &out_ptr;
    out_desc.bufferIdentifiers = &out_id;
    out_desc.bufSizes = &out_size_int;
    out_desc.bufElSizes = &out_elem_size;

    /*
     * 5. 执行一次编码。out_args.numInSamples 表示 FDK 本次真正消费的样本数，
     * 它可能小于 in_args.numInSamples。当前接口尚未保存并重送未消费的 PCM，
     * 这是后续修复 AAC 输入连续性时必须处理的关键字段。
     */
    err = aacEncEncode((HANDLE_AACENCODER)ctx->handle, &in_desc, &out_desc, &in_args, &out_args);
    if (err != AACENC_OK)
    {
        LOG_ERROR("aac_encoder_encode_s16le failed: aacEncEncode err=0x%x", (unsigned)err);
        return -1;
    }

    /* 6. 编码器尚未凑齐一个 AAC access unit 时，返回成功但不产生输出。 */
    if (out_args.numOutBytes <= 0)
        return 0;

    /*
     * 7. 输出缓冲区由 ctx 持有，数据在下一次编码前有效。AAC PTS 每产生一个包，
     * 按编码器真实帧长递增，而不是按 ALSA period 大小递增。
     */
    *out_data = ctx->out_buffer;
    *out_size = (size_t)out_args.numOutBytes;
    *out_pts_us = ctx->pending_pts_us;
    ctx->pending_pts_us += (uint64_t)ctx->encoder_frame_samples * 1000000ULL / (uint64_t)ctx->config.sample_rate;
    return 0;
}

/**
 * @description: 关闭 FDK-AAC 句柄、释放输出缓冲区并清空上下文。
 */
void aac_encoder_deinit(AacEncoderCtx *ctx)
{
    HANDLE_AACENCODER handle = NULL;

    if (!ctx)
        return;

    /* FDK 通过句柄地址关闭实例，因此使用局部强类型句柄接收不透明指针。 */
    if (ctx->handle)
    {
        handle = (HANDLE_AACENCODER)ctx->handle;
        aacEncClose(&handle);
    }

    /* free(NULL) 安全；最后清零可避免释放后的句柄和 initialized 状态被继续使用。 */
    free(ctx->out_buffer);
    memset(ctx, 0, sizeof(*ctx));
}
