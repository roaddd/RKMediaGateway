#include "audioEncoderInternal.h"

#include "codecs/aacEncoder.h"
#include "codecs/g711Encoder.h"
#include "logger.h"
#include "opus_defines.h"
#include "codecs/opusEncoder.h"

#include <functional>
#include <new>

namespace rkmedia {
namespace {

const int kDefaultG711SampleRate = 8000;
const int kDefaultG711MaxSamples = 320;
const int kDefaultAacSampleRate = 8000;
const int kDefaultAacMaxSamples = 1024;
const int kDefaultAacBitrate = 32000;
const int kDefaultAacProfile = 2;
const int kDefaultOpusSampleRate = 48000;
const int kDefaultOpusBitrate = 24000;
const int kDefaultOpusComplexity = 6;
const int kDefaultOpusPacketLossPercent = 10;
const int kDefaultOpusPacketBytes = 4000;

bool isSupportedOpusSampleRate(int sample_rate)
{
    return sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 ||
           sample_rate == 24000 || sample_rate == 48000;
}

bool isSupportedOpusApplication(int application)
{
    return application == OPUS_APPLICATION_VOIP ||
           application == OPUS_APPLICATION_AUDIO ||
           application == OPUS_APPLICATION_RESTRICTED_LOWDELAY;
}

void hashCombine(size_t *seed, int value)
{
    size_t value_hash = 0;

    if (!seed)
        return;
    value_hash = std::hash<int>()(value);
    *seed ^= value_hash + static_cast<size_t>(0x9e3779b9U) + (*seed << 6) + (*seed >> 2);
}

} // namespace

AudioEncoderKey::AudioEncoderKey()
    : codec(MEDIA_CODEC_NONE),
      sample_rate(0),
      encoder_channels(0),
      max_samples_per_frame(0),
      capture_channel_index(0),
      bitrate(0),
      profile(0),
      complexity(0),
      enable_vbr(0),
      enable_fec(0),
      enable_dtx(0),
      packet_loss_percent(0),
      application(0),
      max_packet_bytes(0)
{
}

bool AudioEncoderKey::operator==(const AudioEncoderKey &other) const
{
    return codec == other.codec &&
           sample_rate == other.sample_rate &&
           encoder_channels == other.encoder_channels &&
           max_samples_per_frame == other.max_samples_per_frame &&
           capture_channel_index == other.capture_channel_index &&
           bitrate == other.bitrate &&
           profile == other.profile &&
           complexity == other.complexity &&
           enable_vbr == other.enable_vbr &&
           enable_fec == other.enable_fec &&
           enable_dtx == other.enable_dtx &&
           packet_loss_percent == other.packet_loss_percent &&
           application == other.application &&
           max_packet_bytes == other.max_packet_bytes;
}

bool AudioEncoderKey::operator!=(const AudioEncoderKey &other) const
{
    return !(*this == other);
}

size_t AudioEncoderKeyHash::operator()(const AudioEncoderKey &key) const
{
    size_t seed = 0;

    hashCombine(&seed, static_cast<int>(key.codec));
    hashCombine(&seed, key.sample_rate);
    hashCombine(&seed, key.encoder_channels);
    hashCombine(&seed, key.max_samples_per_frame);
    hashCombine(&seed, key.capture_channel_index);
    hashCombine(&seed, key.bitrate);
    hashCombine(&seed, key.profile);
    hashCombine(&seed, key.complexity);
    hashCombine(&seed, key.enable_vbr);
    hashCombine(&seed, key.enable_fec);
    hashCombine(&seed, key.enable_dtx);
    hashCombine(&seed, key.packet_loss_percent);
    hashCombine(&seed, key.application);
    hashCombine(&seed, key.max_packet_bytes);
    return seed;
}

/**
 * @description: 将各编码器原有的缺省规则集中到统一接口，并校验公共参数组合。
 */
int normalizeAudioEncoderConfig(const AudioEncoderConfig &config,
                                AudioEncoderConfig *normalized)
{
    AudioEncoderConfig result;

    if (!normalized) {
        LOG_ERROR("normalizeAudioEncoderConfig failed: normalized is NULL codec=%d", config.codec);
        return -1;
    }
    result = config;
    if (result.codec != MEDIA_CODEC_G711A && result.codec != MEDIA_CODEC_G711U &&
        result.codec != MEDIA_CODEC_AAC && result.codec != MEDIA_CODEC_OPUS) {
        LOG_ERROR("normalizeAudioEncoderConfig failed: unsupported codec=%d", result.codec);
        return -1;
    }
    if (result.encoder_channels <= 0)
        result.encoder_channels = 1;

    /* mono 编码需要保留输入声道选择；stereo 输入不执行单声道选择。 */
    if (result.encoder_channels == 1) {
        if (result.capture_channel_index < 0)
            result.capture_channel_index = 0;
    } else {
        result.capture_channel_index = -1;
    }

    if (result.codec == MEDIA_CODEC_G711A || result.codec == MEDIA_CODEC_G711U) {
        if (result.sample_rate <= 0)
            result.sample_rate = kDefaultG711SampleRate;
        if (result.max_samples_per_frame <= 0)
            result.max_samples_per_frame = kDefaultG711MaxSamples;
        if (result.encoder_channels != 1) {
            LOG_ERROR("normalizeAudioEncoderConfig failed: G711 requires mono channels=%d", result.encoder_channels);
            return -1;
        }
    } else if (result.codec == MEDIA_CODEC_AAC) {
        if (result.sample_rate <= 0)
            result.sample_rate = kDefaultAacSampleRate;
        if (result.max_samples_per_frame <= 0)
            result.max_samples_per_frame = kDefaultAacMaxSamples;
        if (result.aac.bitrate <= 0)
            result.aac.bitrate = kDefaultAacBitrate;
        if (result.aac.profile <= 0)
            result.aac.profile = kDefaultAacProfile;
        if (result.encoder_channels != 1 && result.encoder_channels != 2) {
            LOG_ERROR("normalizeAudioEncoderConfig failed: AAC channels=%d expected=1..2", result.encoder_channels);
            return -1;
        }
    } else {
        if (result.sample_rate <= 0)
            result.sample_rate = kDefaultOpusSampleRate;
        if (result.opus.bitrate <= 0)
            result.opus.bitrate = kDefaultOpusBitrate;
        if (result.opus.complexity < 0 || result.opus.complexity > 10)
            result.opus.complexity = kDefaultOpusComplexity;
        if (result.opus.application == 0)
            result.opus.application = OPUS_APPLICATION_VOIP;
        if (result.opus.max_packet_bytes <= 0)
            result.opus.max_packet_bytes = kDefaultOpusPacketBytes;
        result.opus.enable_vbr = result.opus.enable_vbr ? 1 : 0;
        result.opus.enable_fec = result.opus.enable_fec ? 1 : 0;
        result.opus.enable_dtx = result.opus.enable_dtx ? 1 : 0;
        /* Opus 不使用公共最大输入采样数，帧长由每次 encode 的合法时长决定。 */
        result.max_samples_per_frame = 0;
        if (!isSupportedOpusSampleRate(result.sample_rate)) {
            LOG_ERROR("normalizeAudioEncoderConfig failed: unsupported Opus sample_rate=%d", result.sample_rate);
            return -1;
        }
        if (!isSupportedOpusApplication(result.opus.application)) {
            LOG_ERROR("normalizeAudioEncoderConfig failed: unsupported Opus application=%d",
                      result.opus.application);
            return -1;
        }
        if (result.encoder_channels != 1 && result.encoder_channels != 2) {
            LOG_ERROR("normalizeAudioEncoderConfig failed: Opus channels=%d expected=1..2", result.encoder_channels);
            return -1;
        }
        if (result.opus.packet_loss_percent < 0 || result.opus.packet_loss_percent > 100) {
            LOG_ERROR("normalizeAudioEncoderConfig failed: Opus packet_loss_percent=%d expected=0..100",
                      result.opus.packet_loss_percent);
            return -1;
        }
    }

    *normalized = result;
    return 0;
}

/**
 * @description: 先归一化配置，再只提取当前编码格式真正生效的字段生成 Key。
 */
int makeAudioEncoderKey(const AudioEncoderConfig &config, AudioEncoderKey *key)
{
    AudioEncoderConfig normalized;
    AudioEncoderKey result;

    if (!key) {
        LOG_ERROR("makeAudioEncoderKey failed: key is NULL codec=%d", config.codec);
        return -1;
    }
    if (normalizeAudioEncoderConfig(config, &normalized) != 0) {
        LOG_ERROR("makeAudioEncoderKey failed: normalize codec=%d", config.codec);
        return -1;
    }

    result.codec = normalized.codec;
    result.sample_rate = normalized.sample_rate;
    result.encoder_channels = normalized.encoder_channels;
    result.max_samples_per_frame = normalized.max_samples_per_frame;
    result.capture_channel_index = normalized.capture_channel_index;
    if (normalized.codec == MEDIA_CODEC_AAC) {
        result.bitrate = normalized.aac.bitrate;
        result.profile = normalized.aac.profile;
    } else if (normalized.codec == MEDIA_CODEC_OPUS) {
        result.bitrate = normalized.opus.bitrate;
        result.complexity = normalized.opus.complexity;
        result.enable_vbr = normalized.opus.enable_vbr;
        result.enable_fec = normalized.opus.enable_fec;
        result.enable_dtx = normalized.opus.enable_dtx;
        result.packet_loss_percent = normalized.opus.packet_loss_percent;
        result.application = normalized.opus.application;
        result.max_packet_bytes = normalized.opus.max_packet_bytes;
    }
    *key = result;
    return 0;
}

namespace {

/** 清空输出视图，避免编码失败或 AAC 暂无输出时遗留上一帧数据。 */
void resetOutput(AudioEncoderOutput *output, uint64_t pts_us, MediaCodecType codec)
{
    if (!output)
        return;
    output->data = NULL;
    output->size = 0;
    output->pts_us = pts_us;
    output->codec = codec;
}

/**
 * @description: 使用现有 G711 C 编码器实现统一接口。
 */
class G711AudioEncoderAdapter : public AudioEncoder {
public:
    G711AudioEncoderAdapter() : ctx_() {}

