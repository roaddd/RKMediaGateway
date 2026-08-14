#include "opusEncoder.h"

#include "logger.h"
#include "opus.h"

#include <new>
#include <vector>

namespace {

const int kDefaultSampleRate = 48000;
const int kDefaultChannels = 1;
const int kDefaultBitrate = 24000;
const int kDefaultComplexity = 6;
const int kDefaultPacketBytes = 4000;

bool isSupportedSampleRate(int rate)
{
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

bool isSupportedFrameSize(int rate, int samples)
{
    return samples == rate / 400 || samples == rate / 200 || samples == rate / 100 ||
           samples == rate / 50 || samples == rate / 25 || samples == 3 * rate / 50;
}

/*
 * C++ 编码核心使用 RAII 管理 libopus handle 和复用输出缓冲。
 * 网关仍由 C 实现，因此文件末尾仅暴露轻量 extern "C" 适配函数。
 */
class OpusAudioEncoder {
public:
    explicit OpusAudioEncoder(const OpusEncoderConfig &config)
        : config_(config), encoder_(NULL), output_(static_cast<size_t>(config.max_packet_bytes))
    {
    }

    ~OpusAudioEncoder()
    {
        if (encoder_)
            opus_encoder_destroy(encoder_);
    }

    bool initialize()
    {
        int error = OPUS_OK;

        encoder_ = opus_encoder_create(config_.sample_rate,
                                       config_.channels,
                                       config_.application,
                                       &error);
        if (!encoder_ || error != OPUS_OK) {
            LOG_ERROR("opus_encoder_create failed: %s", opus_strerror(error));
            return false;
        }
        if (opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(config_.bitrate)) != OPUS_OK ||
            opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(config_.complexity)) != OPUS_OK ||
            opus_encoder_ctl(encoder_, OPUS_SET_VBR(config_.enable_vbr ? 1 : 0)) != OPUS_OK ||
            opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(config_.enable_fec ? 1 : 0)) != OPUS_OK ||
            opus_encoder_ctl(encoder_, OPUS_SET_DTX(config_.enable_dtx ? 1 : 0)) != OPUS_OK ||
            opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(config_.packet_loss_percent)) != OPUS_OK) {
            LOG_ERROR("opus encoder ctl failed");
            return false;
        }
        return true;
    }

    int encode(const int16_t *pcm, int samples, const uint8_t **data, size_t *size)
    {
        int bytes;

        if (!pcm || !data || !size || !isSupportedFrameSize(config_.sample_rate, samples)) {
            LOG_ERROR("opus encode invalid frame: samples=%d rate=%d", samples, config_.sample_rate);
            return -1;
        }
        bytes = opus_encode(encoder_, reinterpret_cast<const opus_int16 *>(pcm), samples,
                            output_.data(), static_cast<opus_int32>(output_.size()));
        if (bytes < 0) {
            LOG_ERROR("opus_encode failed: %s", opus_strerror(bytes));
            return -1;
        }
        *data = output_.data();
        *size = static_cast<size_t>(bytes);
        return 0;
    }

private:
    OpusEncoderConfig config_;
    OpusEncoder *encoder_;
    std::vector<uint8_t> output_;
};

OpusEncoderConfig normalizeConfig(const OpusEncoderConfig *config)
{
    OpusEncoderConfig result = {};

    if (config) {
        result = *config;
    } else {
        result.complexity = kDefaultComplexity;
    }
    if (result.sample_rate <= 0) result.sample_rate = kDefaultSampleRate;
    if (result.channels <= 0) result.channels = kDefaultChannels;
    if (result.bitrate <= 0) result.bitrate = kDefaultBitrate;
    if (result.complexity < 0 || result.complexity > 10) result.complexity = kDefaultComplexity;
    if (result.application == 0) result.application = OPUS_APPLICATION_VOIP;
    if (result.max_packet_bytes <= 0) result.max_packet_bytes = kDefaultPacketBytes;
    return result;
}

} // namespace

