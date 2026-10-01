/**
 * @file opusDecoder.h
 * @brief Opus 解码适配器的模块内部类声明及工厂接口。
 */

#ifndef __OPUS_AUDIO_DECODER_H__
#define __OPUS_AUDIO_DECODER_H__

#include "../audioDecoderBase.h"

#include "opus.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace rkmedia {

/**
 * @description: 使用 libopus 实现普通解码、带内 FEC 恢复和 PLC 补偿的内部适配器。
 *
 * 该类仅在 audioDecoder 模块内部可见。调用方通过 AudioDecoderBase 和工厂函数
 * 持有实例，不直接依赖 libopus 的创建、销毁及 PCM 缓冲管理细节。
 */
class OpusAudioDecoder final : public AudioDecoderBase {
public:
    /** @description: 保存已经严格校验的 Opus 解码配置，尚不创建 libopus 句柄。 */
    explicit OpusAudioDecoder(const AudioDecoderConfig &config);

    /** @description: 销毁 libopus 解码器并释放内部 PCM 输出缓冲。 */
    ~OpusAudioDecoder() override;

    /** @description: 校验 Opus 参数、分配输出缓冲并创建 libopus 解码器。 */
    MediaResult initialize();

    /** @description: 校验并正常解码一个完整的 Opus 包。 */
    MediaResult decode(const AudioDecoderInput *input,
                       AudioDecoderOutput *output) override;

    /** @description: 优先使用后继包的带内 FEC 恢复丢失帧，否则执行 PLC。 */
    MediaResult recoverLoss(const AudioDecoderLossInput *input,
                            AudioDecoderOutput *output,
                            bool *used_inband_redundancy) override;

    /** @description: 清除 libopus 内部历史状态，使实例可用于一条新的连续音频流。 */
    MediaResult reset() override;

private:
    AudioDecoderConfig config_;          /* 已通过严格校验的 Opus 输出参数。 */
    OpusDecoder *decoder_;               /* libopus 解码器句柄。 */
    std::vector<int16_t> output_buffer_; /* 在多次 decode 间复用的 PCM 缓冲。 */

    OpusAudioDecoder(const OpusAudioDecoder &) = delete;
    OpusAudioDecoder &operator=(const OpusAudioDecoder &) = delete;
};

/** @description: 根据已经严格校验的参数创建并初始化 Opus 解码适配器。 */
MediaResult createOpusAudioDecoder(
    const AudioDecoderConfig &config,
    std::unique_ptr<AudioDecoderBase> *decoder);

} // namespace rkmedia

#endif /* __OPUS_AUDIO_DECODER_H__ */
