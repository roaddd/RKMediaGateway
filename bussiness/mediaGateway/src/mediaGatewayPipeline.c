#include "mediaGatewayPipeline.h"

#include "mediaGatewayProcess.h"

#include "logger.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @description: 生成 pthread 条件变量使用的绝对超时时间。
 */
static void make_abs_timeout(struct timespec *ts, int timeout_ms)
{
    long nsec;
    clock_gettime(CLOCK_REALTIME, ts);
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
        return;
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
    int ret;
    if (!pipeline)
    {
        LOG_ERROR("media_gateway_pipeline_get_ret failed: pipeline is NULL");
        return -1;
    }
    if (!pipeline->ret_lock_ready)
        return pipeline->ret;
    pthread_mutex_lock(&pipeline->ret_lock);
    ret = pipeline->ret;
    pthread_mutex_unlock(&pipeline->ret_lock);
    return ret;
}

/**
 * @description: 初始化单路视频 latest 输入槽。
 */
static int video_input_init(VideoEncodeInput *input)
{
    if (!input)
    {
        LOG_ERROR("video_input_init failed: input is NULL");
        return -1;
    }
    memset(input, 0, sizeof(*input));
    if (pthread_mutex_init(&input->lock, NULL) != 0)
    {
        LOG_ERROR("video_input_init failed: pthread_mutex_init");
        return -1;
    }
    if (pthread_cond_init(&input->cond, NULL) != 0)
    {
        LOG_ERROR("video_input_init failed: pthread_cond_init");
        pthread_mutex_destroy(&input->lock);
        return -1;
    }
    input->running = 1;
    input->ready = 1;
    return 0;
}

/**
 * @description: 停止单路视频 latest 输入槽并唤醒等待线程。
 */
static void video_input_stop(VideoEncodeInput *input)
{
    if (!input || !input->ready)
        return;
    pthread_mutex_lock(&input->lock);
    input->running = 0;
    pthread_cond_broadcast(&input->cond);
    pthread_mutex_unlock(&input->lock);
}

/**
 * @description: 释放单路视频 latest 输入槽。
 */
static void video_input_deinit(VideoEncodeInput *input)
{
    if (!input)
        return;
    video_input_stop(input);
    free(input->data);
    input->data = NULL;
    input->capacity = 0;
    if (input->ready)
    {
        pthread_cond_destroy(&input->cond);
        pthread_mutex_destroy(&input->lock);
    }
    memset(input, 0, sizeof(*input));
}

/**
 * @description: 将视频帧复制发布到 latest 输入槽。
 * 当前的设计目标是降低实时链路延迟，宁可丢旧帧，也不让视频积压。
 * TODO:如果后续要做录像，要改成不丢帧的方式
 */
int media_gateway_video_input_publish(VideoEncodeInput *input, const MediaFrame *frame)
{
    size_t need_size;
    uint8_t *new_data;

    if (!input || !frame || !frame->raw_frame || frame->raw_len <= 0)
    {
        LOG_ERROR("media_gateway_video_input_publish failed: invalid args input=%p frame=%p raw=%p len=%d",
                  (void *)input,
                  (const void *)frame,
                  frame ? (const void *)frame->raw_frame : NULL,
                  frame ? frame->raw_len : 0);
        return -1;
    }

    need_size = (size_t)frame->raw_len;
    pthread_mutex_lock(&input->lock);
    if (!input->running)
    {
        pthread_mutex_unlock(&input->lock);
        return 0;
    }
    if (input->capacity < need_size)
    {
        new_data = (uint8_t *)realloc(input->data, need_size);
        if (!new_data)
        {
            LOG_ERROR("media_gateway_video_input_publish failed: realloc need=%zu", need_size);
            pthread_mutex_unlock(&input->lock);
            return -1;
        }
        input->data = new_data;
        input->capacity = need_size;
    }
    if (input->valid)
        input->dropped_frames++;
    memcpy(input->data, frame->raw_frame, need_size);
    input->frame = *frame;
    input->frame.raw_frame = input->data;
    input->valid = 1;
    pthread_cond_signal(&input->cond);
    pthread_mutex_unlock(&input->lock);
    return 0;
}

/**
 * @description: 编码线程从 latest 输入槽获取一帧本地副本。
 */
