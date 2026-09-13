#include "opusDecoder.h"

#include "logger.h"
#include "opus.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace rkmedia {
namespace {

/** @description: 判断采样率是否属于 libopus 支持的固定集合。 */
static bool opus_decoder_is_supported_rate(int sample_rate)
{
    return sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 ||
           sample_rate == 24000 || sample_rate == 48000;
}

/** @description: 使用 RAII 管理 libopus 句柄和可复用 PCM 输出缓冲。 */
class OpusAudioDecoder : public AudioDecoderBase {
public:
    explicit OpusAudioDecoder(const AudioDecoderConfig &config)
        : config_(config), decoder_(NULL), output_buffer_()
    {
    }

    ~OpusAudioDecoder() override
    {
        if (decoder_ != NULL) {
            opus_decoder_destroy(decoder_);
            decoder_ = NULL;
        }
    }

    /** @description: 校验 Opus 参数、分配输出缓冲并创建 libopus 解码器。 */
    MediaResult initialize()
    {
        size_t output_sample_capacity = 0;
        int opus_error = OPUS_OK;

        if (!opus_decoder_is_supported_rate(config_.sample_rate)) {
            LOG_ERROR("opus decoder initialize failed: unsupported sample_rate=%d",
                      config_.sample_rate);
            return MEDIA_ERR_INVALID_CONFIG;
        }
        if (config_.output_channels != 1 && config_.output_channels != 2) {
            LOG_ERROR("opus decoder initialize failed: output_channels=%d expected 1 or 2",
                      config_.output_channels);
            return MEDIA_ERR_INVALID_CONFIG;
        }

        output_sample_capacity =
            static_cast<size_t>(config_.max_frame_samples_per_channel) *
            static_cast<size_t>(config_.output_channels);
        try {
            output_buffer_.resize(output_sample_capacity);
        } catch (const std::bad_alloc &) {
            LOG_ERROR("opus decoder initialize failed: allocate output samples=%zu",
                      output_sample_capacity);
            return MEDIA_ERR_NO_MEMORY;
        }

        decoder_ = opus_decoder_create(config_.sample_rate,
                                       config_.output_channels,
                                       &opus_error);
        if (decoder_ == NULL || opus_error != OPUS_OK) {
            LOG_ERROR("opus decoder initialize failed: create rate=%d channels=%d err=%s",
                      config_.sample_rate,
                      config_.output_channels,
                      opus_strerror(opus_error));
            output_buffer_.clear();
            return MEDIA_ERR;
        }
        return MEDIA_OK;
    }

