#ifndef __AUDIO_ENCODER_H__
#define __AUDIO_ENCODER_H__

#include <stddef.h>
#include <stdint.h>

#include "mediaPacket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t AudioEncoderRuntimeGroupId;

#define AUDIO_ENCODER_INVALID_GROUP_ID ((AudioEncoderRuntimeGroupId)0)

typedef struct AudioEncoderManagerHandle AudioEncoderManagerHandle;

/** @description: 项目支持的 AAC Audio Object Type，不暴露 FDK-AAC 枚举。 */
typedef enum {
    AUDIO_ENCODER_AAC_OBJECT_TYPE_INVALID = 0, /* 未指定，归一化为 AAC-LC。 */
    AUDIO_ENCODER_AAC_OBJECT_TYPE_LC = 2,      /* AAC Low Complexity。 */
    AUDIO_ENCODER_AAC_OBJECT_TYPE_HE = 5,      /* HE-AAC，包含 SBR。 */
    AUDIO_ENCODER_AAC_OBJECT_TYPE_LD = 23,     /* ER AAC Low Delay。 */
    AUDIO_ENCODER_AAC_OBJECT_TYPE_HE_V2 = 29,  /* HE-AAC v2，包含 SBR 和 PS。 */
    AUDIO_ENCODER_AAC_OBJECT_TYPE_ELD = 39     /* ER AAC Enhanced Low Delay。 */
} AudioEncoderAacObjectType;

/** @description: Opus 编码器针对输入内容和时延目标采用的优化模式。 */
typedef enum {
    AUDIO_ENCODER_OPUS_APPLICATION_INVALID = 0,              /* 未指定，归一化为 VOIP。 */
    AUDIO_ENCODER_OPUS_APPLICATION_VOIP = 1,                 /* 优化语音清晰度和窄带语音质量。 */
    AUDIO_ENCODER_OPUS_APPLICATION_AUDIO = 2,                /* 优化音乐及一般音频质量。 */
    AUDIO_ENCODER_OPUS_APPLICATION_RESTRICTED_LOW_DELAY = 3  /* 优化极低时延，弱化语音专用处理。 */
} AudioEncoderOpusApplication;

/** @description: AAC 编码格式专用参数。 */
typedef struct {
    int bitrate;                            /* AAC 目标码率，单位 bit/s。 */
    AudioEncoderAacObjectType object_type; /* AAC Audio Object Type。 */
} AudioEncoderAacParams;

/** @description: Opus 编码格式专用参数。 */
typedef struct {
    int bitrate;             /* Opus 目标码率，单位 bit/s。 */
    int complexity;          /* Opus 编码复杂度，范围 0～10。 */
    int enable_vbr;          /* 是否启用可变码率：0=禁用，非 0=启用。 */
    int enable_fec;          /* 是否启用带内前向纠错：0=禁用，非 0=启用。 */
    int enable_dtx;          /* 是否启用非连续传输：0=禁用，非 0=启用。 */
    int packet_loss_percent; /* 预期网络丢包率，范围 0～100，单位百分比。 */
    AudioEncoderOpusApplication application; /* Opus 优化模式；INVALID 归一化为 VOIP。 */
    int max_packet_bytes;    /* 单个编码包的最大输出缓冲区大小，单位字节。 */
} AudioEncoderOpusParams;

/**
 * @description: 编码格式互斥的专用参数。
 *
 * 应根据 AudioEncoderParams.codec 只读写对应成员；G711A/G711U 不使用该联合体。
 */
typedef union {
    AudioEncoderAacParams aac;   /* codec=MEDIA_CODEC_AAC 时有效。 */
    AudioEncoderOpusParams opus; /* codec=MEDIA_CODEC_OPUS 时有效。 */
} AudioEncoderCodecParams;

/**
 * @description: C 模块注册编码组时使用的统一参数，不暴露具体编码器上下文。
 */
typedef struct {
    MediaCodecType codec;      /* 编码格式：G711A、G711U、AAC 或 Opus。 */
    int sample_rate;           /* 编码器输入采样率，单位 Hz。 */
    int channels;              /* 编码器输入及码流声道数，当前支持单声道或双声道。 */
    int max_samples_per_frame; /* 单次编码允许输入的每声道最大采样数；Opus 不使用该字段。 */
    int input_channel;         /* 单声道编码时选择的采集通道下标，从 0 开始。 */
    AudioEncoderCodecParams codec_params; /* 由 codec 判别的编码格式专用参数。 */
} AudioEncoderParams;

/**
 * @description: 一次编码调用返回的数据视图，数据在下一次同组编码前有效。
 */
typedef struct {
    const uint8_t *data;  /* 编码数据地址，由编码器实例持有，调用方不得释放或修改。 */
    size_t size;          /* 编码数据长度，单位字节；为 0 表示本次尚未产生完整编码包。 */
    uint64_t pts_us;      /* 编码包对应的起始显示时间戳，单位微秒。 */
    MediaCodecType codec; /* 实际输出编码格式，应与目标编码组的 codec 一致。 */
} AudioEncoderOutput;

/**
 * @description: 送入编码管理器的原始 PCM S16LE 帧。
 *
 * 管理器根据目标编码组完成声道选择和采样率适配，调用方无需预先转换 PCM。
 */
typedef struct {
    const int16_t *data;      /* PCM S16LE 数据地址；多声道数据按帧交错排列。 */
    int samples_per_channel;  /* 本次输入中每个声道包含的采样数，不是所有声道采样数之和。 */
    int sample_rate;          /* 输入 PCM 采样率，单位 Hz。 */
    int channels;             /* 输入 PCM 声道数，同时决定每个 PCM 帧包含的采样值数量。 */
    uint64_t pts_us;          /* 本帧第一个 PCM 采样对应的时间戳，单位微秒。 */
} AudioEncoderPcmInput;

/** @description: 创建统一编码管理器，成功时由调用方负责销毁。 */
int audio_encoder_manager_create(AudioEncoderManagerHandle **handle);

/** @description: 销毁管理器及其持有的全部去重编码器实例。 */
void audio_encoder_manager_destroy(AudioEncoderManagerHandle *handle);

/** @description: 注册编码参数；有效参数完全相同时复用已有运行时编码组。 */
int audio_encoder_manager_register(AudioEncoderManagerHandle *handle,
                                   const AudioEncoderParams *params,
                                   AudioEncoderRuntimeGroupId *group_id,
                                   int *reused);

/** @description: 返回参数去重后的实际编码组数量。 */
size_t audio_encoder_manager_group_count(const AudioEncoderManagerHandle *handle);

/** @description: 按顺序取得一个实际编码组的 ID 和归一化参数。 */
int audio_encoder_manager_group_at(const AudioEncoderManagerHandle *handle,
                                   size_t index,
                                   AudioEncoderRuntimeGroupId *group_id,
                                   AudioEncoderParams *params);

/** @description: 将一帧原始 PCM 适配并送入指定运行时编码组。 */
int audio_encoder_manager_encode(AudioEncoderManagerHandle *handle,
                                 AudioEncoderRuntimeGroupId group_id,
                                 const AudioEncoderPcmInput *input,
                                 AudioEncoderOutput *output);

#ifdef __cplusplus
}
#endif

#endif