    ~G711AudioEncoderAdapter()
    {
        g711_encoder_deinit(&ctx_);
    }

    int initialize(const AudioEncoderConfig &config)
    {
        G711EncoderConfig native_config = {};

        if (config.codec != MEDIA_CODEC_G711A && config.codec != MEDIA_CODEC_G711U) {
            LOG_ERROR("G711AudioEncoderAdapter initialize failed: unsupported codec=%d", config.codec);
            return -1;
        }
        native_config.mode = (config.codec == MEDIA_CODEC_G711U)
                                 ? G711_ENCODER_MODE_ULAW
                                 : G711_ENCODER_MODE_ALAW;
        native_config.sample_rate = config.sample_rate;
        native_config.channels = config.encoder_channels;
        native_config.max_samples_per_frame = config.max_samples_per_frame;
        if (g711_encoder_init(&ctx_, &native_config) != 0) {
            LOG_ERROR("G711AudioEncoderAdapter initialize failed: rate=%d channels=%d max_samples=%d codec=%d",
                      config.sample_rate,
                      config.encoder_channels,
                      config.max_samples_per_frame,
                      config.codec);
            return -1;
        }
        return 0;
    }

    MediaCodecType codec() const
    {
        return (ctx_.config.mode == G711_ENCODER_MODE_ULAW) ? MEDIA_CODEC_G711U : MEDIA_CODEC_G711A;
    }

