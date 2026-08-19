/**
 * @file mediaGatewayPipeline.c
 * @brief MediaGateway 编码流水线、视频输入队列和音视频编码线程控制。
 *
 * 每路编码线程拥有独立控制命令队列。MPP 编码和运行期配置修改都在对应
 * 编码线程内串行执行，避免外部线程并发访问同一个 MPP 编码上下文。
 */

#include "mediaGatewayPipeline.h"

#include "commonDef.h"
#include "mediaControlMessage.h"
#include "mediaGatewayProcess.h"

#include "logger.h"
#include "util.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VIDEO_COMMAND_QUEUE_CAPACITY 8

static uint64_t pipeline_now_us(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void pipeline_set_thread_name(const char *prefix, const char *name, int index)
{
    char thread_name[16] = {0};

    if (!prefix)
    {
        LOG_ERROR("pipeline_set_thread_name failed: prefix is NULL index=%d", index);
        return;
    }
    if (name && name[0] != '\0')
        snprintf(thread_name, sizeof(thread_name), "%s%.11s", prefix, name);
    else
        snprintf(thread_name, sizeof(thread_name), "%s%d", prefix, index);
    util_set_thread_name(thread_name);
}

/**
 * @description: 生成 pthread 条件变量使用的绝对超时时间。
 */
static void make_abs_timeout(struct timespec *ts, int timeout_ms)
{
    long nsec = 0;
    int ret = 0;

    if (!ts)
    {
        LOG_ERROR("make_abs_timeout failed: ts is NULL timeout_ms=%d", timeout_ms);
        return;
    }
    ret = clock_gettime(CLOCK_REALTIME, ts);
    if (ret != MEDIA_OK)
    {
        LOG_ERROR("make_abs_timeout failed: clock_gettime errno=%d(%s)",
                  errno,
                  strerror(errno));
        memset(ts, 0, sizeof(*ts));
        return;
    }
    if (timeout_ms < 0)
        timeout_ms = 0;
    ts->tv_sec += timeout_ms / 1000;
    nsec = ts->tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    ts->tv_sec += nsec / 1000000000L;
    ts->tv_nsec = nsec % 1000000000L;
}

/**
 * @description: 标记 pipeline 失败并请求 gateway 退出。
 */
static void pipeline_set_error(MediaGatewayPipeline *pipeline)
{
    if (!pipeline || !pipeline->ctx || !pipeline->ret_lock_ready)
    {
        LOG_ERROR("pipeline_set_error failed: pipeline=%p ctx=%p ret_lock_ready=%d",
                  (void *)pipeline,
                  pipeline ? (void *)pipeline->ctx : NULL,
                  pipeline ? pipeline->ret_lock_ready : 0);
        return;
    }
    pthread_mutex_lock(&pipeline->ret_lock);
    pipeline->ret = -1;
    pipeline->ctx->running = 0;
    pthread_mutex_unlock(&pipeline->ret_lock);
}

/**
 * @description: 读取 pipeline 当前错误状态。
 */
int media_gateway_pipeline_get_ret(MediaGatewayPipeline *pipeline)
{
    int ret = 0;
    if (!pipeline)
    {
        LOG_ERROR("media_gateway_pipeline_get_ret failed: pipeline is NULL");
        return -1;
    }
    if (!pipeline->ret_lock_ready)
    {
        LOG_ERROR("media_gateway_pipeline_get_ret failed: ret lock not ready pipeline=%p ret=%d",
                  (void *)pipeline,
                  pipeline->ret);
        return pipeline->ret;
    }
    pthread_mutex_lock(&pipeline->ret_lock);
    ret = pipeline->ret;
    pthread_mutex_unlock(&pipeline->ret_lock);
    return ret;
}

/**
 * @description: 初始化单路视频 latest-frame 输入缓存。
 */
static int video_input_buffer_init(VideoEncodeInputBuffer *input)
{
    int mutex_ret = 0;
    int cond_ret = 0;

    if (!input)
    {
        LOG_ERROR("video_input_buffer_init failed: input is NULL");
        return -1;
    }
    memset(input, 0, sizeof(*input));
    mutex_ret = pthread_mutex_init(&input->lock, NULL);
    if (mutex_ret != 0)
    {
        LOG_ERROR("video_input_buffer_init failed: pthread_mutex_init ret=%d(%s)",
                  mutex_ret,
                  strerror(mutex_ret));
        return -1;
    }
    cond_ret = pthread_cond_init(&input->cond, NULL);
    if (cond_ret != 0)
    {
        LOG_ERROR("video_input_buffer_init failed: pthread_cond_init ret=%d(%s)",
                  cond_ret,
                  strerror(cond_ret));
        pthread_mutex_destroy(&input->lock);
        return -1;
    }
    input->running = 1;
    input->ready = 1;
    return 0;
}

/**
 * @description: 停止单路视频 latest-frame 输入缓存并唤醒等待线程。
 */
static void video_input_buffer_stop(VideoEncodeInputBuffer *input)
{
    if (!input)
    {
        LOG_ERROR("video_input_buffer_stop failed: input is NULL");
        return;
    }
    if (!input->ready)
        return;
    pthread_mutex_lock(&input->lock);
    input->running = 0;
    pthread_cond_broadcast(&input->cond);
    pthread_mutex_unlock(&input->lock);
}

/**
 * @description: 释放单路视频 latest-frame 输入缓存。
 */
static void video_input_buffer_deinit(VideoEncodeInputBuffer *input)
{
    int cond_ret = 0;
    int mutex_ret = 0;

    if (!input)
    {
        LOG_ERROR("video_input_buffer_deinit failed: input is NULL");
        return;
    }
    video_input_buffer_stop(input);
    media_frame_reset(&input->frame);
    if (input->ready)
    {
        cond_ret = pthread_cond_destroy(&input->cond);
        if (cond_ret != 0)
        {
            LOG_ERROR("video_input_buffer_deinit failed: pthread_cond_destroy ret=%d(%s)",
                      cond_ret,
                      strerror(cond_ret));
        }
        mutex_ret = pthread_mutex_destroy(&input->lock);
        if (mutex_ret != 0)
        {
            LOG_ERROR("video_input_buffer_deinit failed: pthread_mutex_destroy ret=%d(%s)",
                      mutex_ret,
                      strerror(mutex_ret));
        }
    }
    memset(input, 0, sizeof(*input));
}

/**
 * @brief 初始化单路视频编码 worker 的输入缓存和控制命令队列。
 */
static int video_worker_init(VideoEncodeWorker *worker)
{
    int queue_ret = 0;

    if (!worker)
    {
        LOG_ERROR("video_worker_init failed: worker is NULL");
        return -1;
    }

    memset(worker, 0, sizeof(*worker));
    if (video_input_buffer_init(&worker->input) != 0)
    {
        LOG_ERROR("video_worker_init failed: init input buffer");
        return -1;
    }

    queue_ret = thread_message_queue_init(&worker->command_queue,
                                          VIDEO_COMMAND_QUEUE_CAPACITY);
    if (queue_ret != MEDIA_OK)
    {
        LOG_ERROR("video_worker_init failed: init command queue ret=%d", queue_ret);
        video_input_buffer_deinit(&worker->input);
        return -1;
    }
    return 0;
}

/**
 * @brief 停止单路视频编码 worker 的输入缓存和控制命令队列。
 */
static void video_worker_stop(VideoEncodeWorker *worker)
{
    if (!worker)
    {
        LOG_ERROR("video_worker_stop failed: worker is NULL");
        return;
    }
    if (!worker->input.ready)
        return;

    video_input_buffer_stop(&worker->input);
    thread_message_queue_stop(&worker->command_queue);
}

/**
 * @brief 释放单路视频编码 worker 的输入缓存和控制命令队列。
 */
static void video_worker_deinit(VideoEncodeWorker *worker)
{
    if (!worker)
    {
        LOG_ERROR("video_worker_deinit failed: worker is NULL");
        return;
    }

    if (!worker->input.ready)
        return;
    video_worker_stop(worker);
    video_input_buffer_deinit(&worker->input);
    thread_message_queue_deinit(&worker->command_queue);
    memset(worker, 0, sizeof(*worker));
}

/**
 * @brief 非阻塞向指定视频编码线程投递控制命令。
 */
int media_gateway_pipeline_submit_video_command(MediaGatewayPipeline *pipeline,
                                                int stream_idx,
                                                const ThreadMessage *command)
{
    VideoEncodeWorker *worker = NULL;
    VideoEncodeInputBuffer *input = NULL;
    int ret = 0;

    if (!pipeline || !command ||
        stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("media_gateway_pipeline_submit_video_command failed: pipeline=%p command=%p stream=%d",
                  (void *)pipeline,
                  (const void *)command,
                  stream_idx);
        return -1;
    }

    worker = &pipeline->video.workers[stream_idx];
    input = &worker->input;
    if (!input->ready)
    {
        LOG_ERROR("media_gateway_pipeline_submit_video_command failed: input not ready stream=%d",
                  stream_idx);
        return -1;
    }
    ret = thread_message_queue_push_copy(&worker->command_queue, command);
    if (ret != MEDIA_OK)
    {
        LOG_ERROR("media_gateway_pipeline_submit_video_command failed: stream=%d type=%u request=%" PRIu64 " ret=%d",
                  stream_idx,
                  command->type,
                  command->request_id,
                  ret);
        return ret;
    }
    pthread_mutex_lock(&input->lock);
    pthread_cond_signal(&input->cond);
    pthread_mutex_unlock(&input->lock);
    return ret;
}

/**
 * @description: 将视频帧引用发布到 latest-frame 输入缓存。
 * 当前设计目标是降低实时链路延迟，宁可丢旧帧，也不让视频积压。
 * TODO: 如果后续要做录像，需要改成不丢帧的方式。
 */
int media_gateway_video_input_publish(VideoEncodeInputBuffer *input, const MediaFrame *frame)
{
    if (!input || !frame || !frame->raw_frame || frame->raw_len <= 0 || !frame->capture_buffer)
    {
        LOG_ERROR("media_gateway_video_input_publish failed: invalid args input=%p frame=%p raw=%p len=%d",
                  (void *)input,
                  (const void *)frame,
                  frame ? (const void *)frame->raw_frame : NULL,
                  frame ? frame->raw_len : 0);
        return -1;
    }

    pthread_mutex_lock(&input->lock);
    if (!input->running)
    {
        pthread_mutex_unlock(&input->lock);
        return 0;
    }
    if (input->valid) {
        media_frame_reset(&input->frame);
        input->dropped_frames++;
    }
    media_frame_copy_ref(&input->frame, frame);
    input->frame.metrics.video_input_publish_copy_us = 0;
    input->valid = 1;
    pthread_cond_signal(&input->cond);
    pthread_mutex_unlock(&input->lock);
    return 0;
}

/**
 * @description: 编码 worker 从 latest-frame 输入缓存获取一帧引用。
 */
static int video_worker_acquire_frame(VideoEncodeWorker *worker,
                                      MediaFrame *frame,
                                      int timeout_ms)
{
    struct timespec ts = {0};
    VideoEncodeInputBuffer *input = NULL;
    int wait_ret = 0;

    if (!worker || !frame)
    {
        LOG_ERROR("video_worker_acquire_frame failed: invalid args worker=%p frame=%p",
                  (void *)worker,
                  (void *)frame);
        return -1;
    }

    input = &worker->input;
    if (!input->ready)
    {
        LOG_ERROR("video_worker_acquire_frame failed: input not ready");
        return -1;
    }

    make_abs_timeout(&ts, timeout_ms);

    pthread_mutex_lock(&input->lock);
    while (input->running && !input->valid &&
           !thread_message_queue_has_messages(&worker->command_queue))
    {
        wait_ret = pthread_cond_timedwait(&input->cond, &input->lock, &ts);
        if (wait_ret == ETIMEDOUT)
        {
            pthread_mutex_unlock(&input->lock);
            return 0;
        }
        if (wait_ret != 0)
        {
            LOG_ERROR("video_worker_acquire_frame failed: pthread_cond_timedwait ret=%d(%s)",
                      wait_ret,
                      strerror(wait_ret));
            pthread_mutex_unlock(&input->lock);
            return -1;
        }
    }
    if (!input->valid)
    {
        pthread_mutex_unlock(&input->lock);
        return 0;
    }

    media_frame_copy_ref(frame, &input->frame);
    frame->metrics.video_input_acquire_copy_us = 0;
    media_frame_reset(&input->frame);
    input->valid = 0;
    pthread_mutex_unlock(&input->lock);
    return 1;
}

/**
 * @brief 将编码线程命令执行结果发送到 gateway 统一结果队列。
 */
static void video_worker_publish_command_result(MediaGatewayPipeline *pipeline,
                                                int stream_idx,
                                                const ThreadMessage *command,
                                                int status,
                                                const void *data,
                                                size_t data_size)
{
    ThreadMessage result = {0};
    int push_ret = 0;

    if (!pipeline || !command || !pipeline->result_queue)
    {
        LOG_ERROR("video_worker_publish_command_result failed: pipeline=%p command=%p result_queue=%p stream=%d",
                  (void *)pipeline,
                  (const void *)command,
                  pipeline ? (void *)pipeline->result_queue : NULL,
                  stream_idx);
        return;
    }

    result.type = command->type;
    result.request_id = command->request_id;
    result.endpoint_type = MEDIA_CONTROL_ENDPOINT_ENCODER;
    result.endpoint_index = stream_idx;
    result.status = status;
    result.data = (void *)data;
    result.data_size = data_size;
    /* 将结果发送到主循环的统一结果队列 */
    push_ret = thread_message_queue_push_copy(pipeline->result_queue, &result);
    if (push_ret != MEDIA_OK)
    {
        LOG_ERROR("video worker publish command result failed stream=%d request=%" PRIu64 " ret=%d",
                  stream_idx,
                  command->request_id,
                  push_ret);
    }
}

/**
 * @brief 在编码线程中执行设置帧率命令。
 */
static void video_worker_execute_set_fps(MediaGatewayPipeline *pipeline,
                                         int stream_idx,
                                         const ThreadMessage *command)
{
    const MediaSetFpsRequest *request = NULL;
    MediaSetFpsResult result = {0};
    int ret = -1;

    if (!pipeline || !command ||
        command->data_size != sizeof(MediaSetFpsRequest) ||
        !command->data)
    {
        LOG_ERROR("video_worker_execute_set_fps failed: pipeline=%p command=%p stream=%d data=%p data_size=%zu expected=%zu",
                  (void *)pipeline,
                  (const void *)command,
                  stream_idx,
                  command ? command->data : NULL,
                  command ? command->data_size : 0,
                  sizeof(MediaSetFpsRequest));
        if (pipeline && command)
            video_worker_publish_command_result(pipeline, stream_idx, command, MEDIA_ERR_INVALID_PARAM, NULL, 0);
        return;
    }

    request = (const MediaSetFpsRequest *)command->data;
    result.requested_fps = request->target_fps;
    ret = mpp_encoder_set_fps(&pipeline->ctx->encoders[stream_idx],
                              request->target_fps);
    if (ret == MEDIA_OK)
    {
        result.applied_fps = request->target_fps;
        /*
         * 流配置由所属编码线程更新，避免 gateway 主循环与编码线程并发
         * 读写同一路 stream 配置。
         */
        pipeline->ctx->config.video.streams[stream_idx].fps = request->target_fps;
        ret = mpp_encoder_request_idr(&pipeline->ctx->encoders[stream_idx]);
    }
    video_worker_publish_command_result(pipeline,
                                        stream_idx,
                                        command,
                                        ret,
                                        &result,
                                        sizeof(result));
}

/**
 * @brief 在编码线程中执行完整视频编码运行参数命令。
 * 该命令用于联合控制输出，避免只改 fps 而遗漏码率、GOP、RC 和 QP。
 */
static void video_worker_execute_set_video_encode_params(MediaGatewayPipeline *pipeline,
                                                   int stream_idx,
                                                   const ThreadMessage *command)
{
    const MediaSetVideoEncodeParamsRequest *request = NULL;
    MediaSetVideoEncodeParamsResult result = {0};
    MediaGatewayStreamConfig *stream = NULL;
    int ret = -1;

    if (!pipeline || !command ||
        command->data_size != sizeof(MediaSetVideoEncodeParamsRequest) ||
        !command->data)
    {
        LOG_ERROR("video_worker_execute_set_video_encode_params failed: pipeline=%p command=%p stream=%d data=%p data_size=%zu expected=%zu",
                  (void *)pipeline,
                  (const void *)command,
                  stream_idx,
                  command ? command->data : NULL,
                  command ? command->data_size : 0,
                  sizeof(MediaSetVideoEncodeParamsRequest));
        if (pipeline && command)
            video_worker_publish_command_result(pipeline, stream_idx, command, MEDIA_ERR_INVALID_PARAM, NULL, 0);
        return;
    }

    request = (const MediaSetVideoEncodeParamsRequest *)command->data;
    result.requested_params = request->params;
    ret = mpp_encoder_apply_video_encode_params(&pipeline->ctx->encoders[stream_idx],
                                                &request->params);
    if (ret == MEDIA_OK)
    {
        result.applied_params = request->params;
        stream = &pipeline->ctx->config.video.streams[stream_idx];
        stream->fps = request->params.fps;
        stream->bitrate = request->params.bitrate;
        stream->gop = request->params.gop;
        stream->rc_mode = request->params.rc_mode;
        stream->qp_init = request->params.qp_init;
        stream->qp_min = request->params.qp_min;
        stream->qp_max = request->params.qp_max;
        stream->qp_min_i = request->params.qp_min_i;
        stream->qp_max_i = request->params.qp_max_i;
        stream->qp_max_step = request->params.qp_max_step;
        ret = mpp_encoder_request_idr(&pipeline->ctx->encoders[stream_idx]);
    }
    video_worker_publish_command_result(pipeline,
                                        stream_idx,
                                        command,
                                        ret,
                                        &result,
                                        sizeof(result));
}

/**
 * @brief 处理指定编码线程当前积压的控制命令。
 */
static void video_worker_process_commands(MediaGatewayPipeline *pipeline,
                                          int stream_idx)
{
    ThreadMessage command = {0};
    VideoEncodeWorker *worker = NULL;
    int pop_ret = 0;
    int processed = 0;

    if (!pipeline || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("video_worker_process_commands failed: pipeline=%p stream=%d",
                  (void *)pipeline,
                  stream_idx);
        return;
    }

    worker = &pipeline->video.workers[stream_idx]; /* 编码线程运行期资源 */
    while (processed < VIDEO_COMMAND_QUEUE_CAPACITY)
    {
        /* 从控制命令队列中尝试弹出一条命令 */
        pop_ret = thread_message_queue_try_pop(&worker->command_queue, &command);
        if (pop_ret < 0)
        {
            if (pop_ret != MEDIA_ERR_STOPPED)
            {
                LOG_ERROR("video_worker_process_commands failed: pop command stream=%d ret=%d",
                          stream_idx,
                          pop_ret);
            }
            break;
        }
        if (pop_ret == 0)
            break;
        switch (command.type)
        {
        case MEDIA_CONTROL_MESSAGE_SET_FPS:
            /* 改变帧率 */
            video_worker_execute_set_fps(pipeline, stream_idx, &command);
            break;
        case MEDIA_CONTROL_MESSAGE_SET_VIDEO_ENCODE_PARAMS:
            /* 应用联合控制输出的完整编码运行参数。 */
            video_worker_execute_set_video_encode_params(pipeline, stream_idx, &command);
            break;
        default:
            LOG_WARN("video worker ignored unknown command type=%u stream=%d",
                     command.type,
                     stream_idx);
            video_worker_publish_command_result(pipeline, stream_idx, &command, MEDIA_ERR_UNSUPPORTED, NULL, 0);
            break;
        }
        thread_message_release(&command);
        processed++;
    }
}

/**
 * @description: 视频编码 worker 主函数。
 */
static void *video_encode_thread_main(void *arg)
{
    VideoEncodeThreadArg *thread_arg = (VideoEncodeThreadArg *)arg;
    MediaGatewayPipeline *pipeline = NULL;
    int stream_idx = -1;
    MediaFrame frame = {0};
    int ret = 0;

    if (!thread_arg)
    {
        LOG_ERROR("video_encode_thread_main failed: arg is NULL");
        return NULL;
    }
    pipeline = thread_arg->pipeline;
    stream_idx = thread_arg->stream_idx;
    if (!pipeline || !pipeline->ctx ||
        stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("video_encode_thread_main failed: pipeline=%p ctx=%p stream=%d",
                  (void *)pipeline,
                  pipeline ? (void *)pipeline->ctx : NULL,
                  stream_idx);
        free(thread_arg);
        return NULL;
    }

    pipeline_set_thread_name("enc-", pipeline->ctx->config.video.streams[stream_idx].name, stream_idx);
    free(thread_arg);
    while (pipeline->ctx->running)
    {
        /* MPP 控制命令与编码调用始终在当前编码线程内串行执行。 */
        video_worker_process_commands(pipeline, stream_idx);
        /*
         * VideoEncodeInputBuffer 是单槽 latest-frame 输入缓存，不做 FIFO 排队。
         * 编码线程慢时新帧会覆盖未消费旧帧，优先降低端到端延迟；主要成本是 publish/acquire 都在锁内整帧拷贝。
         */
        ret = video_worker_acquire_frame(&pipeline->video.workers[stream_idx],
                                         &frame,
                                         100);
        if (ret < 0)
        {
            LOG_ERROR("video encode thread failed: acquire stream=%d", stream_idx);
            pipeline_set_error(pipeline);
            break;
        }
        if (ret == MEDIA_OK)
            continue;
        video_worker_process_commands(pipeline, stream_idx);
        if (media_gateway_process_stream(pipeline->ctx, &pipeline->state, &frame, stream_idx) != 0)
        {
            LOG_ERROR("video encode thread failed: process stream=%d frame=%" PRIu64,
                      stream_idx,
                      frame.frame_id);
            pipeline_set_error(pipeline);
            media_frame_reset(&frame);
            break;
        }
        media_frame_reset(&frame);
    }
    return NULL;
}

/**
 * @description: 把采集帧转换成编码器要求的声道布局，并复制到音频 worker 私有缓存。
 *
 * 当前支持两类路径：采集/编码声道一致时原样复制；双声道采集、单声道编码时，
 * 从交织 PCM 中选择 input_channel。转换完成后源 ring slot 就可以立即释放，
 * 避免编码或输出入队阶段占用采集缓存。
 */
static int audio_prepare_encoder_frame(MediaGatewayPipeline *pipeline,
                                       const AudioFrame *source_frame,
                                       AudioFrame *encoder_frame)
{
    MediaGatewayAudioEncodeGroup *audio = NULL;
    const int16_t *source_samples = NULL;
    int16_t *encoder_samples = NULL;
    uint8_t *new_buffer = NULL;
    size_t source_size = 0;
    size_t output_size = 0;
    int encoder_channels = 0;
    int input_channel = 0;
    int i = 0;

    /*
     * 第一步：校验调用参数和基础帧信息。
     * source_frame->data 仍指向 AudioFrameSource 的 ring slot，调用方必须在
     * 本函数返回后才能 release 对应 slot。
     */
    if (!pipeline || !pipeline->ctx || !source_frame || !encoder_frame ||
        !source_frame->data || source_frame->samples_per_channel <= 0)
    {
        LOG_ERROR("audio_prepare_encoder_frame failed: invalid args pipeline=%p frame=%p data=%p samples=%d",
                  (void *)pipeline,
                  (const void *)source_frame,
                  source_frame ? (const void *)source_frame->data : NULL,
                  source_frame ? source_frame->samples_per_channel : 0);
        return -1;
    }

    /*
     * 当前声道转换只处理 S16LE 交织 PCM，支持单声道和双声道采集。
     * 双声道样本排列为：ch0_sample0、ch1_sample0、ch0_sample1、ch1_sample1……
     */
    if (source_frame->format != AUDIO_SAMPLE_FORMAT_S16LE ||
        (source_frame->channels != 1 && source_frame->channels != 2))
    {
        LOG_ERROR("audio_prepare_encoder_frame failed: unsupported format=%d capture_channels=%d",
                  source_frame->format,
                  source_frame->channels);
        return -1;
    }

    /*
     * 第二步：根据“每声道采样数”和声道数计算输入、输出 PCM 字节数。
     * samples_per_channel 表示 ALSA PCM frame 数，声道转换不会改变它，也不会
     * 改变本帧时长和 PTS，只会改变每个 PCM frame 包含的样本数量。
     */
    audio = &pipeline->audio;
    encoder_channels = pipeline->ctx->config.audio.source.encoder.channels;
    input_channel = pipeline->ctx->config.audio.source.encoder.input_channel;
    source_size = (size_t)source_frame->samples_per_channel *
                  (size_t)source_frame->channels * sizeof(int16_t);
    output_size = (size_t)source_frame->samples_per_channel *
                  (size_t)encoder_channels * sizeof(int16_t);
    if (source_frame->size < source_size || encoder_channels <= 0)
    {
        LOG_ERROR("audio_prepare_encoder_frame failed: invalid PCM size=%zu expected=%zu encoder_channels=%d",
                  source_frame->size,
                  source_size,
                  encoder_channels);
        return -1;
    }

    /*
     * 第三步：确认声道转换组合受支持。
     * 当前允许：1→1、2→2 原样复制，以及 2→1 选择一个有效采集声道；
     * 当前不做 1→2 复制扩展，也不做左右声道混音。
     */
    if (source_frame->channels != encoder_channels &&
        !(source_frame->channels == 2 && encoder_channels == 1 &&
          input_channel >= 0 && input_channel < source_frame->channels))
    {
        LOG_ERROR("audio_prepare_encoder_frame failed: unsupported channel conversion capture=%d encoder=%d input_channel=%d",
                  source_frame->channels,
                  encoder_channels,
                  input_channel);
        return -1;
    }

    /*
     * 第四步：扩容音频编码线程私有缓存。
     * 缓存只在容量不足时 realloc，正常逐帧处理不会反复申请内存。
     */
    if (audio->pcm_capacity < output_size)
    {
        new_buffer = (uint8_t *)realloc(audio->pcm_buffer, output_size);
        if (!new_buffer)
        {
            LOG_ERROR("audio_prepare_encoder_frame failed: realloc size=%zu", output_size);
            return -1;
        }
        audio->pcm_buffer = new_buffer;
        audio->pcm_capacity = output_size;
    }

    if (source_frame->channels == encoder_channels)
    {
        /* 采集与编码声道布局一致，直接复制完整 PCM 帧。 */
        memcpy(audio->pcm_buffer, source_frame->data, output_size);
    }
    else
    {
        /*
         * 第五步：双声道采集转单声道编码。
         * 从每个交织 PCM frame 中提取 input_channel 指定的一个样本。
         * RK809 当前配置使用 input_channel=0，静音的另一个通道不会进入编码器。
         */
        source_samples = (const int16_t *)source_frame->data;
        encoder_samples = (int16_t *)audio->pcm_buffer;
        for (i = 0; i < source_frame->samples_per_channel; ++i)
            encoder_samples[i] = source_samples[i * source_frame->channels + input_channel];
    }

    /*
     * 第六步：保留采集帧的 PTS、frame_id 和链路时间信息，只替换编码输入数据、
     * 字节数及声道数。此后调用方可以释放源 ring slot，编码器仅访问私有缓存。
     */
    *encoder_frame = *source_frame;
    encoder_frame->data = audio->pcm_buffer;
    encoder_frame->size = output_size;
    encoder_frame->channels = encoder_channels;
    encoder_frame->path_timing.encoder_input_ready_us = pipeline_now_us();
    return 0;
}

/**
 * @description: 音频编码 worker 直接阻塞等待 AudioFrameSource，不再由 gateway 主循环搬运。
 */
static void *audio_encode_thread_main(void *arg)
{
    MediaGatewayPipeline *pipeline = (MediaGatewayPipeline *)arg;
    AudioFrame source_frame = {0};
    AudioFrame encoder_frame = {0};
    int source_slot = -1;
    int ret = 0;

    if (!pipeline || !pipeline->ctx || !pipeline->audio.source)
    {
        LOG_ERROR("audio_encode_thread_main failed: pipeline=%p ctx=%p source=%p",
                  (void *)pipeline,
                  pipeline ? (void *)pipeline->ctx : NULL,
                  pipeline ? (void *)pipeline->audio.source : NULL);
        return NULL;
    }
    pipeline_set_thread_name("enc-", "audio", 0);
    while (pipeline->ctx->running)
    {
        source_slot = -1;
        ret = audio_frame_source_acquire(pipeline->audio.source,
                                         &source_frame,
                                         &source_slot,
                                         100);
        if (ret < 0)
        {
            /* stop 路径可能与帧源 fatal 状态同时发生，退出时不再升级为 pipeline 错误。 */
            if (pipeline->ctx->running)
            {
                LOG_ERROR("audio encode thread failed: acquire source");
                pipeline_set_error(pipeline);
            }
            break;
        }
        if (ret == MEDIA_OK)
            continue;
        if (!pipeline->ctx->running)
        {
            /* stop 唤醒时 ring 中可能还有未消费帧，退出前只释放 slot，不再发送尾包。 */
            audio_frame_source_release(pipeline->audio.source, source_slot);
            source_slot = -1;
            break;
        }

        ret = audio_prepare_encoder_frame(pipeline, &source_frame, &encoder_frame);
        audio_frame_source_release(pipeline->audio.source, source_slot);
        source_slot = -1;
        if (ret != 0)
        {
            LOG_ERROR("audio encode thread failed: prepare frame=%" PRIu64, source_frame.frame_id);
            pipeline_set_error(pipeline);
            break;
        }
        if (media_gateway_process_audio(pipeline->ctx, &encoder_frame) != 0)
        {
            LOG_ERROR("audio encode thread failed: process frame=%" PRIu64, encoder_frame.frame_id);
            pipeline_set_error(pipeline);
            break;
        }
    }
    if (source_slot >= 0)
        audio_frame_source_release(pipeline->audio.source, source_slot);
    return NULL;
}

/**
 * @description: 初始化 pipeline 的视频输入槽、音频帧源引用和同步状态。
 */
int media_gateway_pipeline_init(MediaGatewayPipeline *pipeline,
                                MediaGatewayCtx *ctx,
                                ThreadMessageQueue *result_queue,
                                AudioFrameSource *audio_source)
{
    int i = 0;
    int mutex_ret = 0;

    if (!pipeline || !ctx || !result_queue ||
        (ctx->config.audio.source.enabled && ctx->audio_capture_ready && !audio_source))
    {
        LOG_ERROR("media_gateway_pipeline_init failed: pipeline=%p ctx=%p result_queue=%p audio_source=%p",
                  (void *)pipeline,
                  (void *)ctx,
                  (void *)result_queue,
                  (void *)audio_source);
        return -1;
    }
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->ctx = ctx;
    pipeline->result_queue = result_queue;
    mutex_ret = pthread_mutex_init(&pipeline->ret_lock, NULL);
    if (mutex_ret != 0)
    {
        LOG_ERROR("media_gateway_pipeline_init failed: pthread_mutex_init ret=%d(%s)",
                  mutex_ret,
                  strerror(mutex_ret));
        return -1;
    }
    pipeline->ret_lock_ready = 1;

    /* 初始化单路视频编码 worker 的输入缓存和控制命令队列 */
    for (i = 0; i < ctx->config.video.stream_count; ++i)
    {
        if (!ctx->stream_enabled[i])
            continue;
        if (video_worker_init(&pipeline->video.workers[i]) != 0)
        {
            LOG_ERROR("pipeline init failed: video input stream=%d", i);
            return -1;
        }
    }

    /* 音频 worker 直接持有帧源引用；帧源由 mediaGateway run resources 管理。 */
    if (ctx->config.audio.source.enabled && ctx->audio_capture_ready)
        pipeline->audio.source = audio_source;
    return 0;
}

/**
 * @description: 停止 pipeline 内由其拥有的视频输入队列。
 */
static void pipeline_stop_queues(MediaGatewayPipeline *pipeline)
{
    int i = 0;

    if (!pipeline)
    {
        LOG_ERROR("pipeline_stop_queues failed: pipeline is NULL");
        return;
    }
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
        video_worker_stop(&pipeline->video.workers[i]);
}

/**
 * @description: 释放 pipeline 内所有运行期资源。
 */
void media_gateway_pipeline_deinit(MediaGatewayPipeline *pipeline)
{
    int i = 0;
    int mutex_ret = 0;

    if (!pipeline)
    {
        LOG_ERROR("media_gateway_pipeline_deinit failed: pipeline is NULL");
        return;
    }
    pipeline_stop_queues(pipeline);
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
        video_worker_deinit(&pipeline->video.workers[i]);
    free(pipeline->audio.pcm_buffer);
    pipeline->audio.pcm_buffer = NULL;
    pipeline->audio.pcm_capacity = 0;
    if (pipeline->ret_lock_ready)
    {
        mutex_ret = pthread_mutex_destroy(&pipeline->ret_lock);
        if (mutex_ret != 0)
        {
            LOG_ERROR("media_gateway_pipeline_deinit failed: pthread_mutex_destroy ret=%d(%s)",
                      mutex_ret,
                      strerror(mutex_ret));
        }
    }
    memset(pipeline, 0, sizeof(*pipeline));
}

/**
 * @description: 启动 pipeline 内所有编码 worker。
 */
int media_gateway_pipeline_start_workers(MediaGatewayPipeline *pipeline)
{
    int i = 0;
    int thread_ret = 0;
    VideoEncodeThreadArg *arg = NULL;

    if (!pipeline || !pipeline->ctx)
    {
        LOG_ERROR("media_gateway_pipeline_start_workers failed: invalid pipeline=%p ctx=%p",
                  (void *)pipeline, pipeline ? (void *)pipeline->ctx : NULL);
        return -1;
    }
    /* 为每个流创建编码线程，TODO：不能无限创建吧，RK的MPP硬件编解码是有限的 */
    for (i = 0; i < pipeline->ctx->config.video.stream_count; ++i)
    {
        if (!pipeline->ctx->stream_enabled[i])
            continue;
        arg = (VideoEncodeThreadArg *)calloc(1, sizeof(*arg));
        if (!arg)
        {
            LOG_ERROR("media_gateway_pipeline_start_workers failed: alloc video arg stream=%d", i);
            return -1;
        }
        arg->pipeline = pipeline;
        arg->stream_idx = i;
        /* 为每路流创建编码线程 */
        thread_ret = pthread_create(&pipeline->video.workers[i].thread,
                                    NULL,
                                    video_encode_thread_main,
                                    arg);
        if (thread_ret != 0)
        {
            free(arg);
            LOG_ERROR("media_gateway_pipeline_start_workers failed: pthread_create video stream=%d ret=%d(%s)",
                      i,
                      thread_ret,
                      strerror(thread_ret));
            return -1;
        }
        pipeline->video.workers[i].thread_started = 1;
    }
    if (pipeline->ctx->config.audio.source.enabled && pipeline->ctx->audio_capture_ready)
    {
        thread_ret = pthread_create(&pipeline->audio.thread,
                                    NULL,
                                    audio_encode_thread_main,
                                    pipeline);
        if (thread_ret != 0)
        {
            LOG_ERROR("media_gateway_pipeline_start_workers failed: pthread_create audio ret=%d(%s)",
                      thread_ret,
                      strerror(thread_ret));
            return -1;
        }
        pipeline->audio.thread_started = 1;
    }
    return 0;
}

/**
 * @description: 停止输入队列并等待所有 worker 退出。
 */
void media_gateway_pipeline_join_workers(MediaGatewayPipeline *pipeline)
{
    int i = 0;
    int join_ret = 0;

    if (!pipeline)
    {
        LOG_ERROR("media_gateway_pipeline_join_workers failed: pipeline is NULL");
        return;
    }
    pipeline_stop_queues(pipeline);
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        if (pipeline->video.workers[i].thread_started)
        {
            join_ret = pthread_join(pipeline->video.workers[i].thread, NULL);
            if (join_ret != 0)
            {
                LOG_ERROR("media_gateway_pipeline_join_workers failed: pthread_join video stream=%d ret=%d(%s)",
                          i,
                          join_ret,
                          strerror(join_ret));
                continue;
            }
            pipeline->video.workers[i].thread_started = 0;
        }
    }
    if (pipeline->audio.thread_started)
    {
        join_ret = pthread_join(pipeline->audio.thread, NULL);
        if (join_ret != 0)
        {
            LOG_ERROR("media_gateway_pipeline_join_workers failed: pthread_join audio ret=%d(%s)",
                      join_ret,
                      strerror(join_ret));
        }
        else
        {
            pipeline->audio.thread_started = 0;
        }
    }
}
