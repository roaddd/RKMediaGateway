#include "audioDecoder.h"

#include "audioDecoderBase.h"
#include "codecs/opusDecoder.h"
#include "logger.h"

#include <cstring>
#include <memory>
#include <new>

/**
 * @description: 纯 C 接口背后的不透明运行时对象。
 *
 * 调用方只能持有 AudioDecoderHandle 指针；具体 C++ 基类、适配器、配置和统计
 * 均在本实现文件内管理，不会成为公共头文件的一部分。
 */
struct AudioDecoderHandle {
    AudioDecoderHandle()
        : decoder(), config(), stats()
    {
        std::memset(&config, 0, sizeof(config));
        std::memset(&stats, 0, sizeof(stats));
    }

    std::unique_ptr<rkmedia::AudioDecoderBase> decoder; /* 当前编码格式的解码适配器。 */
    AudioDecoderConfig config;                         /* 已通过严格校验的创建配置。 */
    AudioDecoderStats stats;                           /* 跨适配器统一维护的运行统计。 */
};

namespace {

/** @description: 严格校验调用方显式提供的解码参数，不修改任何配置值。 */
static MediaResult audio_decoder_validate_config(const AudioDecoderConfig *config)
{
    if (config == NULL) {
        LOG_ERROR("audio decoder validate config failed: config is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (config->codec != MEDIA_CODEC_OPUS) {
        LOG_ERROR("audio decoder validate config failed: unsupported codec=%d",
                  (int)config->codec);
        return MEDIA_ERR_UNSUPPORTED;
    }
    if (config->sample_rate != 8000 && config->sample_rate != 12000 &&
        config->sample_rate != 16000 && config->sample_rate != 24000 &&
        config->sample_rate != 48000) {
        LOG_ERROR("audio decoder validate config failed: unsupported Opus sample_rate=%d",
                  config->sample_rate);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (config->output_channels != 1 && config->output_channels != 2) {
        LOG_ERROR("audio decoder validate config failed: output_channels=%d expected 1 or 2",
                  config->output_channels);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (config->max_frame_samples_per_channel <= 0 ||
        config->max_frame_samples_per_channel > config->sample_rate * 120 / 1000) {
        LOG_ERROR("audio decoder validate config failed: max_frame_samples=%d rate=%d",
                  config->max_frame_samples_per_channel,
                  config->sample_rate);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    return MEDIA_OK;
}

/** @description: 根据编码格式创建对应的模块内部 C++ 解码适配器。 */
static MediaResult audio_decoder_create_adapter(
    const AudioDecoderConfig &config,
    std::unique_ptr<rkmedia::AudioDecoderBase> *decoder)
{
    MediaResult result = MEDIA_OK;

    if (decoder == NULL) {
        LOG_ERROR("audio decoder create adapter failed: decoder is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    decoder->reset();

    switch (config.codec) {
    case MEDIA_CODEC_OPUS:
        result = rkmedia::createOpusAudioDecoder(config, decoder);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder create adapter failed: create Opus result=%d",
                      (int)result);
            return result;
        }
        break;
    default:
        LOG_ERROR("audio decoder create adapter failed: unsupported codec=%d",
                  (int)config.codec);
        return MEDIA_ERR_UNSUPPORTED;
    }
    return MEDIA_OK;
}

} // namespace

/**
 * @description: 严格校验配置，创建不透明句柄及其对应的 C++ 解码适配器。
 */
extern "C" MediaResult audio_decoder_create(const AudioDecoderConfig *config,
                                             AudioDecoderHandle **handle)
{
    AudioDecoderHandle *instance = NULL;
    MediaResult result = MEDIA_OK;

    if (handle == NULL) {
        LOG_ERROR("audio decoder create failed: handle output is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    *handle = NULL;

    result = audio_decoder_validate_config(config);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio decoder create failed: validate config result=%d", (int)result);
        return result;
    }

    instance = new (std::nothrow) AudioDecoderHandle();
    if (instance == NULL) {
        LOG_ERROR("audio decoder create failed: allocate handle");
        return MEDIA_ERR_NO_MEMORY;
    }
    result = audio_decoder_create_adapter(*config, &instance->decoder);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio decoder create failed: create codec=%d adapter result=%d",
                  (int)config->codec,
                  (int)result);
        delete instance;
        return result;
    }

    instance->config = *config;
    *handle = instance;
    LOG_INFO("audio decoder create success: codec=%d rate=%d output_channels=%d max_frame_samples=%d",
             (int)config->codec,
             config->sample_rate,
             config->output_channels,
             config->max_frame_samples_per_channel);
    return MEDIA_OK;
}

/** @description: delete 不透明句柄，并由 unique_ptr 级联释放具体解码器。 */
extern "C" void audio_decoder_destroy(AudioDecoderHandle *handle)
{
    delete handle;
}

/**
 * @description: 将普通压缩包交给具体格式适配器，并统一累计成功包、采样和错误统计。
 */
extern "C" MediaResult audio_decoder_decode(AudioDecoderHandle *handle,
                                             const AudioDecoderInput *input,
                                             AudioDecoderOutput *output)
{
    MediaResult result = MEDIA_OK;

    if (handle == NULL || !handle->decoder) {
        LOG_ERROR("audio decoder decode failed: handle=%p decoder_ready=%d",
                  (void *)handle,
                  handle != NULL && handle->decoder ? 1 : 0);
        return MEDIA_ERR_NOT_READY;
    }
    if (input == NULL || output == NULL) {
        LOG_ERROR("audio decoder decode failed: input=%p output=%p",
                  (const void *)input,
                  (void *)output);
        return MEDIA_ERR_INVALID_PARAM;
    }

    result = handle->decoder->decode(input, output);
    if (result != MEDIA_OK) {
        ++handle->stats.decode_errors;
        LOG_ERROR("audio decoder decode failed: codec=%d size=%zu result=%d",
                  (int)handle->config.codec,
                  input->size,
                  (int)result);
        return result;
    }

    ++handle->stats.decoded_packets;
    handle->stats.decoded_samples +=
        static_cast<uint64_t>(output->samples_per_channel);
    return MEDIA_OK;
}

/**
 * @description: 将通用丢包恢复请求交给具体格式适配器，并统一记录实际恢复方式。
 */
extern "C" MediaResult audio_decoder_recover_loss(
    AudioDecoderHandle *handle,
    const AudioDecoderLossInput *input,
    AudioDecoderOutput *output)
{
    MediaResult result = MEDIA_OK;
    bool used_inband_redundancy = false;

    if (handle == NULL || !handle->decoder) {
        LOG_ERROR("audio decoder recover loss failed: handle=%p decoder_ready=%d",
                  (void *)handle,
                  handle != NULL && handle->decoder ? 1 : 0);
        return MEDIA_ERR_NOT_READY;
    }
    if (input == NULL || output == NULL) {
        LOG_ERROR("audio decoder recover loss failed: input=%p output=%p",
                  (const void *)input,
                  (void *)output);
        return MEDIA_ERR_INVALID_PARAM;
    }

    result = handle->decoder->recoverLoss(input, output, &used_inband_redundancy);
    if (result != MEDIA_OK) {
        ++handle->stats.decode_errors;
        LOG_ERROR("audio decoder recover loss failed: codec=%d following_size=%zu "
                  "lost_samples=%d result=%d",
                  (int)handle->config.codec,
                  input->following_packet_size,
                  input->lost_samples_per_channel,
                  (int)result);
        return result;
    }

    if (used_inband_redundancy) {
        ++handle->stats.inband_recovered_frames;
    } else {
        ++handle->stats.concealed_frames;
    }
    handle->stats.decoded_samples +=
        static_cast<uint64_t>(output->samples_per_channel);
    return MEDIA_OK;
}

extern "C" MediaResult audio_decoder_reset(AudioDecoderHandle *handle)
{
    MediaResult result = MEDIA_OK;

    if (handle == NULL || !handle->decoder) {
        LOG_ERROR("audio decoder reset failed: handle=%p decoder_ready=%d",
                  (void *)handle,
                  handle != NULL && handle->decoder ? 1 : 0);
        return MEDIA_ERR_NOT_READY;
    }
    result = handle->decoder->reset();
    if (result != MEDIA_OK) {
        LOG_ERROR("audio decoder reset failed: codec=%d result=%d",
                  (int)handle->config.codec,
                  (int)result);
        return result;
    }
    return MEDIA_OK;
}

extern "C" MediaResult audio_decoder_get_config(const AudioDecoderHandle *handle,
                                                 AudioDecoderConfig *config)
{
    if (handle == NULL || config == NULL) {
        LOG_ERROR("audio decoder get config failed: handle=%p config=%p",
                  (const void *)handle,
                  (void *)config);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!handle->decoder) {
        LOG_ERROR("audio decoder get config failed: decoder is not ready");
        return MEDIA_ERR_NOT_READY;
    }
    *config = handle->config;
    return MEDIA_OK;
}

extern "C" MediaResult audio_decoder_get_stats(const AudioDecoderHandle *handle,
                                                AudioDecoderStats *stats)
{
    if (handle == NULL || stats == NULL) {
        LOG_ERROR("audio decoder get stats failed: handle=%p stats=%p",
                  (const void *)handle,
                  (void *)stats);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!handle->decoder) {
        LOG_ERROR("audio decoder get stats failed: decoder is not ready");
        return MEDIA_ERR_NOT_READY;
    }
    *stats = handle->stats;
    return MEDIA_OK;
}