static int video_input_acquire_copy(VideoEncodeInput *input,
                                    MediaFrame *frame,
                                    uint8_t **local_data,
                                    size_t *local_capacity,
                                    int timeout_ms)
{
    struct timespec ts;
    int wait_ret;
    size_t need_size;
    uint8_t *new_data;

    if (!input || !frame || !local_data || !local_capacity)
    {
        LOG_ERROR("video_input_acquire_copy failed: invalid args input=%p frame=%p local_data=%p capacity=%p",
                  (void *)input,
                  (void *)frame,
                  (void *)local_data,
                  (void *)local_capacity);
        return -1;
    }
    make_abs_timeout(&ts, timeout_ms);

    pthread_mutex_lock(&input->lock);
    while (input->running && !input->valid)
    {
        wait_ret = pthread_cond_timedwait(&input->cond, &input->lock, &ts);
        if (wait_ret == ETIMEDOUT)
        {
            pthread_mutex_unlock(&input->lock);
            return 0;
        }
    }
    if (!input->valid)
    {
        pthread_mutex_unlock(&input->lock);
        return 0;
    }

    need_size = (size_t)input->frame.raw_len;
    if (*local_capacity < need_size)
    {
        new_data = (uint8_t *)realloc(*local_data, need_size);
        if (!new_data)
        {
            LOG_ERROR("video_input_acquire_copy failed: realloc need=%zu", need_size);
            pthread_mutex_unlock(&input->lock);
            return -1;
        }
        *local_data = new_data;
        *local_capacity = need_size;
    }
    memcpy(*local_data, input->data, need_size); /* 这里有一次内存复制， */
    *frame = input->frame;
    frame->raw_frame = *local_data;
    input->valid = 0;
    pthread_mutex_unlock(&input->lock);
    return 1;
}

/**
 * @description: 初始化音频编码前 FIFO 队列。
 */
static int audio_queue_init(AudioEncodeQueue *queue, int capacity)
{
    if (!queue)
    {
        LOG_ERROR("audio_queue_init failed: queue is NULL");
        return -1;
    }
    if (capacity <= 0)
        capacity = AUDIO_FRAME_SOURCE_DEFAULT_SLOTS;
    if (capacity > AUDIO_FRAME_SOURCE_MAX_SLOTS)
        capacity = AUDIO_FRAME_SOURCE_MAX_SLOTS;
    memset(queue, 0, sizeof(*queue));
    queue->slots = (AudioEncodeSlot *)calloc((size_t)capacity, sizeof(AudioEncodeSlot));
    if (!queue->slots)
    {
        LOG_ERROR("audio_queue_init failed: calloc slots capacity=%d", capacity);
        return -1;
    }
    queue->capacity = capacity;
    if (pthread_mutex_init(&queue->lock, NULL) != 0)
    {
        LOG_ERROR("audio_queue_init failed: pthread_mutex_init");
        free(queue->slots);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }
    if (pthread_cond_init(&queue->cond, NULL) != 0)
    {
        LOG_ERROR("audio_queue_init failed: pthread_cond_init");
        pthread_mutex_destroy(&queue->lock);
        free(queue->slots);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }
    queue->running = 1;
    queue->ready = 1;
    return 0;
}

/**
 * @description: 停止音频 FIFO 并唤醒等待线程。
 */
static void audio_queue_stop(AudioEncodeQueue *queue)
{
    if (!queue || !queue->ready)
        return;
    pthread_mutex_lock(&queue->lock);
    queue->running = 0;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
}

/**
 * @description: 释放音频 FIFO 队列及其槽位缓存。
 */
static void audio_queue_deinit(AudioEncodeQueue *queue)
{
    int i;
    if (!queue)
        return;
    audio_queue_stop(queue);
    for (i = 0; i < queue->capacity; ++i)
    {
        free(queue->slots[i].data);
        queue->slots[i].data = NULL;
    }
    free(queue->slots);
    queue->slots = NULL;
    if (queue->ready)
    {
        pthread_cond_destroy(&queue->cond);
        pthread_mutex_destroy(&queue->lock);
    }
    memset(queue, 0, sizeof(*queue));
}

/**
 * @description: 将 PCM 音频帧复制发布到编码 FIFO。
 * 和视频不同，音频不能只保留最新帧，否则容易产生明显断音，所以这里保留短队列
 */
int media_gateway_audio_queue_publish(AudioEncodeQueue *queue, const AudioFrame *frame)
{
    int idx;
    size_t need_size;
    uint8_t *new_data;
    AudioEncodeSlot *slot;

    if (!queue || !frame || !frame->data || frame->size == 0)
    {
        LOG_ERROR("media_gateway_audio_queue_publish failed: invalid args queue=%p frame=%p data=%p size=%zu",
                  (void *)queue,
                  (const void *)frame,
                  frame ? (const void *)frame->data : NULL,
                  frame ? frame->size : 0);
        return -1;
    }

    need_size = frame->size;
    pthread_mutex_lock(&queue->lock);
    if (!queue->running)
    {
        pthread_mutex_unlock(&queue->lock);
        return 0;
    }
    if (queue->size >= queue->capacity)
    {
        queue->head = (queue->head + 1) % queue->capacity;
        queue->size--;
        queue->dropped_frames++;
    }
    idx = (queue->head + queue->size) % queue->capacity;
    slot = &queue->slots[idx];
    if (slot->capacity < need_size)
    {
        new_data = (uint8_t *)realloc(slot->data, need_size);
        if (!new_data)
        {
            LOG_ERROR("media_gateway_audio_queue_publish failed: realloc need=%zu", need_size);
            pthread_mutex_unlock(&queue->lock);
            return -1;
        }
        slot->data = new_data;
        slot->capacity = need_size;
    }
    memcpy(slot->data, frame->data, need_size); 
    slot->frame = *frame;
    slot->frame.data = slot->data;
    queue->size++;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
    return 0;
}