    int sampleRate() const
    {
        return ctx_.config.sample_rate;
    }

    int channels() const
    {
        return ctx_.config.channels;
    }

    int encode(const int16_t *pcm,
               int samples_per_channel,
               uint64_t pts_us,
               AudioEncoderOutput *output)
    {
        const uint8_t *data = NULL;
        size_t size = 0;
        MediaCodecType output_codec = MEDIA_CODEC_NONE;

        if (!output) {
            LOG_ERROR("G711AudioEncoderAdapter encode failed: output is NULL");
            return -1;
        }
        resetOutput(output, pts_us, codec());
        if (g711_encoder_encode_s16le(&ctx_, pcm, samples_per_channel,
                                      &data, &size, &output_codec) != 0) {
            LOG_ERROR("G711AudioEncoderAdapter encode failed: pcm=%p samples=%d pts=%llu",
                      static_cast<const void *>(pcm),
                      samples_per_channel,
                      static_cast<unsigned long long>(pts_us));
            return -1;
        }
        output->data = data;
        output->size = size;
        output->pts_us = pts_us;
        output->codec = output_codec;
        return 0;
    }

private:
    G711EncoderCtx ctx_;
};

/**
 * @description: 使用现有 AAC C 编码器实现统一接口，并保留其 PCM 缓存和 PTS 语义。
 */
class AacAudioEncoderAdapter : public AudioEncoder {
public:
    AacAudioEncoderAdapter() : ctx_() {}

