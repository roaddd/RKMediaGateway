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
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define VIDEO_COMMAND_QUEUE_CAPACITY 8

/** video worker 通过 poll 同时等待的事件类型。 */
typedef enum VideoWorkerPollIndex
{
    VIDEO_WORKER_POLL_FRAME = 0,
    VIDEO_WORKER_POLL_CONTROL,
    VIDEO_WORKER_POLL_COUNT
} VideoWorkerPollIndex;

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
 * @brief 写控制 eventfd，通知编码线程有 MPP 命令或停止请求需要处理。
 */
static int video_worker_notify_control(VideoEncodeWorker *worker)
{
    uint64_t value = 1;
    ssize_t written = -1;

    if (!worker || worker->control_event_fd < 0)
    {
        LOG_ERROR("video_worker_notify_control failed: worker=%p event_fd=%d",
                  (void *)worker,
                  worker ? worker->control_event_fd : -1);
        return -1;
    }

    do
    {
        written = write(worker->control_event_fd, &value, sizeof(value));
    } while (written < 0 && errno == EINTR);

    if (written == (ssize_t)sizeof(value))
        return 0;
    if (written < 0 && errno == EAGAIN)
        return 0;

    LOG_ERROR("video_worker_notify_control failed: event_fd=%d written=%ld errno=%d(%s)",
              worker->control_event_fd,
              (long)written,
              errno,
              strerror(errno));
    return -1;
}

/**
 * @brief 清空控制 eventfd 的累计计数。
 *
 * 多条命令可以合并成一次唤醒；真正的命令数量和内容由 command_queue 保存，
 * 因此这里读到 EAGAIN 后再由编码线程清空命令队列。
 */
static int video_worker_drain_control_event(VideoEncodeWorker *worker)
{
    uint64_t value = 0;
    ssize_t read_size = -1;

    if (!worker || worker->control_event_fd < 0)
    {
        LOG_ERROR("video_worker_drain_control_event failed: worker=%p event_fd=%d",
                  (void *)worker,
                  worker ? worker->control_event_fd : -1);
        return -1;
    }

    for (;;)
    {
        read_size = read(worker->control_event_fd, &value, sizeof(value));
        if (read_size == (ssize_t)sizeof(value))
            continue;
        if (read_size < 0 && errno == EINTR)
            continue;
        if (read_size < 0 && errno == EAGAIN)
            return 0;

        LOG_ERROR("video_worker_drain_control_event failed: event_fd=%d read=%ld errno=%d(%s)",
                  worker->control_event_fd,
                  (long)read_size,
                  errno,
                  strerror(errno));
        return -1;
    }
}

/**
 * @brief 在线程安全的状态锁保护下读取 worker 运行标志。
 */
static int video_worker_is_running(VideoEncodeWorker *worker)
{
    int running = 0;

    if (!worker || !worker->ready)
        return 0;
    pthread_mutex_lock(&worker->state_lock);
    running = worker->running;
    pthread_mutex_unlock(&worker->state_lock);
    return running;
}

/**
 * @brief 初始化单路视频编码 worker 的帧源引用、控制事件和命令队列。
 */
static int video_worker_init(VideoEncodeWorker *worker, MediaFrameSource *source)
{
    int mutex_ret = 0;
    int queue_ret = 0;
    int event_fd = -1;

    if (!worker || !source)
    {
        LOG_ERROR("video_worker_init failed: worker=%p source=%p",
                  (void *)worker,
                  (void *)source);
        return -1;
    }

    memset(worker, 0, sizeof(*worker));
    worker->control_event_fd = -1;
    worker->source = source;
    /* 第一步：初始化只保护 worker 生命周期状态的锁。 */
    mutex_ret = pthread_mutex_init(&worker->state_lock, NULL);
    if (mutex_ret != 0)
    {
        LOG_ERROR("video_worker_init failed: pthread_mutex_init ret=%d(%s)",
                  mutex_ret,
                  strerror(mutex_ret));
        return -1;
    }

    /* 第二步：创建命令和停止请求共用的非阻塞 eventfd。 */
    event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0)
    {
        LOG_ERROR("video_worker_init failed: eventfd errno=%d(%s)",
                  errno,
                  strerror(errno));
        pthread_mutex_destroy(&worker->state_lock);
        return -1;
    }
    worker->control_event_fd = event_fd;

    /* 第三步：命令内容保存在队列中，eventfd 仅用于唤醒 poll。 */
    queue_ret = thread_message_queue_init(&worker->command_queue,
                                          VIDEO_COMMAND_QUEUE_CAPACITY);
    if (queue_ret != MEDIA_OK)
    {
        LOG_ERROR("video_worker_init failed: init command queue ret=%d", queue_ret);
        close(worker->control_event_fd);
        worker->control_event_fd = -1;
        pthread_mutex_destroy(&worker->state_lock);
        return -1;
    }
    worker->running = 1;
    worker->ready = 1;
    return 0;
}