/** C ABI 初始化入口：创建 C++ 对象并把不透明指针保存到 C 上下文。 */
extern "C" int opus_audio_encoder_init(OpusEncoderCtx *ctx, const OpusEncoderConfig *config)
{
    OpusEncoderConfig normalized;
    OpusAudioEncoder *encoder;

    if (!ctx) {
        LOG_ERROR("opus_audio_encoder_init failed: ctx is NULL");
        return -1;
    }
    *ctx = OpusEncoderCtx{};
    normalized = normalizeConfig(config);
    if (!isSupportedSampleRate(normalized.sample_rate) ||
        (normalized.channels != 1 && normalized.channels != 2) ||
        normalized.packet_loss_percent < 0 || normalized.packet_loss_percent > 100) {
        LOG_ERROR("opus encoder invalid config: rate=%d channels=%d loss=%d",
                  normalized.sample_rate, normalized.channels, normalized.packet_loss_percent);
        return -1;
    }
    /* vector 的缓冲分配仍可能抛异常，不能让异常越过 C ABI 边界。 */
    try {
        encoder = new OpusAudioEncoder(normalized);
    } catch (const std::bad_alloc &) {
        LOG_ERROR("opus encoder allocation failed");
        return -1;
    }
    if (!encoder || !encoder->initialize()) {
        LOG_ERROR("opus_audio_encoder_init failed: initialize rate=%d channels=%d bitrate=%d",
                  normalized.sample_rate, normalized.channels, normalized.bitrate);
        delete encoder;
        return -1;
    }
    ctx->config = normalized;
    ctx->handle = encoder;
    ctx->initialized = 1;
    LOG_INFO("opus encoder init success: rate=%d channels=%d bitrate=%d complexity=%d vbr=%d fec=%d dtx=%d",
             normalized.sample_rate, normalized.channels, normalized.bitrate, normalized.complexity,
             normalized.enable_vbr, normalized.enable_fec, normalized.enable_dtx);
    return 0;
}

/** C ABI 编码入口：返回的裸 Opus packet 会由网关立即复制到 MediaBuffer。 */
extern "C" int opus_audio_encoder_encode_s16le(OpusEncoderCtx *ctx,
                                                 const int16_t *pcm,
                                                 int samples_per_channel,
                                                 const uint8_t **out_data,
                                                 size_t *out_size,
                                                 MediaCodecType *out_codec)
{
    OpusAudioEncoder *encoder;

    if (!ctx || !ctx->initialized || !ctx->handle || !pcm || samples_per_channel <= 0 ||
        !out_data || !out_size || !out_codec) {
        LOG_ERROR("opus_audio_encoder_encode_s16le failed: ctx=%p initialized=%d handle=%p pcm=%p samples=%d out_data=%p out_size=%p out_codec=%p",
                  static_cast<void *>(ctx),
                  ctx ? ctx->initialized : 0,
                  ctx ? ctx->handle : NULL,
                  static_cast<const void *>(pcm),
                  samples_per_channel,
                  static_cast<void *>(out_data),
                  static_cast<void *>(out_size),
                  static_cast<void *>(out_codec));
        return -1;
    }
    encoder = static_cast<OpusAudioEncoder *>(ctx->handle);
    if (encoder->encode(pcm, samples_per_channel, out_data, out_size) != 0) {
        LOG_ERROR("opus_audio_encoder_encode_s16le failed: encode samples=%d rate=%d channels=%d",
                  samples_per_channel, ctx->config.sample_rate, ctx->config.channels);
        return -1;
    }
    *out_codec = MEDIA_CODEC_OPUS;
    return 0;
}

/** C ABI 释放入口：delete 会依次释放复用缓冲和 libopus encoder。 */
extern "C" void opus_audio_encoder_deinit(OpusEncoderCtx *ctx)
{
    if (!ctx)
        return;
    delete static_cast<OpusAudioEncoder *>(ctx->handle);
    *ctx = OpusEncoderCtx{};
}
