/**
 * @file audioDecoder.h
 * @brief 音频解码模块纯 C 对外接口。
 */

#ifndef __AUDIO_DECODER_H__
#define __AUDIO_DECODER_H__

#include "commonDef.h"
#include "mediaPacket.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 音频解码器不透明句柄；具体 C++ 类型和第三方库状态只存在于模块内部。 */
typedef struct AudioDecoderHandle AudioDecoderHandle;

/** @brief 音频解码器创建配置，所有字段必须由调用方显式填写。 */
typedef struct {
    MediaCodecType codec;              /* 输入压缩数据的编码格式，当前支持 Opus。 */
    int sample_rate;                   /* 解码输出采样率；Opus 支持 8/12/16/24/48 kHz。 */
    int output_channels;               /* 解码输出 PCM 声道数，当前支持单声道或双声道。 */
    int max_frame_samples_per_channel; /* 单次解码允许输出的每声道最大采样数。 */
} AudioDecoderConfig;

/** @brief 一个待正常解码的压缩音频包。 */
typedef struct {
    const uint8_t *data; /* 裸压缩音频包地址，不能为空。 */
    size_t size;         /* 压缩音频包字节数，必须大于 0。 */
    uint64_t pts_us;     /* 当前音频包的起始时间戳，单位微秒。 */
} AudioDecoderInput;

/** @brief 请求解码器恢复一个丢失的音频帧。 */
typedef struct {
    const uint8_t *following_packet_data; /* 后续压缩包；可能携带恢复前一帧所需的冗余数据，允许为 NULL。 */
    size_t following_packet_size;         /* 后续压缩包字节数；无后续包时必须为 0。 */
    int lost_samples_per_channel;         /* 需要恢复的每声道 PCM 采样数。 */
    uint64_t pts_us;                      /* 丢失音频帧原本的起始时间戳，单位微秒。 */
} AudioDecoderLossInput;

/** @brief 单次解码返回的交错 PCM S16_LE 数据视图。 */
typedef struct {
    const int16_t *data;     /* 内部 PCM 缓冲地址，在下一次 decode/recover/reset/destroy 前有效。 */
    int samples_per_channel; /* 每个输出声道包含的采样数。 */
    int sample_rate;         /* 输出 PCM 采样率，单位 Hz。 */
    int channels;            /* 输出 PCM 声道数。 */
    uint64_t pts_us;         /* 从输入继承的起始时间戳，单位微秒。 */
} AudioDecoderOutput;

/** @brief 音频解码器累计统计。 */
typedef struct {
    uint64_t decoded_packets;         /* 成功完成普通解码的压缩包数。 */
    uint64_t inband_recovered_frames; /* 使用后续包携带的带内冗余成功恢复的帧数。 */
    uint64_t concealed_frames;        /* 未使用带内冗余、由解码器隐藏丢包影响的帧数。 */
    uint64_t decoded_samples;         /* 已输出的每声道 PCM 采样总数。 */
    uint64_t decode_errors;           /* 底层解码或丢包恢复失败次数。 */
} AudioDecoderStats;

/**
 * @description: 校验配置并创建一个解码器实例。
 * @param config 解码器配置，不能为空；所有字段必须由调用方显式填写。
 * @param handle 输出解码器句柄，不能为空；失败时保证写入 NULL。
 * @return MEDIA_OK 创建成功；否则返回参数、配置、内存或底层解码器初始化错误。
 * @note 每条独立音频流必须拥有自己的句柄，不能跨流共享有状态解码器。
 */
MediaResult audio_decoder_create(const AudioDecoderConfig *config,
                                 AudioDecoderHandle **handle);

/**
 * @description: 销毁解码器实例及其内部输出缓冲和编解码库状态。
 * @param handle audio_decoder_create() 返回的句柄；为 NULL 时不执行任何操作。
 * @note 调用后句柄及其返回过的 AudioDecoderOutput::data 均失效。
 */
void audio_decoder_destroy(AudioDecoderHandle *handle);

/**
 * @description: 将一个完整的裸压缩音频包正常解码为交错 PCM S16_LE。
 * @param handle 已成功创建且属于当前音频流的解码器句柄。
 * @param input 待解码压缩包及其时间戳，不能为空。
 * @param output 输出 PCM 数据视图，不能为空；仅在返回 MEDIA_OK 时有效。
 * @return MEDIA_OK 解码成功；否则返回未就绪、参数或底层解码错误。
 * @note output->data 由解码器持有，下一次解码、丢包恢复、重置或销毁前有效。
 */
MediaResult audio_decoder_decode(AudioDecoderHandle *handle,
                                 const AudioDecoderInput *input,
                                 AudioDecoderOutput *output);

/**
 * @description: 恢复一个丢失音频帧，优先使用后续包中的带内冗余，否则执行丢包隐藏。
 * @param handle 已成功创建且属于当前音频流的解码器句柄。
 * @param input 丢失帧长度、时间戳和可选后续压缩包，不能为空。
 * @param output 恢复出的 PCM 数据视图，不能为空；仅在返回 MEDIA_OK 时有效。
 * @return MEDIA_OK 恢复成功；否则返回未就绪、不支持、参数或底层解码错误。
 * @note 恢复后仍需调用 audio_decoder_decode() 正常解码 following_packet_data 对应的当前帧。
 * @note output->data 由解码器持有，必须在下一次解码、恢复、重置或销毁前消费或复制。
 */
MediaResult audio_decoder_recover_loss(AudioDecoderHandle *handle,
                                       const AudioDecoderLossInput *input,
                                       AudioDecoderOutput *output);

/**
 * @description: 清除解码历史状态，供流切换或时间戳中断后重新开始解码。
 * @param handle 已成功创建的解码器句柄。
 * @return MEDIA_OK 重置成功；否则返回未就绪或底层解码器错误。
 * @note 调用后此前返回的 AudioDecoderOutput::data 视图不再有效。
 */
MediaResult audio_decoder_reset(AudioDecoderHandle *handle);

/**
 * @description: 获取创建实例时通过严格校验的解码配置。
 * @param handle 已成功创建的解码器句柄。
 * @param config 输出配置快照，不能为空。
 * @return MEDIA_OK 获取成功；否则返回参数或未就绪错误。
 */
MediaResult audio_decoder_get_config(const AudioDecoderHandle *handle,
                                     AudioDecoderConfig *config);

/**
 * @description: 获取当前解码统计快照。
 * @param handle 已成功创建的解码器句柄。
 * @param stats 输出累计统计快照，不能为空。
 * @return MEDIA_OK 获取成功；否则返回参数或未就绪错误。
 */
MediaResult audio_decoder_get_stats(const AudioDecoderHandle *handle,
                                    AudioDecoderStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_DECODER_H__ */