/**
 * @brief 停止单路视频编码 worker，并通过控制 eventfd 唤醒 poll。
 */
static void video_worker_stop(VideoEncodeWorker *worker)
{
    if (!worker)
    {
        LOG_ERROR("video_worker_stop failed: worker is NULL");
        return;
    }
    if (!worker->ready)
        return;

    /* 先发布停止状态，再写 eventfd，保证线程醒来后立即观察到退出条件。 */
    pthread_mutex_lock(&worker->state_lock);
    worker->running = 0;
    pthread_mutex_unlock(&worker->state_lock);
    thread_message_queue_stop(&worker->command_queue);
    if (video_worker_notify_control(worker) != 0)
        LOG_ERROR("video_worker_stop failed: notify control event");
}

/**
 * @brief 释放单路视频编码 worker 的控制 eventfd、状态锁和命令队列。
 */
static void video_worker_deinit(VideoEncodeWorker *worker)
{
    int close_ret = 0;
    int mutex_ret = 0;

    if (!worker)
    {
        LOG_ERROR("video_worker_deinit failed: worker is NULL");
        return;
    }
    if (!worker->ready)
        return;

    video_worker_stop(worker);
    thread_message_queue_deinit(&worker->command_queue);
    if (worker->control_event_fd >= 0)
    {
        close_ret = close(worker->control_event_fd);
        if (close_ret != 0)
        {
            LOG_ERROR("video_worker_deinit failed: close event_fd=%d errno=%d(%s)",
                      worker->control_event_fd,
                      errno,
                      strerror(errno));
        }
        worker->control_event_fd = -1;
    }
    mutex_ret = pthread_mutex_destroy(&worker->state_lock);
    if (mutex_ret != 0)
    {
        LOG_ERROR("video_worker_deinit failed: pthread_mutex_destroy ret=%d(%s)",
                  mutex_ret,
                  strerror(mutex_ret));
    }
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
    if (!worker->ready || !video_worker_is_running(worker))
    {
        LOG_ERROR("media_gateway_pipeline_submit_video_command failed: worker not ready stream=%d ready=%d",
                  stream_idx,
                  worker->ready);
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
    if (video_worker_notify_control(worker) != 0)
    {
        LOG_ERROR("media_gateway_pipeline_submit_video_command failed: notify stream=%d type=%u request=%" PRIu64,
                  stream_idx,
                  command->type,
                  command->request_id);
        return -1;
    }
    return ret;
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
    ret = mpp_encoder_set_fps(&pipeline->ctx->video.streams[stream_idx].encoder,
                              request->target_fps);
    if (ret == MEDIA_OK)
    {
        result.applied_fps = request->target_fps;
        /*
         * 流配置由所属编码线程更新，避免 gateway 主循环与编码线程并发
         * 读写同一路 stream 配置。
         */
        pipeline->ctx->config.video.streams[stream_idx].fps = request->target_fps;
        ret = mpp_encoder_request_idr(&pipeline->ctx->video.streams[stream_idx].encoder);
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
    ret = mpp_encoder_apply_video_encode_params(&pipeline->ctx->video.streams[stream_idx].encoder,
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
        ret = mpp_encoder_request_idr(&pipeline->ctx->video.streams[stream_idx].encoder);
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
 * @description: 处理控制 eventfd，并执行当前已经入队的 MPP 控制命令。
 *
 * 本函数由视频编码线程调用，因此 MPP 控制命令与视频编码仍在同一线程内串行执行。
 */
static int video_worker_handle_control_event(MediaGatewayPipeline *pipeline,
                                             VideoEncodeWorker *worker,
                                             int stream_idx)
{
    int ret = 0;

    if (!pipeline || !pipeline->ctx || !worker ||
        stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("video_worker_handle_control_event failed: pipeline=%p ctx=%p worker=%p stream=%d",
                  (void *)pipeline,
                  pipeline ? (void *)pipeline->ctx : NULL,
                  (void *)worker,
                  stream_idx);
        return -1;
    }

    /* eventfd 仅负责唤醒；清空计数后，再从 command_queue 读取实际命令。 */
    ret = video_worker_drain_control_event(worker);
    if (ret != 0)
    {
        LOG_ERROR("video_worker_handle_control_event failed: drain control event stream=%d ret=%d",
                  stream_idx,
                  ret);
        return -1;
    }
    if (!pipeline->ctx->running || !video_worker_is_running(worker))
        return 0;

    video_worker_process_commands(pipeline, stream_idx);
    return 0;
}

/**
 * @description: 处理帧源 eventfd，取得最新视频帧并完成本轮编码。
 *
 * 主要流程：清空帧事件、取得最新帧、尽早释放帧源槽位、处理最新控制命令，
 * 最后调用当前视频流的编码与分发流程。
 */
static int video_worker_handle_frame_event(MediaGatewayPipeline *pipeline,
                                           VideoEncodeWorker *worker,
                                           MediaFrameSource *source,
                                           int stream_idx)
{
    MediaFrame frame = {0};
    int source_slot = -1;
    int ret = 0;

    if (!pipeline || !pipeline->ctx || !worker || !source ||
        stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("video_worker_handle_frame_event failed: pipeline=%p ctx=%p worker=%p source=%p stream=%d",
                  (void *)pipeline,
                  pipeline ? (void *)pipeline->ctx : NULL,
                  (void *)worker,
                  (void *)source,
                  stream_idx);
        return -1;
    }

    /* eventfd 只表示帧源状态发生变化，实际视频帧仍保存在 MediaFrameSource 中。 */
    ret = media_frame_source_drain_event(source);
    if (ret != 0)
    {
        LOG_ERROR("video_worker_handle_frame_event failed: drain frame event stream=%d source=%d ret=%d",
                  stream_idx,
                  source->config.source_index,
                  ret);
        return -1;
    }
    if (!pipeline->ctx->running || !video_worker_is_running(worker))
        return 0;

    ret = media_frame_source_try_acquire_latest(source, &frame, &source_slot);
    if (ret < 0)
    {
        LOG_ERROR("video_worker_handle_frame_event failed: acquire source stream=%d source=%d ret=%d",
                  stream_idx,
                  source->config.source_index,
                  ret);
        return -1;
    }
    if (ret == 0)
        return 0;

    /* frame 已持有 buffer 引用，可先释放 ring slot，避免编码期间占用帧源槽位。 */
    media_frame_source_release(source, source_slot);
    source_slot = -1;

    /* 处理等待帧期间新到达的命令，确保新配置在当前帧编码前生效。 */
    video_worker_process_commands(pipeline, stream_idx);
    if (!pipeline->ctx->running || !video_worker_is_running(worker))
    {
        media_frame_reset(&frame);
        return 0;
    }

    ret = media_gateway_process_stream(pipeline->ctx, &pipeline->state, &frame, stream_idx);
    if (ret != 0)
    {
        LOG_ERROR("video_worker_handle_frame_event failed: process stream=%d frame=%" PRIu64 " ret=%d",
                  stream_idx,
                  frame.frame_id,
                  ret);
        media_frame_reset(&frame);
        return -1;
    }

    media_frame_reset(&frame);
    return 0;
}

/**
 * @description: 视频编码 worker 主函数。
 */
static void *video_encode_thread_main(void *arg)
{
    VideoEncodeThreadArg *thread_arg = (VideoEncodeThreadArg *)arg;
    MediaGatewayPipeline *pipeline = NULL;
    VideoEncodeWorker *worker = NULL;
    MediaFrameSource *source = NULL;
    struct pollfd poll_fds[VIDEO_WORKER_POLL_COUNT] = {{0}};
    int stream_idx = -1;
    int source_event_fd = -1;
    int poll_ret = 0;
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

    worker = &pipeline->video.workers[stream_idx];
    source = worker->source;
    if (!worker->ready || !source)
    {
        LOG_ERROR("video_encode_thread_main failed: worker not ready stream=%d ready=%d source=%p",
                  stream_idx,
                  worker->ready,
                  (void *)source);
        free(thread_arg);
        pipeline_set_error(pipeline);
        return NULL;
    }
    source_event_fd = media_frame_source_get_event_fd(source);
    if (source_event_fd < 0)
    {
        LOG_ERROR("video_encode_thread_main failed: get source event fd stream=%d source=%d",
                  stream_idx,
                  source->config.source_index);
        free(thread_arg);
        pipeline_set_error(pipeline);
        return NULL;
    }

    poll_fds[VIDEO_WORKER_POLL_FRAME].fd = source_event_fd;
    poll_fds[VIDEO_WORKER_POLL_FRAME].events = POLLIN;
    poll_fds[VIDEO_WORKER_POLL_CONTROL].fd = worker->control_event_fd;
    poll_fds[VIDEO_WORKER_POLL_CONTROL].events = POLLIN;
    pipeline_set_thread_name("enc-", pipeline->ctx->config.video.streams[stream_idx].name, stream_idx);
    free(thread_arg);

    LOG_INFO("video encode worker started: stream=%d source=%d frame_event_fd=%d control_event_fd=%d",
             stream_idx,
             source->config.source_index,
             source_event_fd,
             worker->control_event_fd);

    while (pipeline->ctx->running && video_worker_is_running(worker))
    {
        /*
         * 帧源和控制命令各自维护 eventfd，poll 可以在没有超时轮询的情况下同时等待两类事件。
         * eventfd 只负责唤醒，实际帧和命令仍分别保存在 MediaFrameSource 与 command_queue。
         */
        poll_fds[VIDEO_WORKER_POLL_FRAME].revents = 0;
        poll_fds[VIDEO_WORKER_POLL_CONTROL].revents = 0;
        poll_ret = poll(poll_fds, VIDEO_WORKER_POLL_COUNT, -1);
        if (poll_ret < 0)
        {
            if (errno == EINTR)
                continue;
            LOG_ERROR("video encode thread failed: poll stream=%d errno=%d(%s)",
                      stream_idx,
                      errno,
                      strerror(errno));
            pipeline_set_error(pipeline);
            break;
        }

        if ((poll_fds[VIDEO_WORKER_POLL_FRAME].revents |
             poll_fds[VIDEO_WORKER_POLL_CONTROL].revents) &
            (POLLERR | POLLHUP | POLLNVAL))
        {
            LOG_ERROR("video encode thread failed: poll event stream=%d source_revents=0x%x control_revents=0x%x",
                      stream_idx,
                      poll_fds[VIDEO_WORKER_POLL_FRAME].revents,
                      poll_fds[VIDEO_WORKER_POLL_CONTROL].revents);
            pipeline_set_error(pipeline);
            break;
        }

        /* 控制事件优先处理，确保新配置在本轮即将编码的视频帧之前生效。 */
        if (poll_fds[VIDEO_WORKER_POLL_CONTROL].revents & POLLIN)
        {
            ret = video_worker_handle_control_event(pipeline, worker, stream_idx);
            if (ret != 0)
            {
                LOG_ERROR("video encode thread failed: handle control event stream=%d ret=%d",
                          stream_idx,
                          ret);
                pipeline_set_error(pipeline);
                break;
            }
            if (!pipeline->ctx->running || !video_worker_is_running(worker))
                break;
        }

        if (poll_fds[VIDEO_WORKER_POLL_FRAME].revents & POLLIN)
        {
            ret = video_worker_handle_frame_event(pipeline, worker, source, stream_idx);
            if (ret != 0)
            {
                LOG_ERROR("video encode thread failed: handle frame event stream=%d source=%d ret=%d",
                          stream_idx,
                          source->config.source_index,
                          ret);
                pipeline_set_error(pipeline);
                break;
            }
        }
    }
    LOG_INFO("video encode worker stopped: stream=%d source=%d",
             stream_idx,
             source->config.source_index);
    return NULL;
}

/**
 * @description: 音频 worker 将同一原始 PCM 帧依次送入全部去重编码组。
 *
 * 声道选择和采样率适配由 AudioEncoderManager 内部完成；worker 只负责等待帧源、
 * 枚举编码组和保证源 ring slot 在所有同步编码调用完成前保持有效。
 */
static void *audio_encode_thread_main(void *arg)
{
    MediaGatewayPipeline *pipeline = (MediaGatewayPipeline *)arg;
    AudioFrame source_frame = {0};
    AudioEncoderParams encoder_params = {0};
    AudioEncoderRuntimeGroupId group_id = AUDIO_ENCODER_INVALID_GROUP_ID;
    size_t group_count = 0;
    size_t group_index = 0;
    int source_slot = -1;
    int ret = 0;

    if (!pipeline || !pipeline->ctx || !pipeline->audio.source ||
        !pipeline->ctx->audio.encoder_manager)
    {
        LOG_ERROR("audio_encode_thread_main failed: pipeline=%p ctx=%p source=%p manager=%p",
                  (void *)pipeline,
                  pipeline ? (void *)pipeline->ctx : NULL,
                  pipeline ? (void *)pipeline->audio.source : NULL,
                  (pipeline && pipeline->ctx) ? (void *)pipeline->ctx->audio.encoder_manager : NULL);
        return NULL;
    }
    group_count = audio_encoder_manager_group_count(pipeline->ctx->audio.encoder_manager);
    if (group_count == 0)
    {
        LOG_ERROR("audio_encode_thread_main failed: no runtime encoder groups");
        pipeline_set_error(pipeline);
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

        source_frame.path_timing.encoder_input_ready_us = pipeline_now_us();
        /*
         * 同一采集帧依次驱动每个去重后的编码器；同参数配置不会重复编码。
         * 源 slot 必须保留到循环结束，因为每个组可能选择不同的采集声道。
         */
        ret = 0;
        for (group_index = 0; group_index < group_count; ++group_index)
        {
            memset(&encoder_params, 0, sizeof(encoder_params));
            group_id = AUDIO_ENCODER_INVALID_GROUP_ID;
            if (audio_encoder_manager_group_at(pipeline->ctx->audio.encoder_manager,
                                               group_index,
                                               &group_id,
                                               &encoder_params) != 0)
            {
                LOG_ERROR("audio encode thread failed: lookup group index=%zu frame=%" PRIu64,
                          group_index,
                          source_frame.frame_id);
                ret = -1;
                break;
            }
            if (media_gateway_process_audio_group(pipeline->ctx, &source_frame, group_id) != 0)
            {
                LOG_ERROR("audio encode thread failed: process group_id=%llu frame=%" PRIu64,
                          (unsigned long long)group_id,
                          source_frame.frame_id);
                ret = -1;
                break;
            }
        }
        audio_frame_source_release(pipeline->audio.source, source_slot);
        source_slot = -1;
        if (ret != 0)
        {
            pipeline_set_error(pipeline);
            break;
        }
    }
    if (source_slot >= 0)
        audio_frame_source_release(pipeline->audio.source, source_slot);
    return NULL;
}

/**
 * @description: 初始化 pipeline 的视频帧源引用、控制事件、音频帧源引用和同步状态。
 */
int media_gateway_pipeline_init(MediaGatewayPipeline *pipeline,
                                MediaGatewayCtx *ctx,
                                ThreadMessageQueue *result_queue,
                                MediaFrameSource *video_sources,
                                AudioFrameSource *audio_source)
{
    int i = 0;
    int source_idx = -1;
    int mutex_ret = 0;

    if (!pipeline || !ctx || !result_queue ||
        (ctx->config.video.stream_count > 0 && !video_sources) ||
        (ctx->config.audio.source.enabled && ctx->audio.capture_ready && !audio_source))
    {
        LOG_ERROR("media_gateway_pipeline_init failed: pipeline=%p ctx=%p result_queue=%p video_sources=%p audio_source=%p",
                  (void *)pipeline,
                  (void *)ctx,
                  (void *)result_queue,
                  (void *)video_sources,
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

    /* 每路启用 stream 独占一个帧源，worker 直接 poll 帧源和自己的控制 eventfd。 */
    for (i = 0; i < ctx->config.video.stream_count; ++i)
    {
        if (!ctx->video.streams[i].enabled)
            continue;
        source_idx = ctx->config.video.streams[i].source_index;
        if (source_idx < 0 || source_idx >= ctx->config.input.capture_source_count ||
            !ctx->video.captures[source_idx].ready)
        {
            LOG_ERROR("media_gateway_pipeline_init failed: invalid video source stream=%d source=%d capture_ready=%d",
                      i,
                      source_idx,
                      (source_idx >= 0 && source_idx < MEDIA_GATEWAY_MAX_CAPTURE_SOURCES)
                          ? ctx->video.captures[source_idx].ready
                          : 0);
            return -1;
        }
        if (video_worker_init(&pipeline->video.workers[i], &video_sources[source_idx]) != 0)
        {
            LOG_ERROR("media_gateway_pipeline_init failed: video worker stream=%d source=%d",
                      i,
                      source_idx);
            return -1;
        }
    }

    /* 音频 worker 直接持有帧源引用；帧源由 mediaGateway run resources 管理。 */
    if (ctx->config.audio.source.enabled && ctx->audio.capture_ready)
        pipeline->audio.source = audio_source;
    return 0;
}

/**
 * @description: 停止 pipeline 内由其拥有的视频编码 worker。
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
        if (!pipeline->ctx->video.streams[i].enabled)
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
    if (pipeline->ctx->config.audio.source.enabled && pipeline->ctx->audio.capture_ready)
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
 * @description: 唤醒并停止所有编码 worker，然后等待线程退出。
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
