/**
 * @file audioDecoderBase.h
 * @brief audioDecoder 模块内部的 C++ 解码适配基类。
 */

#ifndef __AUDIO_DECODER_BASE_H__
#define __AUDIO_DECODER_BASE_H__

#include "audioDecoder.h"

namespace rkmedia {

/**
 * @description: 具体音频编码格式解码器使用的模块内部纯虚基类。
 *
 * 该基类只存在于 audioDecoder/src 内部，对外纯 C 接口通过不透明句柄持有其实例。
 */
class AudioDecoderBase {
public:
    /** @description: 释放具体格式解码器持有的编解码库状态和输出缓冲。 */
    virtual ~AudioDecoderBase() {}

    /**
     * @description: 正常解码一个完整的压缩音频包。
     * @param input 待解码压缩包及其时间戳，不能为空。
     * @param output 输出 PCM 数据视图，不能为空；仅在返回 MEDIA_OK 时有效。
     * @return MEDIA_OK 解码成功；否则返回参数、状态或底层解码错误。
     * @note output->data 由具体适配器持有，在下一次操作前有效。
     */
    virtual MediaResult decode(const AudioDecoderInput *input,
                               AudioDecoderOutput *output) = 0;

    /**
     * @description: 使用后续包的带内冗余或解码器自身的丢包隐藏恢复一个丢失帧。
     * @param input 丢失帧长度、时间戳和可选后续压缩包，不能为空。
     * @param output 恢复出的 PCM 数据视图，不能为空；仅在返回 MEDIA_OK 时有效。
     * @param used_inband_redundancy 输出恢复方式，不能为空；返回 MEDIA_OK 时，true 表示
     *        本次确实读取了 following_packet_data 中的带内冗余，false 表示没有使用
     *        后续包冗余，而是执行了具体解码器的丢包隐藏。函数失败时该值无业务意义。
     * @return MEDIA_OK 恢复成功；否则返回不支持、参数、状态或底层解码错误。
     * @note 若使用了后续包冗余，调用方之后仍需正常解码同一个后续包以得到当前帧。
     */
    virtual MediaResult recoverLoss(const AudioDecoderLossInput *input,
                                    AudioDecoderOutput *output,
                                    bool *used_inband_redundancy) = 0;

    /**
     * @description: 清除当前编码格式解码器的历史状态。
     * @return MEDIA_OK 重置成功；否则返回未就绪或底层解码器错误。
     * @note 调用后此前返回的 PCM 数据视图不再有效。
     */
    virtual MediaResult reset() = 0;

protected:
    /** @description: 仅允许具体格式适配器构造基类部分。 */
    AudioDecoderBase() {}

private:
    AudioDecoderBase(const AudioDecoderBase &) = delete;
    AudioDecoderBase &operator=(const AudioDecoderBase &) = delete;
};

} // namespace rkmedia

#endif /* __AUDIO_DECODER_BASE_H__ */
