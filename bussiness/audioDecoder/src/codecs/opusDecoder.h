/**
 * @file opusDecoder.h
 * @brief Opus 解码适配器的模块内部工厂接口。
 */

#ifndef __OPUS_AUDIO_DECODER_H__
#define __OPUS_AUDIO_DECODER_H__

#include "../audioDecoderBase.h"

#include <memory>

namespace rkmedia {

/** @description: 根据归一化参数创建并初始化 Opus 解码适配器。 */
MediaResult createOpusAudioDecoder(
    const AudioDecoderConfig &config,
    std::unique_ptr<AudioDecoderBase> *decoder);

} // namespace rkmedia

#endif /* __OPUS_AUDIO_DECODER_H__ */
