/*** 
 * @Author: huangkelong
 * @Date: 2026-05-09 22:46:00
 * @LastEditTime: 2026-05-19 22:40:10
 * @LastEditors: huangkelong
 * @Description: 采集层和编码处理层之间的并发调度层，负责音视频帧的线程安全转交、编码 worker 生命周期管理、丢帧策略和错误传播
 * @FilePath: \Fork\RKMediaGateway\bussiness\mediaGateway\src\mediaGatewayPipeline.h
 * @可以输入预定的版权声明、个性签名、空行等
 */

#ifndef __MEDIA_GATEWAY_PIPELINE_H__
#define __MEDIA_GATEWAY_PIPELINE_H__

#include "mediaGateway.h"

#include "audioFrameSource.h"
#include "mediaFrameSource.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 单路视频编码线程的输入槽。
 *
 * 该结构不是 FIFO 队列，而是 latest-frame 单槽缓存：主循环发布新帧，
 * 编码线程取走最新帧；如果旧帧尚未被消费就被新帧覆盖，会计入 dropped_frames。
 * 这样优先降低实时链路延迟，但 publish/acquire 会在锁内做整帧拷贝。
 */
typedef struct {
    pthread_mutex_t lock;       /* 保护 data/frame/valid/running/dropped_frames。 */
    pthread_cond_t cond;        /* 新帧到达或停止时唤醒编码线程。 */
    uint8_t *data;              /* 单槽保存的 NV12 帧副本。 */
    size_t capacity;            /* data 当前已分配容量。 */
    MediaFrame frame;           /* data 对应的帧元信息，raw_frame 指向 data。 */
    int valid;                  /* 当前槽内是否有待编码线程消费的新帧。 */
    int running;                /* 输入槽是否仍允许等待和发布。 */
    int ready;                  /* lock/cond 是否初始化成功，用于安全释放。 */
    uint64_t dropped_frames;    /* 因新帧覆盖未消费旧帧而丢弃的数量。 */
} VideoEncodeInput;

/* 音频编码 FIFO 中的一个槽位，保存一帧 PCM 副本及其元信息。 */
typedef struct {
    uint8_t *data;              /* PCM 帧副本。 */
    size_t capacity;            /* data 当前已分配容量。 */
    AudioFrame frame;           /* data 对应的音频帧元信息，data 字段指向本槽 data。 */
} AudioEncodeSlot;

/*
 * 音频编码输入 FIFO。
 *
 * 音频不能像视频一样随意只保留最新帧，否则会产生明显断音；因此这里使用小 FIFO。
 * 队列满时丢弃最旧帧，避免长期积压造成音画延迟持续扩大。
 */
typedef struct {
    pthread_mutex_t lock;       /* 保护 slots/head/size/running/dropped_frames。 */
    pthread_cond_t cond;        /* 新音频帧到达或停止时唤醒音频编码线程。 */
    AudioEncodeSlot *slots;     /* 环形 FIFO 槽位数组。 */
    int capacity;               /* FIFO 最大槽位数。 */
    int head;                   /* 当前最旧帧所在槽位下标。 */
    int size;                   /* 当前队列中待消费帧数。 */
    int running;                /* 队列是否仍允许等待和发布。 */
    int ready;                  /* lock/cond/slots 是否初始化成功，用于安全释放。 */
    uint64_t dropped_frames;    /* 队列满时被丢弃的旧音频帧数量。 */
} AudioEncodeQueue;

/*
 * mediaGateway 运行期编码 pipeline。
 *
 * pipeline 负责启动/管理视频编码线程、音频编码线程，以及它们的输入队列。
 * 采集线程不直接依赖该结构；主循环负责把采集到的帧发布到对应输入队列。
 */
typedef struct {
    MediaGatewayCtx *ctx;                                   /* 所属 gateway 上下文，生命周期由调用方管理。 */
    MediaGatewayRunState state;                             /* 编码过程中的运行状态和失败统计。 */
    VideoEncodeInput video_inputs[MEDIA_GATEWAY_MAX_STREAMS]; /* 每路视频 stream 的 latest-frame 输入槽。 */
    AudioEncodeQueue audio_queue;                           /* 音频编码线程输入 FIFO。 */
    pthread_t video_threads[MEDIA_GATEWAY_MAX_STREAMS];      /* 每路视频编码线程句柄。 */
    int video_thread_started[MEDIA_GATEWAY_MAX_STREAMS];     /* 对应视频编码线程是否已成功启动。 */
    pthread_t audio_thread;                                  /* 音频编码线程句柄。 */
    int audio_thread_started;                                /* 音频编码线程是否已成功启动。 */
    pthread_mutex_t ret_lock;                                /* 保护 ret，并在 fatal error 时同步 ctx->running。 */
    int ret_lock_ready;                                      /* ret_lock 是否初始化成功。 */
    int ret;                                                 /* pipeline 运行结果：0 正常，-1 表示工作线程遇到致命错误。 */
} MediaGatewayPipeline;

/* 创建视频编码线程时传入的参数。线程启动后会释放该对象。 */
typedef struct {
    MediaGatewayPipeline *pipeline;  /* 所属 pipeline。 */
    int stream_idx;                  /* 当前线程负责的 stream 下标。 */
} VideoEncodeThreadArg;

/*
 * 初始化编码 pipeline。
 *
 * @param pipeline 待初始化对象。
 * @param ctx      已完成配置和模块初始化的 gateway 上下文。
 * @return 0 成功，-1 失败。
 */
int media_gateway_pipeline_init(MediaGatewayPipeline *pipeline, MediaGatewayCtx *ctx);

/*
 * 启动所有启用 stream 对应的视频编码线程，以及可选音频编码线程。
 *
 * @return 0 成功，-1 失败。
 */
int media_gateway_pipeline_start_workers(MediaGatewayPipeline *pipeline);

/*
 * 停止输入队列并等待所有已启动的编码线程退出。
 */
void media_gateway_pipeline_join_workers(MediaGatewayPipeline *pipeline);

/*
 * 释放 pipeline 内部输入队列、线程同步对象和运行状态。
 * 调用前后均允许调用 join_workers；函数内部也会停止队列。
 */
void media_gateway_pipeline_deinit(MediaGatewayPipeline *pipeline);

/*
 * 获取 pipeline 运行状态。
 *
 * @return 0 表示暂无致命错误，-1 表示某个工作线程已触发 pipeline error。
 */
int media_gateway_pipeline_get_ret(MediaGatewayPipeline *pipeline);

/*
 * 将一帧视频发布到单路视频编码输入槽。
 *
 * 该函数会复制 frame->raw_frame 到 VideoEncodeInput 内部缓存。
 * 如果槽中已有未消费帧，旧帧会被覆盖并计入 dropped_frames。
 *
 * @return 0 成功或输入槽已停止，-1 表示参数或内存分配错误。
 */
int media_gateway_video_input_publish(VideoEncodeInput *input, const MediaFrame *frame);

/*
 * 将一帧 PCM 音频发布到音频编码 FIFO。
 *
 * 队列满时丢弃最旧帧，再插入新帧，避免音频延迟持续累积。
 *
 * @return 0 成功或队列已停止，-1 表示参数或内存分配错误。
 */
int media_gateway_audio_queue_publish(AudioEncodeQueue *queue, const AudioFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