/**
 * @description: 音频编码线程从 FIFO 获取一帧本地副本。
 */
static int audio_queue_acquire_copy(AudioEncodeQueue *queue,
                                    AudioFrame *frame,
                                    uint8_t **local_data,
                                    size_t *local_capacity,
                                    int timeout_ms)
{
    struct timespec ts;
    int wait_ret;
    AudioEncodeSlot *slot;
    size_t need_size;
    uint8_t *new_data;

    if (!queue || !frame || !local_data || !local_capacity)
    {
        LOG_ERROR("audio_queue_acquire_copy failed: invalid args queue=%p frame=%p local_data=%p capacity=%p",
                  (void *)queue,
                  (void *)frame,
                  (void *)local_data,
                  (void *)local_capacity);
        return -1;
    }
    make_abs_timeout(&ts, timeout_ms);

    pthread_mutex_lock(&queue->lock);
    while (queue->running && queue->size == 0)
    {
        wait_ret = pthread_cond_timedwait(&queue->cond, &queue->lock, &ts);
        if (wait_ret == ETIMEDOUT)
        {
            pthread_mutex_unlock(&queue->lock);
            return 0;
        }
    }
    if (queue->size == 0)
    {
        pthread_mutex_unlock(&queue->lock);
        return 0;
    }

    slot = &queue->slots[queue->head];
    need_size = slot->frame.size;
    if (*local_capacity < need_size)
    {
        new_data = (uint8_t *)realloc(*local_data, need_size);
        if (!new_data)
        {
            LOG_ERROR("audio_queue_acquire_copy failed: realloc need=%zu", need_size);
            pthread_mutex_unlock(&queue->lock);
            return -1;
        }
        *local_data = new_data;
        *local_capacity = need_size;
    }
    /* 从 FIFO 中拷贝音频帧数据 */
    memcpy(*local_data, slot->data, need_size);
    *frame = slot->frame;
    frame->data = *local_data;
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;

    pthread_mutex_unlock(&queue->lock);
    return 1;
}

/**
 * @description: 视频编码 worker 主函数。
 */
static void *video_encode_thread_main(void *arg)
{
    VideoEncodeThreadArg *thread_arg = (VideoEncodeThreadArg *)arg;
    MediaGatewayPipeline *pipeline = thread_arg->pipeline;
    int stream_idx = thread_arg->stream_idx;
    uint8_t *local_data = NULL;
    size_t local_capacity = 0;
    MediaFrame frame;
    int ret;

    free(thread_arg);
    while (pipeline->ctx->running)
    {
        /*
         * VideoEncodeInput 是单槽 latest-frame 输入，不做 FIFO 排队。
         * 编码线程慢时新帧会覆盖未消费旧帧，优先降低端到端延迟；主要成本是 publish/acquire 都在锁内整帧拷贝。
         */
        ret = video_input_acquire_copy(&pipeline->video_inputs[stream_idx],
                                       &frame,
                                       &local_data,
                                       &local_capacity,
                                       100);
        if (ret < 0)
        {
            LOG_ERROR("video encode thread failed: acquire stream=%d", stream_idx);
            pipeline_set_error(pipeline);
            break;
        }
        if (ret == 0)
            continue;
        if (media_gateway_process_stream(pipeline->ctx, &pipeline->state, &frame, stream_idx) != 0)
        {
            LOG_ERROR("video encode thread failed: process stream=%d frame=%" PRIu64,
                      stream_idx,
                      frame.frame_id);
            pipeline_set_error(pipeline);
            break;
        }
    }
    free(local_data);
    return NULL;
}

/**
 * @description: 音频编码 worker 主函数。
 */
static void *audio_encode_thread_main(void *arg)
{
    MediaGatewayPipeline *pipeline = (MediaGatewayPipeline *)arg;
    uint8_t *local_data = NULL; /* 线程私有的 PCM 帧缓存 */
    size_t local_capacity = 0;
    AudioFrame frame;
    int ret;

    while (pipeline->ctx->running)
    {
        ret = audio_queue_acquire_copy(&pipeline->audio_queue,
                                       &frame,
                                       &local_data,
                                       &local_capacity,
                                       100);
        if (ret < 0)
        {
            LOG_ERROR("audio encode thread failed: acquire");
            pipeline_set_error(pipeline);
            break;
        }
        if (ret == 0)
            continue;
        if (media_gateway_process_audio(pipeline->ctx, &frame) != 0)
        {
            LOG_ERROR("audio encode thread failed: process frame=%" PRIu64, frame.frame_id);
            pipeline_set_error(pipeline);
            break;
        }
    }
    free(local_data);
    return NULL;
}