    /** @description: 校验并正常解码一个完整的 Opus 包。 */
    MediaResult decode(const AudioDecoderInput *input,
                       AudioDecoderOutput *output) override
    {
        const unsigned char *packet_data = NULL;
        opus_int32 packet_size = 0;
        int packet_samples = 0;
        int decoded_samples = 0;

        if (decoder_ == NULL) {
            LOG_ERROR("opus decoder decode failed: decoder is not initialized");
            return MEDIA_ERR_NOT_READY;
        }
        if (input == NULL || output == NULL || input->data == NULL || input->size == 0) {
            LOG_ERROR("opus decoder decode failed: input=%p output=%p data=%p size=%zu",
                      (const void *)input,
                      (void *)output,
                      input != NULL ? (const void *)input->data : NULL,
                      input != NULL ? input->size : 0);
            return MEDIA_ERR_INVALID_PARAM;
        }
        std::memset(output, 0, sizeof(*output));

        /* libopus 的包长度参数是 opus_int32，转换前必须避免整数截断。 */
        if (input->size > static_cast<size_t>(INT32_MAX)) {
            LOG_ERROR("opus decoder decode failed: packet size=%zu exceeds opus_int32",
                      input->size);
            return MEDIA_ERR_INVALID_PARAM;
        }

        packet_data = reinterpret_cast<const unsigned char *>(input->data);
        packet_size = static_cast<opus_int32>(input->size);

        /*
         * Opus 包自身携带帧时长。先读取实际采样数，避免把浏览器输入固定假设为
         * 20 ms/960 采样，并确保预分配的 PCM 缓冲足够容纳本次输出。
         */
        packet_samples = opus_packet_get_nb_samples(packet_data,
                                                    packet_size,
                                                    config_.sample_rate);
        if (packet_samples < 0) {
            LOG_ERROR("opus decoder decode failed: invalid packet size=%zu err=%s",
                      input->size,
                      opus_strerror(packet_samples));
            return MEDIA_ERR_INVALID_PARAM;
        }
        if (packet_samples > config_.max_frame_samples_per_channel) {
            LOG_ERROR("opus decoder decode failed: packet samples=%d exceeds max=%d",
                      packet_samples,
                      config_.max_frame_samples_per_channel);
            return MEDIA_ERR_INVALID_CONFIG;
        }

        /*
         * 最后一个参数固定为 0，明确表示正常解码当前包。packet_samples 是每声道
         * 可写入的采样数，opus_decode 返回本次实际输出的每声道采样数。
         */
        decoded_samples = opus_decode(decoder_,
                                      packet_data,
                                      packet_size,
                                      reinterpret_cast<opus_int16 *>(output_buffer_.data()),
                                      packet_samples,
                                      0);
        if (decoded_samples < 0) {
            LOG_ERROR("opus decoder decode failed: size=%zu frame_size=%d err=%s",
                      input->size,
                      packet_samples,
                      opus_strerror(decoded_samples));
            return MEDIA_ERR;
        }
        if (decoded_samples != packet_samples) {
            LOG_ERROR("opus decoder decode failed: output samples mismatch size=%zu "
                      "packet_samples=%d decoded_samples=%d",
                      input->size,
                      packet_samples,
                      decoded_samples);
            return MEDIA_ERR;
        }

        /*
         * 第五步：返回内部 PCM 数据视图，不复制样本。该地址会在下一次解码时复用，
         * 调用方必须在下一次 decode/reset/deinit 之前完成消费或自行复制。
         */
        output->data = output_buffer_.data();
        output->samples_per_channel = decoded_samples;
        output->sample_rate = config_.sample_rate;
        output->channels = config_.output_channels;
        output->pts_us = input->pts_us;
        return MEDIA_OK;
    }

