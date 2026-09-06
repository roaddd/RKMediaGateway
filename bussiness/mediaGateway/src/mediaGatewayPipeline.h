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
 * 单路视频编码 worker 运行期资源。
 *
 * worker 通过 poll 同时等待帧源 eventfd 和控制 eventfd。视频帧仍保存在
 * MediaFrameSource 的 latest-frame 槽中，控制命令仍保存在 command_queue 中。
 */
typedef struct {
    MediaFrameSource *source;         /* 当前 stream 独占的视频采集帧源。 */
    ThreadMessageQueue command_queue; /* 仅由对应编码线程消费的控制命令队列。 */
    pthread_mutex_t state_lock;       /* 保护 running 生命周期状态。 */
    pthread_t thread;                 /* 视频编码线程句柄。 */
    int control_event_fd;             /* MPP 控制命令和停止请求通知 fd。 */
    int running;                      /* worker 是否继续等待和编码。 */
    int ready;                        /* 命令队列、锁和 eventfd 是否初始化完成。 */
    int thread_started;               /* 视频编码线程是否已成功启动。 */
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
 * PCM 声道和采样率适配由 AudioEncoderManager 的私有实现负责。
 */
typedef struct {
    AudioFrameSource *source; /* 音频采集帧源，生命周期由 mediaGateway run resources 管理。 */
    pthread_t thread;         /* 音频编码线程句柄。 */
    int thread_started;       /* 音频编码线程是否已成功启动。 */
} MediaGatewayAudioEncodeGroup;

/*
 * mediaGateway 运行期编码 pipeline。
 *
 * pipeline 负责启动/管理视频编码线程和音频编码线程。视频编码线程通过 eventfd
 * 直接等待 MediaFrameSource；音频编码线程直接阻塞等待 AudioFrameSource。
 */
typedef struct {
    MediaGatewayCtx *ctx;                                   /* 所属 gateway 上下文，生命周期由调用方管理。 */
    MediaGatewayRunState state;                             /* 编码过程中的运行状态和失败统计。 */
    MediaGatewayVideoEncodeGroup video;                      /* 视频帧源引用、控制 eventfd 和 worker 线程资源。 */
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
 * @param video_sources 已初始化并启动的视频帧源数组。
 * @param audio_source 已初始化并启动的音频帧源；未启用音频时可以为 NULL。
 * @return 0 成功，-1 失败。
 */
int media_gateway_pipeline_init(MediaGatewayPipeline *pipeline,
                                MediaGatewayCtx *ctx,
                                ThreadMessageQueue *result_queue,
                                MediaFrameSource *video_sources,
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

#ifdef __cplusplus
}
#endif

#endif