/**
 * @description: 初始化 pipeline 队列、输入槽和同步状态。
 */
int media_gateway_pipeline_init(MediaGatewayPipeline *pipeline, MediaGatewayCtx *ctx)
{
    int i;
    if (!pipeline || !ctx)
    {
        LOG_ERROR("media_gateway_pipeline_init failed: invalid args pipeline=%p ctx=%p",
                  (void *)pipeline,
                  (void *)ctx);
        return -1;
    }
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->ctx = ctx;
    if (pthread_mutex_init(&pipeline->ret_lock, NULL) != 0)
    {
        LOG_ERROR("media_gateway_pipeline_init failed: pthread_mutex_init ret_lock");
        return -1;
    }
    pipeline->ret_lock_ready = 1;
    for (i = 0; i < ctx->config.stream_count; ++i)
    {
        if (!ctx->stream_enabled[i])
            continue;
        if (video_input_init(&pipeline->video_inputs[i]) != 0)
        {
            LOG_ERROR("pipeline init failed: video input stream=%d", i);
            return -1;
        }
    }
    if (ctx->config.audio.enabled && ctx->audio_capture_ready)
    {
        if (audio_queue_init(&pipeline->audio_queue, ctx->config.audio.source_slots) != 0)
        {
            LOG_ERROR("pipeline init failed: audio queue");
            return -1;
        }
    }
    return 0;
}

/**
 * @description: 停止 pipeline 内所有输入队列。
 */
static void pipeline_stop_queues(MediaGatewayPipeline *pipeline)
{
    int i;
    if (!pipeline)
        return;
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
        video_input_stop(&pipeline->video_inputs[i]);
    audio_queue_stop(&pipeline->audio_queue);
}

/**
 * @description: 释放 pipeline 内所有运行期资源。
 */
void media_gateway_pipeline_deinit(MediaGatewayPipeline *pipeline)
{
    int i;
    if (!pipeline)
        return;
    pipeline_stop_queues(pipeline);
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
        video_input_deinit(&pipeline->video_inputs[i]);
    audio_queue_deinit(&pipeline->audio_queue);
    if (pipeline->ret_lock_ready)
        pthread_mutex_destroy(&pipeline->ret_lock);
    memset(pipeline, 0, sizeof(*pipeline));
}

/**
 * @description: 启动 pipeline 内所有编码 worker。
 */
int media_gateway_pipeline_start_workers(MediaGatewayPipeline *pipeline)
{
    int i = 0;
    if (!pipeline || !pipeline->ctx)
    {
        LOG_ERROR("media_gateway_pipeline_start_workers failed: invalid pipeline=%p ctx=%p",
                  (void *)pipeline, pipeline ? (void *)pipeline->ctx : NULL);
        return -1;
    }
    /* 为每个流创建编码线程，TODO：不能无限创建吧，RK的MPP硬件编解码是有限的 */
    for (i = 0; i < pipeline->ctx->config.stream_count; ++i)
    {
        VideoEncodeThreadArg *arg;
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
        if (pthread_create(&pipeline->video_threads[i], NULL, video_encode_thread_main, arg) != 0)
        {
            free(arg);
            LOG_ERROR("pipeline start failed: video encode thread stream=%d", i);
            return -1;
        }
        pipeline->video_thread_started[i] = 1;
    }
    if (pipeline->ctx->config.audio.enabled && pipeline->ctx->audio_capture_ready)
    {
        if (pthread_create(&pipeline->audio_thread, NULL, audio_encode_thread_main, pipeline) != 0)
        {
            LOG_ERROR("pipeline start failed: audio encode thread");
            return -1;
        }
        pipeline->audio_thread_started = 1;
    }
    return 0;
}

/**
 * @description: 停止输入队列并等待所有 worker 退出。
 */
void media_gateway_pipeline_join_workers(MediaGatewayPipeline *pipeline)
{
    int i;
    if (!pipeline)
        return;
    pipeline_stop_queues(pipeline);
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        if (pipeline->video_thread_started[i])
        {
            pthread_join(pipeline->video_threads[i], NULL);
            pipeline->video_thread_started[i] = 0;
        }
    }
    if (pipeline->audio_thread_started)
    {
        pthread_join(pipeline->audio_thread, NULL);
        pipeline->audio_thread_started = 0;
    }
}
