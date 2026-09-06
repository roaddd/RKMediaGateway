#ifndef __AUDIO_ENCODER_INTERNAL_H__
#define __AUDIO_ENCODER_INTERNAL_H__

#include "audioEncoder.h"

#include <memory>

namespace rkmedia {

/** @description: AAC 编码器私有参数。 */
struct AudioEncoderAacOptions {
    int bitrate; /* 目标码率，单位 bit/s。 */
    int profile; /* AAC object type，2 表示 AAC-LC。 */

    AudioEncoderAacOptions() : bitrate(32000), profile(2) {}
};

/** @description: Opus 编码器私有参数。 */
struct AudioEncoderOpusOptions {
    int bitrate;             /* 目标码率，单位 bit/s。 */
    int complexity;          /* 编码复杂度，范围 0..10。 */
    int enable_vbr;          /* 是否启用 VBR。 */
    int enable_fec;          /* 是否启用带内 FEC。 */
    int enable_dtx;          /* 是否启用 DTX。 */
    int packet_loss_percent; /* 预期丢包率，范围 0..100。 */
    int application;         /* OPUS_APPLICATION_*；0 表示使用底层默认值。 */
    int max_packet_bytes;    /* 单个 Opus packet 的最大输出缓冲字节数。 */

    AudioEncoderOpusOptions()
        : bitrate(24000),
          complexity(6),
          enable_vbr(1),
          enable_fec(1),
          enable_dtx(0),
          packet_loss_percent(10),
          application(0),
          max_packet_bytes(4000)
    {
    }
};

/**
 * @description: 统一音频编码器创建参数。
 *
 * codec 决定实际创建的适配器。公共参数描述 PCM 输入格式，aac/opus 仅在选择
 * 对应编码格式时生效。结构体可以使用零初始化，具体默认值由现有编码器归一化。
 */
struct AudioEncoderConfig {
    MediaCodecType codec;          /* G711A、G711U、AAC 或 Opus。 */
    int sample_rate;               /* PCM 输入采样率。 */
    int encoder_channels;          /* 送入编码器并写入码流的目标通道数。 */
    int max_samples_per_frame;     /* 单次输入的每通道最大采样数。 */
    int capture_channel_index;     /* 单声道编码时选择的采集 PCM 通道下标。 */
    AudioEncoderAacOptions aac;    /* AAC 私有参数。 */
    AudioEncoderOpusOptions opus;  /* Opus 私有参数。 */

    AudioEncoderConfig()
        : codec(MEDIA_CODEC_NONE),
          sample_rate(0),
          encoder_channels(1),
          max_samples_per_frame(0),
          capture_channel_index(0),
          aac(),
          opus()
    {
    }
};

/**
 * @description: 归一化后的音频编码参数键。
 *
 * Key 只保存会影响编码器实例、PCM 输入选择或编码结果的字段，不包含配置名称、
 * 输出协议等路由信息。
 */
struct AudioEncoderKey {
    MediaCodecType codec;
    int sample_rate;
    int encoder_channels;
    int max_samples_per_frame;
    int capture_channel_index;
    int bitrate;
    int profile;
    int complexity;
    int enable_vbr;
    int enable_fec;
    int enable_dtx;
    int packet_loss_percent;
    int application;
    int max_packet_bytes;

    AudioEncoderKey();
    bool operator==(const AudioEncoderKey &other) const;
    bool operator!=(const AudioEncoderKey &other) const;
};

/** @description: AudioEncoderKey 的 unordered_map 哈希计算器。 */
struct AudioEncoderKeyHash {
    size_t operator()(const AudioEncoderKey &key) const;
};

/** @description: 将缺省参数转换为确定值，并校验编码格式和参数组合。 */
int normalizeAudioEncoderConfig(const AudioEncoderConfig &config,
                                AudioEncoderConfig *normalized);

/** @description: 根据归一化后的有效参数生成稳定的编码器去重键。 */
int makeAudioEncoderKey(const AudioEncoderConfig &config, AudioEncoderKey *key);

/** @description: 音频编码器统一 C++ 接口。 */
class AudioEncoder {
public:
    virtual ~AudioEncoder() {}

    /** @description: 返回该实例实际输出的编码格式。 */
    virtual MediaCodecType codec() const = 0;

    /** @description: 返回初始化后实际使用的 PCM 输入采样率。 */
    virtual int sampleRate() const = 0;

    /** @description: 返回初始化后实际使用的 PCM 输入通道数。 */
    virtual int channels() const = 0;

    /** @description: 编码一帧交错排列的 PCM S16LE 数据。 */
    virtual int encode(const int16_t *pcm,
                       int samples_per_channel,
                       uint64_t pts_us,
                       AudioEncoderOutput *output) = 0;

protected:
    AudioEncoder() {}

private:
    AudioEncoder(const AudioEncoder &);
    AudioEncoder &operator=(const AudioEncoder &);
};

/** @description: 根据归一化后的参数创建具体音频编码器实例。 */
std::unique_ptr<AudioEncoder> createAudioEncoder(const AudioEncoderConfig &config);

} // namespace rkmedia

#endif
