#include "mediaOutput.h"

#include "commonDef.h"
#include "gb28181/inc/gb28181Output.h"
#include "logger.h"
#include "mediaOutputPathMetrics.h"
#include "rtmp/inc/rtmpOutput.h"
#include "rtsp/inc/rtspOutput.h"
#include "util.h"
#include "webRTC/inc/webRTCOutput.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_OUTPUT_QUEUE_CAPACITY 32
#define DEFAULT_RECONNECT_INTERVAL_MS 1000

static void output_set_thread_name(const char *output_name)
{
    char thread_name[16];
    snprintf(thread_name,
             sizeof(thread_name),
             "out-%.11s",
             (output_name && output_name[0] != '\0') ? output_name : "media");
    util_set_thread_name(thread_name);
}

/**
 * @description: 初始化输出包队列。
 * @param queue 待初始化的队列。
 * @param capacity 队列容量；小于等于 0 时使用默认容量。
 * @return MEDIA_OK 成功，错误码表示失败原因。
 */
static int output_queue_init(MediaOutputPacketQueue *queue, int capacity)
{
    if (!queue)
    {
        LOG_ERROR("output_queue_init failed: queue is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    memset(queue, 0, sizeof(*queue));
    queue->capacity = (capacity > 0) ? capacity : DEFAULT_OUTPUT_QUEUE_CAPACITY;
    queue->items = (MediaPacket *)calloc((size_t)queue->capacity, sizeof(MediaPacket));
    if (!queue->items)
    {
        LOG_ERROR("output_queue_init failed: alloc capacity=%d", queue->capacity);
        return MEDIA_ERR_NO_MEMORY;
    }
    return MEDIA_OK;
}

/**
 * @description: 释放输出包队列，并释放队列中仍持有的 MediaPacket 引用。
 * @param queue 待释放的队列。
 */
static void output_queue_deinit(MediaOutputPacketQueue *queue)
{
    int i;
    if (!queue)
        return;
    for (i = 0; i < queue->capacity; ++i)
        media_packet_reset(&queue->items[i]);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

/**
 * @description: 统计音频队列和视频队列的总深度。
 * @param output 输出通道。
 * @return 当前待发送包总数。
 */
static int output_queue_total_depth(const MediaOutput *output)
{
    return output->video_queue.size + output->audio_queue.size;
}

/**
 * @description: 更新统计中的队列深度，调用方必须已经持有 output->lock。
 * @param output 输出通道。
 */
static void output_update_queue_depth_locked(MediaOutput *output)
{
    if (!output)
        return;
    /*
     * 同时保存总队列深度和音视频分队列深度。
     * 总深度继续用于原有网络策略判断，分队列深度用于调试定位延迟来源。
     */
    output->stats.queue_depth = output_queue_total_depth(output);
    output->stats.video_queue_depth = output->video_queue.size;
    output->stats.audio_queue_depth = output->audio_queue.size;
}

/**
 * @description: 查看队首包但不出队。
 * @param queue 待查看的队列。
 * @return 队首包指针；队列为空时返回 NULL。
 */
static MediaPacket *output_queue_peek(MediaOutputPacketQueue *queue)
{
    if (!queue || queue->size <= 0)
        return NULL;
    return &queue->items[queue->head];
}

/**
 * @description: 从指定队列弹出队首包，调用方必须已经持有 output->lock。
 * @param output 输出通道，用于更新统计。
 * @param queue 待出队的队列。
 * @param packet 输出弹出的包，调用方负责 reset。
 * @return MEDIA_OK 成功，MEDIA_ERR_NOT_FOUND 表示队列为空，错误码表示失败原因。
 */
static int output_queue_pop_locked(MediaOutput *output, MediaOutputPacketQueue *queue, MediaPacket *packet)
{
    if (!queue || queue->size <= 0)
        return MEDIA_ERR_NOT_FOUND;

    *packet = queue->items[queue->head];
    media_packet_init(&queue->items[queue->head]);
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;
    output_update_queue_depth_locked(output);
    return MEDIA_OK;
}

/**
 * @description: 丢弃指定队列中最旧的包，调用方必须已经持有 output->lock。
 * @param output 输出通道，用于更新丢帧统计。
 * @param queue 待丢弃旧包的队列。
 */
static void output_queue_drop_oldest_locked(MediaOutput *output, MediaOutputPacketQueue *queue)
{
    MediaPacket packet;

    media_packet_init(&packet);
    if (output_queue_pop_locked(output, queue, &packet) == MEDIA_OK)
    {
        output->stats.dropped_frames++;
        media_packet_reset(&packet);
    }
}

/**
 * @description: 根据包类型选择对应的输出队列。
 * @param output 输出通道。
 * @param packet 待入队的媒体包。
 * @return 音频包返回音频队列，其他包返回视频队列。
 */
static MediaOutputPacketQueue *output_queue_for_packet(MediaOutput *output, const MediaPacket *packet)
{
    if (packet->frame_type == MEDIA_FRAME_TYPE_AUDIO)
        return &output->audio_queue;
    return &output->video_queue;
}

/**
 * @description: 将包引用压入指定队列，调用方必须已经持有 output->lock。
 * @param output 输出通道，用于更新统计。
 * @param queue 目标队列。
 * @param packet 待入队媒体包。
 * @return MEDIA_OK 成功，MEDIA_ERR_FULL 表示队列不可用或已满。
 */
static int output_queue_push_locked(MediaOutput *output, MediaOutputPacketQueue *queue, const MediaPacket *packet)
{
    int tail;

    if (!queue || !queue->items || queue->capacity <= 0 || queue->size >= queue->capacity)
    {
        LOG_ERROR("output_queue_push_locked failed: invalid/full queue=%p items=%p capacity=%d size=%d",
                  (void *)queue,
                  queue ? (void *)queue->items : NULL,
                  queue ? queue->capacity : -1,
                  queue ? queue->size : -1);
        return MEDIA_ERR_FULL;
    }

    tail = (queue->head + queue->size) % queue->capacity;
    media_packet_copy_ref(&queue->items[tail], packet);
    queue->size++;
    output_update_queue_depth_locked(output);
    return MEDIA_OK;
}

/**
 * @description: 从音频/视频队列中选择下一包发送，调用方必须已经持有 output->lock。
 *
 * 策略：
 * 1. 重连后等待关键帧时优先取视频包，让关键帧保护逻辑尽快生效。
 * 2. 音频和视频都存在时按 PTS 较小者优先发送，尽量保持音画顺序。
 * 3. 只有一个队列有数据时直接取该队列。
 *
 * @param output 输出通道。
 * @param packet 输出弹出的媒体包，调用方负责 reset。
 * @return MEDIA_OK 成功，MEDIA_ERR_NOT_FOUND 表示当前无包可取。
 */
static int output_pop_next_locked(MediaOutput *output, MediaPacket *packet)
{
    MediaPacket *video = output_queue_peek(&output->video_queue);
    MediaPacket *audio = output_queue_peek(&output->audio_queue);

    if (!video && !audio)
        return MEDIA_ERR_NOT_FOUND;

    if (output->waiting_for_keyframe && video)
        return output_queue_pop_locked(output, &output->video_queue, packet);
    if (!video)
        return output_queue_pop_locked(output, &output->audio_queue, packet);
    if (!audio)
        return output_queue_pop_locked(output, &output->video_queue, packet);
    if (audio->pts_us < video->pts_us)
        return output_queue_pop_locked(output, &output->audio_queue, packet);
    return output_queue_pop_locked(output, &output->video_queue, packet);
}

/**
 * @description: 输出通道后台发送线程。
 *
 * 线程负责等待队列数据、维护连接状态、执行重连后的关键帧保护，
 * 调用协议层 send_packet 回调，并更新发送、丢包和失败统计。
 *
 * @param arg MediaOutput 指针。
 * @return NULL。
 */
static void *media_output_thread(void *arg)
{
    MediaOutput *output = (MediaOutput *)arg;
    uint64_t send_start_us = 0;
    uint64_t send_done_us = 0;
    int send_ret = -1;
    MediaPacket packet; /* 每个packet是一帧图像或者音频，一帧编码图像内部可能包含多个H264 NALU */

    output_set_thread_name(output->config.name);
    media_packet_init(&packet);
    while (1)
    {
        pthread_mutex_lock(&output->lock);
        /* 等待队列中有数据 */
        while (!output->stop_requested && output_queue_total_depth(output) == 0)
            pthread_cond_wait(&output->cond, &output->lock);
        
        if (output->stop_requested && output_queue_total_depth(output) == 0)
        {
            pthread_mutex_unlock(&output->lock);
            break;
        }
        /* 从队列中取出下一帧，可能是音频帧也可能是视频帧 */
        if (output_pop_next_locked(output, &packet) != MEDIA_OK)
        {
            pthread_mutex_unlock(&output->lock);
            continue;
        }
        pthread_mutex_unlock(&output->lock);

        if (!output->connected)
        {
            if (output->vtable->connect(output) == MEDIA_OK)
            {
                pthread_mutex_lock(&output->lock);
                output->connected = 1;
                output->stats.connected = 1;
                output->waiting_for_keyframe = output->config.drop_until_keyframe_after_reconnect ? 1 : 0;
                output->stats.waiting_for_keyframe = output->waiting_for_keyframe;
                output->stats.reconnect_count++;
                pthread_mutex_unlock(&output->lock);
                printf("[OUTPUT] name=%s event=connected reconnects=%" PRIu64 "\n",
                       output->config.name ? output->config.name : "unknown",
                       output->stats.reconnect_count);
            }
            else
            {
                pthread_mutex_lock(&output->lock);
                output->stats.connected = 0;
                pthread_mutex_unlock(&output->lock);
                LOG_WARN("[OUTPUT] name=%s event=connect_failed retry_ms=%d",
                         output->config.name ? output->config.name : "unknown",
                         output->config.reconnect_interval_ms);
                media_packet_reset(&packet);
                usleep((useconds_t)output->config.reconnect_interval_ms * 1000U);
                continue;
            }
        }

        if (output->waiting_for_keyframe && !packet.is_key_frame)
        {
            pthread_mutex_lock(&output->lock);
            output->stats.dropped_frames++;
            pthread_mutex_unlock(&output->lock);
            media_packet_reset(&packet);
            continue;
        }

        if (output->waiting_for_keyframe && packet.is_key_frame)
        {
            pthread_mutex_lock(&output->lock);
            output->waiting_for_keyframe = 0;
            output->stats.waiting_for_keyframe = 0;
            pthread_mutex_unlock(&output->lock);
        }
        /* 发送数据包 */
        send_start_us = media_output_metrics_now_us();
        send_ret = output->vtable->send_packet(output, &packet);
        send_done_us = media_output_metrics_now_us();
        /* 打印一帧音频/视频从采集时间戳到协议层发送完成的路径耗时。 */
        {
            MediaOutputPathLatencySample path_sample;
            path_sample.output_name = output->config.name;
            path_sample.output_type = output->type;
            path_sample.packet = &packet;
            path_sample.send_start_us = send_start_us;
            path_sample.send_done_us = send_done_us;
            media_output_log_path_latency(&path_sample);
        }
        if (send_ret != MEDIA_OK)
        {
            pthread_mutex_lock(&output->lock);
            output->connected = 0;
            output->stats.connected = 0;
            output->waiting_for_keyframe = output->config.drop_until_keyframe_after_reconnect ? 1 : 0;
            output->stats.waiting_for_keyframe = output->waiting_for_keyframe;
            output->stats.send_failures++;
            pthread_mutex_unlock(&output->lock);
            LOG_ERROR("[OUTPUT] name=%s event=send_failed frame=%" PRIu64 " failures=%" PRIu64,
                      output->config.name ? output->config.name : "unknown",
                      packet.frame_id,
                      output->stats.send_failures);
            if (output->vtable->disconnect)
                output->vtable->disconnect(output);
        }
        else
        {
            pthread_mutex_lock(&output->lock);
            output->stats.sent_frames++;
            output->stats.sent_bytes += packet.buffer ? packet.buffer->size : 0;
            pthread_mutex_unlock(&output->lock);
        }
        /* 发送完成后，释放对buffer的引用 */
        media_packet_reset(&packet);
    }

    if (output->vtable->disconnect)
        output->vtable->disconnect(output);
    return NULL;
}

/**
 * @description: 根据输出类型创建并初始化具体协议输出通道。
 * @param output 待初始化的输出通道。
 * @param config 输出配置。
 * @return MEDIA_OK 成功，错误码表示失败原因。
 */
int media_output_setup(MediaOutput *output, const MediaOutputConfig *config)
{
    if (!output || !config)
    {
        LOG_ERROR("media_output_setup failed: invalid arguments output=%p config=%p",
                  (void *)output,
                  (const void *)config);
        return MEDIA_ERR_INVALID_PARAM;
    }

    switch (config->type)
    {
    case MEDIA_OUTPUT_TYPE_RTSP:
        return media_output_setup_rtsp(output, &config->protocol.rtsp);
    case MEDIA_OUTPUT_TYPE_RTMP:
        return media_output_setup_rtmp(output, &config->protocol.rtmp);
    case MEDIA_OUTPUT_TYPE_GB28181:
        return media_output_setup_gb28181(output, &config->protocol.gb28181);
    case MEDIA_OUTPUT_TYPE_WEBRTC:
        return media_output_setup_webrtc(output, &config->protocol.webrtc);
    default:
        LOG_ERROR("media_output_setup failed: unknown type=%d", config->type);
        return MEDIA_ERR_UNSUPPORTED;
    }
}

/**
 * @description: 初始化通用输出通道对象。
 * @param output 待初始化的输出通道。
 * @param config 通道基础配置。
 * @param vtable 协议实现回调表。
 * @param impl 协议实现私有上下文。
 * @return MEDIA_OK 成功，错误码表示失败原因。
 */
int media_output_init(MediaOutput *output,
                      const MediaOutputChannelConfig *config,
                      const MediaOutputVTable *vtable,
                      void *impl)
{
    int queue_capacity;

    if (!output || !config || !vtable || !vtable->connect || !vtable->send_packet)
    {
        LOG_ERROR("media_output_init failed: invalid arguments output=%p config=%p vtable=%p connect=%p send=%p",
                  (void *)output,
                  (const void *)config,
                  (const void *)vtable,
                  vtable ? (void *)vtable->connect : NULL,
                  vtable ? (void *)vtable->send_packet : NULL);
        return MEDIA_ERR_INVALID_PARAM;
    }

    memset(output, 0, sizeof(*output));
    output->config = *config;
    output->vtable = vtable;
    output->impl = impl;
    output->config.reconnect_interval_ms = (config->reconnect_interval_ms > 0)
                                               ? config->reconnect_interval_ms
                                               : DEFAULT_RECONNECT_INTERVAL_MS;

    queue_capacity = (config->queue_capacity > 0) ? config->queue_capacity : DEFAULT_OUTPUT_QUEUE_CAPACITY;
    if (output_queue_init(&output->video_queue, queue_capacity) != MEDIA_OK ||
        output_queue_init(&output->audio_queue, queue_capacity) != MEDIA_OK)
    {
        LOG_ERROR("media_output_init failed: queue alloc name=%s capacity=%d",
                  config->name ? config->name : "unknown",
                  queue_capacity);
        output_queue_deinit(&output->video_queue);
        output_queue_deinit(&output->audio_queue);
        return MEDIA_ERR_NO_MEMORY;
    }

    pthread_mutex_init(&output->lock, NULL);
    pthread_cond_init(&output->cond, NULL);
    output->waiting_for_keyframe = output->config.drop_until_keyframe_after_reconnect ? 1 : 0;
    output->stats.waiting_for_keyframe = output->waiting_for_keyframe;
    return MEDIA_OK;
}

/**
 * @description: 启动输出通道，并创建后台发送线程。
 * @param output 输出通道。
 * @return MEDIA_OK 成功，错误码表示失败原因。
 */
int media_output_start(MediaOutput *output)
{
    if (!output)
    {
        LOG_ERROR("media_output_start failed: output is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    /* 调用各个输出通道的启动函数 */
    if (output->vtable->start && output->vtable->start(output) != MEDIA_OK)
    {
        LOG_ERROR("media_output_start failed: vtable start name=%s",
                  output->config.name ? output->config.name : "unknown");
        return MEDIA_ERR;
    }

    output->running = 1;
    if (pthread_create(&output->thread, NULL, media_output_thread, output) != 0)
    {
        output->running = 0;
        LOG_ERROR("media_output_start failed: pthread_create name=%s",
                  output->config.name ? output->config.name : "unknown");
        if (output->vtable->stop)
            output->vtable->stop(output);
        return MEDIA_ERR;
    }
    return MEDIA_OK;
}

/**
 * @description: 将媒体包放入输出通道队列。
 *
 * 队列满时的策略：
 * - 音频：丢弃最旧包，保留最新音频，降低实时延迟。
 * - 视频非关键帧：直接丢弃新包，避免阻塞输出线程。
 * - 视频关键帧：丢弃旧视频包，为关键帧腾出空间。
 *
 * @param output 输出通道。
 * @param packet 待入队媒体包。
 * @return MEDIA_OK 成功或按策略丢包，错误码表示参数或队列错误。
 */
int media_output_enqueue(MediaOutput *output, const MediaPacket *packet)
{
    MediaOutputPacketQueue *queue;

    if (!output || !packet || !packet->buffer)
    {
        LOG_ERROR("media_output_enqueue failed: invalid args output=%p packet=%p buffer=%p",
                  (void *)output,
                  (const void *)packet,
                  packet ? (void *)packet->buffer : NULL);
        return MEDIA_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&output->lock);
    /* 根据媒体包类型选择对应的队列 */
    queue = output_queue_for_packet(output, packet);

    /* 队列满时按媒体类型执行低延迟丢包策略。 */
    if (queue->size >= queue->capacity)
    {
        if (packet->frame_type == MEDIA_FRAME_TYPE_AUDIO)
        {
            output_queue_drop_oldest_locked(output, queue);
        }
        else if (!packet->is_key_frame)
        {
            output->stats.dropped_frames++;
            pthread_mutex_unlock(&output->lock);
            return MEDIA_OK;
        }
        else
        {
            while (queue->size >= queue->capacity)
                output_queue_drop_oldest_locked(output, queue);
        }
    }

    if (output_queue_push_locked(output, queue, packet) != MEDIA_OK)
    {
        LOG_ERROR("media_output_enqueue failed: push packet frame=%" PRIu64 " type=%d codec=%d",
                  packet->frame_id,
                  packet->frame_type,
                  packet->codec);
        pthread_mutex_unlock(&output->lock);
        return MEDIA_ERR_FULL;
    }
    pthread_cond_signal(&output->cond);
    pthread_mutex_unlock(&output->lock);
    return MEDIA_OK;
}

/**
 * @description: 消费外部输出通道触发的 IDR 请求。
 * @param output 输出通道。
 * @return 1 表示需要向编码器请求 IDR，0 表示没有请求。
 */
int media_output_consume_external_idr_request(MediaOutput *output)
{
    if (!output)
        return 0;
    if (output->type == MEDIA_OUTPUT_TYPE_RTSP)
        return media_output_rtsp_consume_external_idr_request(output);
    if (output->type == MEDIA_OUTPUT_TYPE_GB28181)
        return media_output_gb28181_consume_external_idr_request(output);
    if (output->type == MEDIA_OUTPUT_TYPE_WEBRTC)
        return media_output_webrtc_consume_external_idr_request(output);
    return 0;
}

/**
 * @description: 请求输出线程退出，并等待线程结束。
 * @param output 输出通道。
 */
void media_output_stop(MediaOutput *output)
{
    if (!output || !output->running)
        return;

    pthread_mutex_lock(&output->lock);
    output->stop_requested = 1;
    pthread_cond_broadcast(&output->cond);
    pthread_mutex_unlock(&output->lock);
    pthread_join(output->thread, NULL);
    output->running = 0;
    if (output->vtable->stop)
        output->vtable->stop(output);
}

/**
 * @description: 释放输出通道及其队列、同步对象和协议私有资源。
 * @param output 输出通道。
 */
void media_output_deinit(MediaOutput *output)
{
    if (!output)
        return;

    media_output_stop(output);
    if (!output->running && output->vtable && output->vtable->stop)
        output->vtable->stop(output);
    output_queue_deinit(&output->video_queue);
    output_queue_deinit(&output->audio_queue);
    pthread_cond_destroy(&output->cond);
    pthread_mutex_destroy(&output->lock);
    memset(output, 0, sizeof(*output));
}

/**
 * @description: 获取输出通道统计信息快照。
 * @param output 输出通道。
 * @param stats 输出统计信息。
 */
void media_output_get_stats(MediaOutput *output, MediaOutputStats *stats)
{
    if (!output || !stats)
        return;

    pthread_mutex_lock(&output->lock);
    *stats = output->stats;
    pthread_mutex_unlock(&output->lock);
}

/*
 * 设置输出通道的视频 RTP 包级 pacer。
 * 通用输出层负责参数校验和去重，具体协议层负责把开关下发到真实发送点。
 */
int media_output_set_video_pacer(MediaOutput *output, MediaOutputPacerMode mode, int pacing_rate_bps)
{
    int effective_rate_bps = 0;
    int ret = 0;

    if (!output)
    {
        LOG_ERROR("media_output_set_video_pacer failed: output is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (mode != MEDIA_OUTPUT_PACER_DISABLED && mode != MEDIA_OUTPUT_PACER_ENABLED)
    {
        LOG_ERROR("media_output_set_video_pacer failed: invalid mode=%d", mode);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (mode == MEDIA_OUTPUT_PACER_ENABLED && pacing_rate_bps <= 0)
    {
        LOG_ERROR("media_output_set_video_pacer failed: mode=%d invalid rate=%d",
                  mode,
                  pacing_rate_bps);
        return MEDIA_ERR_INVALID_PARAM;
    }

    effective_rate_bps = (mode == MEDIA_OUTPUT_PACER_ENABLED) ? pacing_rate_bps : 0;

    if (output->video_pacer_mode == mode && output->video_pacing_rate_bps == effective_rate_bps)
        return MEDIA_OK;

    if (output->vtable && output->vtable->set_video_pacer)
    {
        ret = output->vtable->set_video_pacer(output, mode, effective_rate_bps);
        if (ret != MEDIA_OK)
        {
            LOG_ERROR("media_output_set_video_pacer failed: name=%s mode=%d rate=%d ret=%d",
                      output->config.name ? output->config.name : "unknown",
                      mode,
                      effective_rate_bps,
                      ret);
            return ret;
        }
    }
    else if (mode == MEDIA_OUTPUT_PACER_ENABLED)
    {
        /*
         * 非 RTSP 输出当前没有 RTP 包级 pacing 执行点。返回成功但不记录为已应用，
         * 避免上层把 RTMP/GB28181 误判为已经启用 RTP pacer。
         */
        return MEDIA_OK;
    }

    output->video_pacer_mode = mode;
    output->video_pacing_rate_bps = effective_rate_bps;
    return MEDIA_OK;
}

/*
 * 查询输出通道的视频 RTP pacer 调试统计。
 * 非 RTSP 等不支持包级 pacer 的输出协议返回 MEDIA_ERR_UNSUPPORTED。
 */
int media_output_get_video_pacer_stats(MediaOutput *output, MediaOutputPacerStats *stats)
{
    if (!output || !stats)
    {
        LOG_ERROR("media_output_get_video_pacer_stats failed: output=%p stats=%p",
                  (void *)output,
                  (void *)stats);
        return MEDIA_ERR_INVALID_PARAM;
    }

    memset(stats, 0, sizeof(*stats));
    if (!output->vtable || !output->vtable->get_video_pacer_stats)
        return MEDIA_ERR_UNSUPPORTED;
    return output->vtable->get_video_pacer_stats(output, stats);
}
