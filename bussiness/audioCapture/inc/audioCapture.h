#ifndef __AUDIO_CAPTURE_H__
#define __AUDIO_CAPTURE_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_CAPTURE_DEFAULT_DEVICE "default"
#define AUDIO_CAPTURE_DEFAULT_SAMPLE_RATE 8000
#define AUDIO_CAPTURE_DEFAULT_CHANNELS 1
#define AUDIO_CAPTURE_DEFAULT_PERIOD_FRAMES 160
#define AUDIO_CAPTURE_DEFAULT_BUFFER_PERIODS 4

typedef enum {
    AUDIO_SAMPLE_FORMAT_S16LE = 1 /* 16-bit little-endian PCM，当前采集和 G711 编码链路的标准输入格式。 */
} AudioSampleFormat;

typedef struct {
    const char *device_name;     /* ALSA PCM 设备名，例如 default 或 hw:0,0。 */
    int sample_rate;             /* 采样率，语音链路通常使用 8000Hz。 */
    int channels;                /* 声道数；当前 G711 链路要求 mono。 */
    AudioSampleFormat format;    /* PCM 样本格式。 */
    int period_frames;           /* 一个 ALSA period 包含的 PCM 帧数；当前模块每次向上层返回一个完整 period。 */
    int buffer_periods;          /* ALSA PCM 环形缓冲区期望包含的 period 数，用于平衡延迟和抗调度抖动能力。 */
} AudioCaptureConfig;

typedef struct {
    const uint8_t *data;         /* 指向 AudioCaptureCtx 内部 period_buffer，本帧下次读取前有效。 */
    size_t size;                 /* 本帧 PCM 字节数。 */
    uint64_t frame_id;           /* 采集帧序号。 */
    uint64_t pts_us;             /* 本音频帧起点时间戳，单位微秒。 */
    int sample_rate;             /* 本帧采样率。 */
    int channels;                /* 本帧声道数。 */
    AudioSampleFormat format;    /* 本帧 PCM 格式。 */
    int samples_per_channel;     /* 每个声道的采样数。 */
    uint64_t capture_call_us;    /* 完整采集调用耗时。 */
    uint64_t read_us;            /* ALSA read 阶段耗时。 */
    uint64_t xrun_count;         /* 已恢复的 ALSA underrun/overrun 次数。 */
    uint64_t capture_done_us;    /* 本 period 采集完成的单调时钟时间戳，用于端到端链路排查。 */
    uint64_t capture_interval_us;/* 相邻两个 period 采集完成时刻的间隔；首帧为 0。 */
} AudioCaptureFrame;

typedef struct {
    void *pcm_handle;            /* 私有 ALSA snd_pcm_t*，头文件中隐藏 ALSA 依赖。 */
    AudioCaptureConfig config;   /* 归一化后的实际采集配置。 */
    uint8_t *period_buffer;      /* 用户态复用缓存，与 ALSA 内核/驱动中的 PCM 环形缓冲区不是同一个 buffer。 */
    size_t period_buffer_size;   /* 用户态 period_buffer 的字节容量。 */
    int bytes_per_sample;        /* 单个采样点字节数。 */
    int frame_bytes;             /* 一个多声道采样帧的字节数。 */
    uint64_t frame_id;           /* 下一个采集帧序号来源。 */
    uint64_t xrun_count;         /* ALSA xrun 恢复计数。 */
    uint64_t pts_anchor_us;      /* 当前连续采集段首个 PCM 帧的单调时钟 PTS。 */
    uint64_t pts_frames;         /* 从 pts_anchor_us 起累计输出的每声道采样帧数。 */
    uint64_t last_capture_done_us; /* 上一个 period 的采集完成时刻，用于计算实际采集间隔。 */
    int pts_initialized;         /* PTS 采样时钟是否已经建立；xrun 恢复后重新锚定。 */
    int initialized;             /* 模块是否初始化完成。 */
} AudioCaptureCtx;

/**
 * @description: 初始化 ALSA 音频采集设备，并预分配周期缓冲区。
 * @param {AudioCaptureCtx *} ctx 音频采集上下文。
 * @param {AudioCaptureConfig *} config 输入配置，可为 NULL 使用默认值。
 * @return {int} 0 成功，-1 失败。
 */
int audio_capture_init(AudioCaptureCtx *ctx, const AudioCaptureConfig *config);

/**
 * @description: 阻塞读取一个完整音频 period，返回 PCM 帧元数据。
 * @param {AudioCaptureCtx *} ctx 音频采集上下文。
 * @param {AudioCaptureFrame *} frame 输出音频帧；data 指向 ctx 内部缓冲区。
 * @return {int} 0 成功，-1 失败。
 */
int audio_capture_read_frame(AudioCaptureCtx *ctx, AudioCaptureFrame *frame);

/**
 * @brief 获取当前累计的 ALSA XRUN 恢复次数。
 * @param ctx 音频采集上下文。
 * @return XRUN 累计次数；ctx 为空时返回 0。
 * @note 该接口使用原子读取，可由调试线程与采集线程并发调用。
 */
uint64_t audio_capture_get_xrun_count(const AudioCaptureCtx *ctx);

/**
 * @description: 关闭 ALSA 设备并释放内部缓冲区。
 * @param {AudioCaptureCtx *} ctx 音频采集上下文。
 * @return {void}
 */
void audio_capture_deinit(AudioCaptureCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