    ~AacAudioEncoderAdapter()
    {
        aac_encoder_deinit(&ctx_);
    }

    int initialize(const AudioEncoderConfig &config)
    {
        AacEncoderConfig native_config = {};

        if (config.codec != MEDIA_CODEC_AAC) {
            LOG_ERROR("AacAudioEncoderAdapter initialize failed: unsupported codec=%d", config.codec);
            return -1;
        }
        native_config.sample_rate = config.sample_rate;
        native_config.channels = config.encoder_channels;
        native_config.bitrate = config.aac.bitrate;
        native_config.profile = config.aac.profile;
        native_config.max_samples_per_frame = config.max_samples_per_frame;
        if (aac_encoder_init(&ctx_, &native_config) != 0) {
            LOG_ERROR("AacAudioEncoderAdapter initialize failed: rate=%d channels=%d bitrate=%d profile=%d max_samples=%d",
                      config.sample_rate,
                      config.encoder_channels,
                      config.aac.bitrate,
                      config.aac.profile,
                      config.max_samples_per_frame);
            return -1;
        }
        return 0;
    }

    MediaCodecType codec() const
    {
        return MEDIA_CODEC_AAC;
    }

    int sampleRate() const
    {
        return ctx_.config.sample_rate;
    }

    int channels() const
    {
        return ctx_.config.channels;
    }

    int encode(const int16_t *pcm,
               int samples_per_channel,
               uint64_t pts_us,
               AudioEncoderOutput *output)
    {
        const uint8_t *data = NULL;
        size_t size = 0;
        uint64_t output_pts_us = pts_us;
        MediaCodecType output_codec = MEDIA_CODEC_NONE;

        if (!output) {
            LOG_ERROR("AacAudioEncoderAdapter encode failed: output is NULL");
            return -1;
        }
        resetOutput(output, pts_us, codec());
        if (aac_encoder_encode_s16le(&ctx_, pcm, samples_per_channel, pts_us,
                                     &data, &size, &output_pts_us, &output_codec) != 0) {
            LOG_ERROR("AacAudioEncoderAdapter encode failed: pcm=%p samples=%d pts=%llu",
                      static_cast<const void *>(pcm),
                      samples_per_channel,
                      static_cast<unsigned long long>(pts_us));
            return -1;
        }
        output->data = data;
        output->size = size;
        output->pts_us = output_pts_us;
        output->codec = output_codec;
        return 0;
    }

private:
    AacEncoderCtx ctx_;
};

/**
 * @description: 使用现有 Opus C ABI 适配统一接口。
 */
class OpusAudioEncoderAdapter : public AudioEncoder {
public:
    OpusAudioEncoderAdapter() : ctx_() {}

    ~OpusAudioEncoderAdapter()
    {
        opus_audio_encoder_deinit(&ctx_);
    }

    int initialize(const AudioEncoderConfig &config)
    {
        OpusEncoderConfig native_config = {};

        if (config.codec != MEDIA_CODEC_OPUS) {
            LOG_ERROR("OpusAudioEncoderAdapter initialize failed: unsupported codec=%d", config.codec);
            return -1;
        }
        native_config.sample_rate = config.sample_rate;
        native_config.channels = config.encoder_channels;
        native_config.bitrate = config.opus.bitrate;
        native_config.complexity = config.opus.complexity;
        native_config.enable_vbr = config.opus.enable_vbr;
        native_config.enable_fec = config.opus.enable_fec;
        native_config.enable_dtx = config.opus.enable_dtx;
        native_config.packet_loss_percent = config.opus.packet_loss_percent;
        native_config.application = config.opus.application;
        native_config.max_packet_bytes = config.opus.max_packet_bytes;
        if (opus_audio_encoder_init(&ctx_, &native_config) != 0) {
            LOG_ERROR("OpusAudioEncoderAdapter initialize failed: rate=%d channels=%d bitrate=%d complexity=%d",
                      config.sample_rate,
                      config.encoder_channels,
                      config.opus.bitrate,
                      config.opus.complexity);
            return -1;
        }
        return 0;
    }

