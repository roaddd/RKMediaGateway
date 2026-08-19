/**
 * @file mediaGatewayPipeline.h
 * @brief 采集层和编码处理层之间的并发调度接口。
 *
 * 本文件定义视频 latest-frame 缓存、编码 worker 生命周期管理、音视频帧转交、
 * 丢帧策略和错误传播相关接口。
 */

#ifndef __MEDIA_GATEWAY_PIPELINE_H__
#define __MEDIA_GATEWAY_PIPELINE_H__

#include "mediaGateway.h"

#include "audioFrameSource.h"
#include "mediaFrameSource.h"
#include "threadMessageQueue.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 单路视频编码线程的输入缓存。
 *
 * 该结构不是 FIFO 队列，而是 latest-frame 单槽缓存：主循环发布新帧，
 * 编码线程取走最新帧；如果旧帧尚未被消费就被新帧覆盖，会计入 dropped_frames。
 * 这样优先降低实时链路延迟；零拷贝路径下 publish/acquire 只转交采集帧引用。
 */
typedef struct {
    pthread_mutex_t lock;       /* 保护 frame/valid/running/dropped_frames。 */
    pthread_cond_t cond;        /* 新帧到达或停止时唤醒编码线程。 */
    MediaFrame frame;           /* 单槽持有的最新采集帧引用，不保存 NV12 副本。 */
    int valid;                  /* 当前槽内是否有待编码线程消费的新帧。 */
    int running;                /* 输入槽是否仍允许等待和发布。 */
    int ready;                  /* lock/cond 是否初始化成功，用于安全释放。 */
    uint64_t dropped_frames;    /* 因新帧覆盖未消费旧帧而丢弃的数量。 */
} VideoEncodeInputBuffer;

/*
 * 单路视频编码 worker 运行期资源。
 *
 * input 只负责 latest-frame 输入缓存；command_queue、thread 和 thread_started 属于 worker
 * 控制与生命周期状态，不能混入 VideoEncodeInputBuffer，避免输入缓存职责膨胀。
 */
typedef struct {
    VideoEncodeInputBuffer input;     /* 视频编码线程的 latest-frame 输入缓存。*/
    ThreadMessageQueue command_queue; /* 仅由对应编码线程消费的控制命令队列。*/
    pthread_t thread;                 /* 视频编码线程句柄。*/
    int thread_started;               /* 视频编码线程是否已成功启动。*/
} VideoEncodeWorker;

/*
 * 视频编码运行期资源集合。
 *
 * 每路 stream 对应一个 VideoEncodeWorker，worker 内部再聚合输入缓存、命令队列和线程状态。
 */
typedef struct {
    VideoEncodeWorker workers[MEDIA_GATEWAY_MAX_STREAMS]; /* 所有视频编码 worker。*/
} MediaGatewayVideoEncodeGroup;

/*
 * 音频编码运行期资源集合。
 *
 * 音频编码线程直接消费 AudioFrameSource，避免主循环搬运和重复 FIFO。
 * pcm_buffer 是线程私有的编码输入缓存：采集声道与编码声道不同时在这里完成转换。
 */
typedef struct {
    AudioFrameSource *source; /* 音频采集帧源，生命周期由 mediaGateway run resources 管理。 */
    uint8_t *pcm_buffer;      /* 转换后的编码输入 PCM，仅由音频编码线程访问。 */
    size_t pcm_capacity;      /* pcm_buffer 当前容量。 */
    pthread_t thread;         /* 音频编码线程句柄。 */
    int thread_started;       /* 音频编码线程是否已成功启动。 */
} MediaGatewayAudioEncodeGroup;

/*
 * mediaGateway 运行期编码 pipeline。
 *
 * pipeline 负责启动/管理视频编码线程和音频编码线程。视频帧仍由主循环发布到
 * latest-frame 输入槽；音频编码线程则直接阻塞等待 AudioFrameSource。
 */
typedef struct {
    MediaGatewayCtx *ctx;                                   /* 所属 gateway 上下文，生命周期由调用方管理。 */
    MediaGatewayRunState state;                             /* 编码过程中的运行状态和失败统计。 */
    MediaGatewayVideoEncodeGroup video;                      /* 视频编码输入槽和 worker 线程资源。 */
    MediaGatewayAudioEncodeGroup audio;                      /* 音频帧源引用、转换缓存和编码 worker。 */
    pthread_mutex_t ret_lock;                                /* 保护 ret，并在 fatal error 时同步 ctx->running。 */
    int ret_lock_ready;                                      /* ret_lock 是否初始化成功。 */
    int ret;                                                 /* pipeline 运行结果：0 正常，-1 表示工作线程遇到致命错误。 */
    ThreadMessageQueue *result_queue;                         /* 编码线程执行结果统一回传队列。 */
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
 * @param audio_source 已初始化并启动的音频帧源；未启用音频时可以为 NULL。
 * @return 0 成功，-1 失败。
 */
int media_gateway_pipeline_init(MediaGatewayPipeline *pipeline,
                                MediaGatewayCtx *ctx,
                                ThreadMessageQueue *result_queue,
                                AudioFrameSource *audio_source);

/**
 * @brief 非阻塞向指定视频编码线程投递控制命令。
 */
int media_gateway_pipeline_submit_video_command(MediaGatewayPipeline *pipeline,
                                                int stream_idx,
                                                const ThreadMessage *command);

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
 * 该函数会持有 frame 的 capture buffer 引用到编码线程取走或被新帧覆盖。
 * 如果槽中已有未消费帧，旧帧会被释放并计入 dropped_frames。
 *
 * @return 0 成功或输入槽已停止，-1 表示参数或内存分配错误。
 */
int media_gateway_video_input_publish(VideoEncodeInputBuffer *input, const MediaFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