    /**
     * @description: 恢复一个丢失帧；有可用带内冗余时优先恢复，否则执行 PLC。
     */
    MediaResult recoverLoss(const AudioDecoderLossInput *input,
                            AudioDecoderOutput *output,
                            bool *used_inband_redundancy) override
    {
        const unsigned char *packet_data = NULL;
        opus_int32 packet_size = 0;
        int has_inband_redundancy = 0;
        int decoded_samples = 0;
        int decode_fec = 0;

        if (decoder_ == NULL) {
            LOG_ERROR("opus decoder recover loss failed: decoder is not initialized");
            return MEDIA_ERR_NOT_READY;
        }
        if (input == NULL || output == NULL || used_inband_redundancy == NULL) {
            LOG_ERROR("opus decoder recover loss failed: input=%p output=%p used_inband=%p",
                      (const void *)input,
                      (void *)output,
                      (void *)used_inband_redundancy);
            return MEDIA_ERR_INVALID_PARAM;
        }
        std::memset(output, 0, sizeof(*output));
        *used_inband_redundancy = false;

        if ((input->following_packet_data == NULL) != (input->following_packet_size == 0)) {
            LOG_ERROR("opus decoder recover loss failed: inconsistent following packet "
                      "data=%p size=%zu",
                      (const void *)input->following_packet_data,
                      input->following_packet_size);
            return MEDIA_ERR_INVALID_PARAM;
        }
        if (input->following_packet_size > static_cast<size_t>(INT32_MAX)) {
            LOG_ERROR("opus decoder recover loss failed: following packet size=%zu exceeds opus_int32",
                      input->following_packet_size);
            return MEDIA_ERR_INVALID_PARAM;
        }
        if (input->lost_samples_per_channel <= 0 || input->lost_samples_per_channel > config_.max_frame_samples_per_channel) {
            LOG_ERROR("opus decoder recover loss failed: lost_samples=%d max=%d",
                      input->lost_samples_per_channel,
                      config_.max_frame_samples_per_channel);
            return MEDIA_ERR_INVALID_PARAM;
        }

        /*
         * 后续包可能携带前一帧的低码率冗余。先检查 LBRR 标志：存在时把后续包
         * 交给 opus_decode(..., decode_fec=1)；不存在或尚未收到后续包时传入空包，
         * 让 libopus 根据已有历史状态执行 PLC。两种方式都只输出丢失的前一帧。
         */
        if (input->following_packet_data != NULL) {
            packet_data = reinterpret_cast<const unsigned char *>(input->following_packet_data);
            packet_size = static_cast<opus_int32>(input->following_packet_size);
            has_inband_redundancy = opus_packet_has_lbrr(packet_data, packet_size);
            if (has_inband_redundancy < 0) {
                LOG_ERROR("opus decoder recover loss failed: inspect following packet size=%zu err=%s",
                          input->following_packet_size,
                          opus_strerror(has_inband_redundancy));
                return MEDIA_ERR_INVALID_PARAM;
            }
        }
        if (has_inband_redundancy > 0) {
            decode_fec = 1;
            *used_inband_redundancy = true;
        } else {
            packet_data = NULL;
            packet_size = 0;
        }

        decoded_samples = opus_decode(decoder_,
                                      packet_data,
                                      packet_size,
                                      reinterpret_cast<opus_int16 *>(output_buffer_.data()),
                                      input->lost_samples_per_channel,
                                      decode_fec);
        if (decoded_samples < 0) {
            *used_inband_redundancy = false;
            LOG_ERROR("opus decoder recover loss failed: following_size=%zu lost_samples=%d "
                      "decode_fec=%d err=%s",
                      input->following_packet_size,
                      input->lost_samples_per_channel,
                      decode_fec,
                      opus_strerror(decoded_samples));
            return MEDIA_ERR;
        }

        output->data = output_buffer_.data();
        output->samples_per_channel = decoded_samples;
        output->sample_rate = config_.sample_rate;
        output->channels = config_.output_channels;
        output->pts_us = input->pts_us;
        return MEDIA_OK;
    }

    MediaResult reset() override
    {
        int opus_error = OPUS_OK;

        if (decoder_ == NULL) {
            LOG_ERROR("opus decoder reset failed: decoder is not initialized");
            return MEDIA_ERR_NOT_READY;
        }
        opus_error = opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
        if (opus_error != OPUS_OK) {
            LOG_ERROR("opus decoder reset failed: err=%s", opus_strerror(opus_error));
            return MEDIA_ERR;
        }
        return MEDIA_OK;
    }

private:
    AudioDecoderConfig config_;          /* 已通过严格校验的 Opus 输出参数。 */
    OpusDecoder *decoder_;               /* libopus 解码器句柄。 */
    std::vector<int16_t> output_buffer_; /* 在多次 decode 间复用的 PCM 缓冲。 */
};

} // namespace

MediaResult createOpusAudioDecoder(
    const AudioDecoderConfig &config,
    std::unique_ptr<AudioDecoderBase> *decoder)
{
    std::unique_ptr<OpusAudioDecoder> opus_decoder;
    MediaResult result = MEDIA_OK;

    if (decoder == NULL) {
        LOG_ERROR("create Opus audio decoder failed: decoder is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    decoder->reset();
    opus_decoder.reset(new (std::nothrow) OpusAudioDecoder(config));
    if (!opus_decoder) {
        LOG_ERROR("create Opus audio decoder failed: allocate adapter");
        return MEDIA_ERR_NO_MEMORY;
    }
    result = opus_decoder->initialize();
    if (result != MEDIA_OK) {
        LOG_ERROR("create Opus audio decoder failed: initialize result=%d", (int)result);
        return result;
    }

    *decoder = std::move(opus_decoder);
    return MEDIA_OK;
}

} // namespace rkmedia