    MediaCodecType codec() const
    {
        return MEDIA_CODEC_OPUS;
    }

    int sampleRate() const
    {
        return ctx_.config.sample_rate;
    }

    int channels() const
    {
        return ctx_.config.channels;
    }

    int encode(const int16_t *pcm,
               int samples_per_channel,
               uint64_t pts_us,
               AudioEncoderOutput *output)
    {
        const uint8_t *data = NULL;
        size_t size = 0;
        MediaCodecType output_codec = MEDIA_CODEC_NONE;

        if (!output) {
            LOG_ERROR("OpusAudioEncoderAdapter encode failed: output is NULL");
            return -1;
        }
        resetOutput(output, pts_us, codec());
        if (opus_audio_encoder_encode_s16le(&ctx_, pcm, samples_per_channel,
                                            &data, &size, &output_codec) != 0) {
            LOG_ERROR("OpusAudioEncoderAdapter encode failed: pcm=%p samples=%d pts=%llu",
                      static_cast<const void *>(pcm),
                      samples_per_channel,
                      static_cast<unsigned long long>(pts_us));
            return -1;
        }
        output->data = data;
        output->size = size;
        output->pts_us = pts_us;
        output->codec = output_codec;
        return 0;
    }

private:
    OpusEncoderCtx ctx_;
};

} // namespace

/**
 * @description: 创建具体适配器、完成底层编码器初始化，并以统一接口返回。
 */
std::unique_ptr<AudioEncoder> createAudioEncoder(const AudioEncoderConfig &config)
{
    std::unique_ptr<AudioEncoder> result;
    AudioEncoderConfig normalized;
    G711AudioEncoderAdapter *g711_encoder = NULL;
    AacAudioEncoderAdapter *aac_encoder = NULL;
    OpusAudioEncoderAdapter *opus_encoder = NULL;

    if (normalizeAudioEncoderConfig(config, &normalized) != 0) {
        LOG_ERROR("createAudioEncoder failed: normalize codec=%d", config.codec);
        return std::unique_ptr<AudioEncoder>();
    }

    /* 先按编码格式选择适配器，再初始化“格式 + 参数”唯一确定的编码器实例。 */
    if (normalized.codec == MEDIA_CODEC_G711A || normalized.codec == MEDIA_CODEC_G711U) {
        g711_encoder = new (std::nothrow) G711AudioEncoderAdapter();
        if (!g711_encoder) {
            LOG_ERROR("createAudioEncoder failed: allocate G711 adapter codec=%d", normalized.codec);
            return std::unique_ptr<AudioEncoder>();
        }
        if (g711_encoder->initialize(normalized) != 0) {
            LOG_ERROR("createAudioEncoder failed: initialize G711 adapter codec=%d", normalized.codec);
            delete g711_encoder;
            return std::unique_ptr<AudioEncoder>();
        }
        result.reset(g711_encoder);
    } else if (normalized.codec == MEDIA_CODEC_AAC) {
        aac_encoder = new (std::nothrow) AacAudioEncoderAdapter();
        if (!aac_encoder) {
            LOG_ERROR("createAudioEncoder failed: allocate AAC adapter");
            return std::unique_ptr<AudioEncoder>();
        }
        if (aac_encoder->initialize(normalized) != 0) {
            LOG_ERROR("createAudioEncoder failed: initialize AAC adapter");
            delete aac_encoder;
            return std::unique_ptr<AudioEncoder>();
        }
        result.reset(aac_encoder);
    } else if (normalized.codec == MEDIA_CODEC_OPUS) {
        opus_encoder = new (std::nothrow) OpusAudioEncoderAdapter();
        if (!opus_encoder) {
            LOG_ERROR("createAudioEncoder failed: allocate Opus adapter");
            return std::unique_ptr<AudioEncoder>();
        }
        if (opus_encoder->initialize(normalized) != 0) {
            LOG_ERROR("createAudioEncoder failed: initialize Opus adapter");
            delete opus_encoder;
            return std::unique_ptr<AudioEncoder>();
        }
        result.reset(opus_encoder);
    } else {
        LOG_ERROR("createAudioEncoder failed: unsupported normalized codec=%d", normalized.codec);
        return std::unique_ptr<AudioEncoder>();
    }
    return result;
}

} // namespace rkmedia
