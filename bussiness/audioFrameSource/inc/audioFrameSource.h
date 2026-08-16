#ifndef __AUDIO_FRAME_SOURCE_H__
#define __AUDIO_FRAME_SOURCE_H__

#include <pthread.h>
#include <stddef.h>

#include "audioCapture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_FRAME_SOURCE_DEFAULT_SLOTS 8
#define AUDIO_FRAME_SOURCE_MAX_SLOTS 32

/*
 * 音频帧在采集、帧源和编码输入队列之间流转时携带的单调时钟时间点。
 * 各字段只用于诊断实际调度间隔，不参与音频 PTS 或 RTP 时间戳计算。
 */
typedef struct {
    uint64_t capture_done_us;         /* ALSA 完成一个完整 period 的时刻。 */
    uint64_t capture_interval_us;     /* 相邻 period 实际采集完成间隔。 */
    uint64_t source_publish_us;       /* 采集线程发布到 AudioFrameSource ring 的时刻。 */
    uint64_t source_acquire_us;       /* gateway 从 AudioFrameSource ring 取出的时刻。 */
    uint64_t encode_queue_enqueue_us; /* gateway 放入音频编码 FIFO 的时刻。 */
    uint64_t encode_queue_dequeue_us; /* 音频编码线程从 FIFO 取出的时刻。 */
} AudioFramePathTiming;

typedef struct {
    const uint8_t *data;              /* 指向 ring slot 内部数据，release 前有效。 */
    size_t size;                      /* PCM 字节数。 */
    uint64_t frame_id;                /* 音频采集帧序号。 */
    uint64_t pts_us;                  /* 音频帧起点 PTS，单位微秒。 */
    int sample_rate;                  /* 采样率。 */
    int channels;                     /* 声道数。 */
    AudioSampleFormat format;         /* PCM 格式。 */
    int samples_per_channel;          /* 每个声道采样数。 */
    uint64_t capture_call_us;         /* 采集调用耗时。 */
    uint64_t read_us;                 /* 底层设备读取耗时。 */
    uint64_t xrun_count;              /* 采集模块累计 xrun 次数。 */
    AudioFramePathTiming path_timing; /* 随帧传递的音频链路诊断时间点。 */
} AudioFrame;

typedef struct {
    uint8_t *data;                    /* 预分配槽位数据区。 */
    size_t capacity;                  /* 槽位容量。 */
    AudioFrame frame;                 /* 槽位当前保存的音频帧元数据。 */
    uint64_t seq;                     /* ring 内递增序号，用于选择最旧可读帧。 */
    int valid;                        /* 槽位是否有待消费数据。 */
    int in_use;                       /* gateway 是否正在使用该槽位。 */
} AudioFrameSourceSlot;

typedef struct {
    AudioCaptureCtx *capture;         /* 外部音频采集上下文，生命周期由 gateway 管理。 */
    pthread_t thread;                 /* 独立采集线程。 */
    pthread_mutex_t lock;             /* 保护 ring 状态和运行标志。 */
    pthread_cond_t cond;              /* 新帧到达或停止时唤醒消费者。 */

    AudioFrameSourceSlot *slots;      /* 固定大小 ring slot 数组。 */
    int slot_count;                   /* ring slot 数量。 */
    int read_slot;                    /* 当前最旧可读槽位，-1 表示暂无。 */
    uint64_t next_seq;                /* 下一个发布帧序号。 */
    uint64_t dropped_frames;          /* 因消费者积压被覆盖/丢弃的帧数。 */

    int retry_ms;                     /* 采集失败后的退避时间。 */
    int max_consecutive_failures;     /* 连续失败达到该值进入 fatal。 */
    int consecutive_failures;         /* 当前连续失败次数。 */
    int running;                      /* 采集线程运行标志。 */
    int started;                      /* 线程是否已成功启动。 */
    int fatal_error;                  /* 是否发生不可恢复错误。 */
    int mutex_ready;                  /* mutex 是否已初始化，用于安全 deinit。 */
    int cond_ready;                   /* cond 是否已初始化，用于安全 deinit。 */
} AudioFrameSource;

/**
 * @description: 初始化音频帧源，预分配 ring slot，但不启动采集线程。
 */
int audio_frame_source_init(AudioFrameSource *source,
                            AudioCaptureCtx *capture,
                            int slot_count,
                            int retry_ms,
                            int max_consecutive_failures);

/**
 * @description: 启动独立音频采集线程。
 */
int audio_frame_source_start(AudioFrameSource *source);

/**
 * @description: 从 ring 中获取最旧的待消费音频帧，超时返回 0。
 */
int audio_frame_source_acquire(AudioFrameSource *source,
                               AudioFrame *frame,
                               int *slot_index,
                               int timeout_ms);

/**
 * @description: 释放 acquire 得到的槽位，允许采集线程复用。
 */
void audio_frame_source_release(AudioFrameSource *source, int slot_index);

/**
 * @description: 停止采集线程并等待退出。
 */
void audio_frame_source_stop(AudioFrameSource *source);

/**
 * @description: 停止线程并释放 ring slot、同步原语。
 */
void audio_frame_source_deinit(AudioFrameSource *source);

/**
 * @description: 读取累计丢帧数。
 */
uint64_t audio_frame_source_get_dropped_frames(AudioFrameSource *source);

#ifdef __cplusplus
}
#endif

#endif
