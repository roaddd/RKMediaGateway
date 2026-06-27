/**
 * @file mediaFrameSource.c
 * @brief 视频采集线程、最新帧缓存和采集控制命令处理。
 *
 * V4L2 取帧与运行期控制命令均在采集线程内执行，避免多个线程并发操作
 * 同一个 V4L2 文件描述符。外部线程只负责异步投递命令和接收执行结果。
 */

#include "mediaFrameSource.h"

#include "logger.h"
#include "mediaControlMessage.h"
#include "util.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MEDIA_FRAME_SOURCE_COMMAND_QUEUE_CAPACITY 8

/**
 * @brief 获取当前单调时钟时间，单位为微秒。
 *
 * 该时间用于采集链路耗时统计，不受系统时间被校准或跳变的影响。
 *
 * @return 成功返回当前单调时钟微秒值，失败返回 0。
 */
static uint64_t frame_source_now_us(void) {
    struct timespec ts = {0};
    int ret = 0;

    ret = clock_gettime(CLOCK_MONOTONIC, &ts);
    if (ret != 0)
    {
        LOG_ERROR("frame_source_now_us failed: clock_gettime errno=%d(%s)",
                  errno,
                  strerror(errno));
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @brief 读取采集线程运行标志。
 *
 * 运行标志由互斥锁保护，外部停止线程或线程内部发生致命错误时会清除此标志。
 *
 * @param source 采集源对象。
 * @return 1 表示线程应继续运行，0 表示应退出。
 */
static int frame_source_should_run(MediaFrameSource *source) {
    int running = 0;

    if (!source)
    {
        LOG_ERROR("frame_source_should_run failed: source is NULL");
        return 0;
    }
    pthread_mutex_lock(&source->runtime.lock);
    running = source->runtime.running;
    pthread_mutex_unlock(&source->runtime.lock);
    return running;
}

/**
 * @brief 根据当前实时时钟生成 pthread 条件变量等待的绝对超时时间。
 *
 * pthread_cond_timedwait 使用绝对时间，因此调用方传入相对超时时间后，需要在这里转换成
 * CLOCK_REALTIME 时间点。
 *
 * @param ts 输出的绝对超时时间。
 * @param timeout_ms 相对超时时间，单位毫秒；小于 0 时按 0 处理。
 */
static void frame_source_make_abs_timeout(struct timespec *ts, int timeout_ms) {
    long nsec = 0;
    int ret = 0;

    if (!ts)
    {
        LOG_ERROR("frame_source_make_abs_timeout failed: ts is NULL");
        return;
    }
    ret = clock_gettime(CLOCK_REALTIME, ts);
    if (ret != 0)
    {
        LOG_ERROR("frame_source_make_abs_timeout failed: clock_gettime errno=%d(%s)",
                  errno,
                  strerror(errno));
        memset(ts, 0, sizeof(*ts));
        return;
    }
    if (timeout_ms < 0) timeout_ms = 0;
    ts->tv_sec += timeout_ms / 1000;
    nsec = ts->tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    ts->tv_sec += nsec / 1000000000L;
    ts->tv_nsec = nsec % 1000000000L;
}

/**
 * @brief 将采集线程命令执行结果发送到 gateway 统一结果队列。
 */
static void frame_source_publish_command_result(MediaFrameSource *source,
                                                const ThreadMessage *command,
                                                int status,
                                                const void *data,
                                                size_t data_size)
{
    ThreadMessage result = {0};
    int push_ret = 0;

    if (!source || !command || !source->messaging.result_queue)
    {
        LOG_ERROR("frame_source_publish_command_result failed: source=%p command=%p result_queue=%p",
                  (void *)source,
                  (const void *)command,
                  source ? (void *)source->messaging.result_queue : NULL);
        return;
    }

    result.type = command->type;
    result.request_id = command->request_id;
    result.endpoint_type = MEDIA_CONTROL_ENDPOINT_CAPTURE;
    result.endpoint_index = source->config.source_index;
    result.status = status;
    result.data = (void *)data;
    result.data_size = data_size;
    push_ret = thread_message_queue_push_copy(source->messaging.result_queue, &result);
    if (push_ret != 0)
    {
        LOG_ERROR("frame source publish command result failed source=%d request=%" PRIu64 " ret=%d",
                  source->config.source_index,
                  command->request_id,
                  push_ret);
    }
}

/**
 * @brief 在采集线程中执行设置帧率命令。
 */
static void frame_source_execute_set_fps(MediaFrameSource *source,
                                         const ThreadMessage *command)
{
    const MediaSetFpsRequest *request = NULL;
    MediaSetFpsResult result = {0};
    int ret = -1;

    if (!source || !command ||
        command->data_size != sizeof(MediaSetFpsRequest) ||
        !command->data)
    {
        LOG_ERROR("frame_source_execute_set_fps failed: source=%p command=%p data=%p data_size=%zu expected=%zu",
                  (void *)source,
                  (const void *)command,
                  command ? command->data : NULL,
                  command ? command->data_size : 0,
                  sizeof(MediaSetFpsRequest));
        if (source && command)
            frame_source_publish_command_result(source, command, -1, NULL, 0);
        return;
    }

    request = (const MediaSetFpsRequest *)command->data;
    result.requested_fps = request->target_fps;
    ret = v4l2_capture_set_fps(source->config.capture, request->target_fps);
    if (ret == 0)
        result.applied_fps = request->target_fps;
    frame_source_publish_command_result(source,
                                        command,
                                        ret,
                                        &result,
                                        sizeof(result));
}

/**
 * @brief 处理采集线程当前积压的控制命令。
 *
 * 单次最多处理队列容量数量的命令，避免命令持续涌入时长期阻塞取帧。
 */
static void frame_source_process_commands(MediaFrameSource *source)
{
    ThreadMessage command = {0};
    int pop_ret = 0;
    int processed = 0;

    if (!source)
    {
        LOG_ERROR("frame_source_process_commands failed: source is NULL");
        return;
    }

    while (processed < MEDIA_FRAME_SOURCE_COMMAND_QUEUE_CAPACITY)
    {
        pop_ret = thread_message_queue_try_pop(&source->messaging.command_queue, &command);
        if (pop_ret < 0)
        {
            if (pop_ret != -3)
            {
                LOG_ERROR("frame_source_process_commands failed: pop command source=%d ret=%d",
                          source->config.source_index,
                          pop_ret);
            }
            break;
        }
        if (pop_ret == 0)
            break;

        switch (command.type)
        {
        case MEDIA_CONTROL_MESSAGE_SET_FPS:
            frame_source_execute_set_fps(source, &command);
            break;
        default:
            LOG_WARN("frame source ignored unknown command type=%u source=%d",
                     command.type,
                     source->config.source_index);
            frame_source_publish_command_result(source, &command, -1, NULL, 0);
            break;
        }
        thread_message_release(&command);
        processed++;
    }
}

/**
 * @brief 初始化 MediaFrame 对象。
 *
 * 清空帧结构体中的所有字段，确保调用方拿到的是无引用、无有效数据的干净状态。
 *
 * @param frame 待初始化的帧对象。
 */
void media_frame_init(MediaFrame *frame) {
    if (!frame)
    {
        LOG_ERROR("media_frame_init failed: frame is NULL");
        return;
    }
    memset(frame, 0, sizeof(*frame));
}

/**
 * @brief 拷贝一份 MediaFrame 引用。
 *
 * 该函数不会拷贝整帧图像数据，只复制帧描述信息并增加底层 V4L2 capture buffer 的引用计数。
 * 调用方使用完 dst 后必须调用 media_frame_reset 释放引用。
 *
 * @param dst 目标帧对象。
 * @param src 源帧对象。
 */
void media_frame_copy_ref(MediaFrame *dst, const MediaFrame *src) {
    if (!dst || !src)
    {
        LOG_ERROR("media_frame_copy_ref failed: dst=%p src=%p",
                  (void *)dst,
                  (const void *)src);
        return;
    }
    *dst = *src;
    if (dst->capture_buffer)
        v4l2_capture_buffer_ref(dst->capture_buffer);
}

/**
 * @brief 释放 MediaFrame 持有的底层 buffer 引用并清空结构体。
 *
 * 如果帧对象引用了 V4L2 capture buffer，会先减少引用计数，再将帧结构体恢复为空状态。
 *
 * @param frame 待释放的帧对象。
 */
void media_frame_reset(MediaFrame *frame) {
    if (!frame)
    {
        LOG_ERROR("media_frame_reset failed: frame is NULL");
        return;
    }
    if (frame->capture_buffer)
        v4l2_capture_buffer_unref(frame->capture_buffer);
    memset(frame, 0, sizeof(*frame));
}

/**
 * @brief 在 latest-frame 槽位数组中查找可写入槽位。
 *
 * 优先使用空槽；如果没有空槽，则复用尚未被消费者占用的旧帧槽位，并统计被覆盖的旧帧。
 * 该函数要求调用方已经持有 source->runtime.lock。
 *
 * @param source 采集源对象。
 * @return 成功返回槽位下标，失败返回 -1。
 */
static int frame_source_find_write_slot(MediaFrameSource *source) {
    int i = 0;

    if (!source)
    {
        LOG_ERROR("frame_source_find_write_slot failed: source is NULL");
        return -1;
    }

    /* (1)刚启动，还没有采集到帧; (2)某个槽位之前被消费完，并且已经 release */
    for (i = 0; i < MEDIA_FRAME_SOURCE_SLOTS; ++i) 
    {
        if (!source->runtime.slots[i].in_use && !source->runtime.slots[i].valid)
        {
            return i;
        }
    }

    /* 当前有一帧最新帧还没被编码线程拿走，但这个槽位没有正在被使用 */
    if (source->runtime.latest_slot >= 0 &&
        source->runtime.latest_slot < MEDIA_FRAME_SOURCE_SLOTS &&
        !source->runtime.slots[source->runtime.latest_slot].in_use)
    {
        source->runtime.dropped_frames++;
        return source->runtime.latest_slot;
    }

    /* 如果编码线程正在使用最新可消费的槽位，则会 */
    for (i = 0; i < MEDIA_FRAME_SOURCE_SLOTS; ++i) 
    {
        if (!source->runtime.slots[i].in_use)
        {
            if (source->runtime.slots[i].valid)
            {
                source->runtime.dropped_frames++;
            }
            return i;
        }
    }

    LOG_ERROR("frame source find writable slot failed: no writable slot");
    return -1;
}

/**
 * @brief 丢弃除指定槽位外的过期帧槽位。
 *
 * 发布新帧或消费最新帧时，只保留当前需要的槽位，未被占用的旧槽位会释放引用并标记为无效。
 * 该函数要求调用方已经持有 source->runtime.lock。
 *
 * @param source 采集源对象。
 * @param keep_slot 需要保留的槽位下标。
 */
static void frame_source_drop_stale_slots(MediaFrameSource *source, int keep_slot) {
    int i = 0;

    if (!source || keep_slot < 0 || keep_slot >= MEDIA_FRAME_SOURCE_SLOTS)
    {
        LOG_ERROR("frame_source_drop_stale_slots failed: source=%p keep_slot=%d",
                  (void *)source,
                  keep_slot);
        return;
    }
    for (i = 0; i < MEDIA_FRAME_SOURCE_SLOTS; ++i) {
        if (i == keep_slot) {
            continue;
        }
        if (!source->runtime.slots[i].in_use && source->runtime.slots[i].valid) {
            media_frame_reset(&source->runtime.slots[i].frame);
            source->runtime.slots[i].valid = 0;
            source->runtime.dropped_frames++;
        }
    }
}

/**
 * @brief 将采集到的一帧发布到 latest-frame 缓存。
 *
 * 发布过程只保存底层 buffer 引用，不做整帧数据拷贝；发布成功后会唤醒等待最新帧的消费者。
 *
 * @param source 采集源对象。
 * @param src_frame 待发布的采集帧。
 * @return 成功返回 0；参数非法返回 -1；无可写槽位时丢帧并返回 0。
 */
static int frame_source_publish_frame(MediaFrameSource *source, const MediaFrame *src_frame) {
    int slot_idx = -1;
    MediaFrameSourceSlot *slot = NULL;
    uint64_t publish_start_us = 0;
    uint64_t publish_done_us = 0;

    if (!source || !src_frame || !src_frame->raw_frame || src_frame->raw_len <= 0 || !src_frame->capture_buffer) {
        LOG_ERROR("frame_source_publish_frame failed: source=%p frame=%p raw=%p len=%d capture_buffer=%p",
                  (void *)source,
                  (const void *)src_frame,
                  src_frame ? (void *)src_frame->raw_frame : NULL,
                  src_frame ? src_frame->raw_len : 0,
                  src_frame ? (void *)src_frame->capture_buffer : NULL);
        return -1;
    }
    publish_start_us = frame_source_now_us();

    pthread_mutex_lock(&source->runtime.lock);
    slot_idx = frame_source_find_write_slot(source);
    if (slot_idx < 0) {
        source->runtime.dropped_frames++;
        pthread_mutex_unlock(&source->runtime.lock);
        LOG_ERROR("frame_source_publish_frame failed: no writable slot source=%d dropped=%" PRIu64,
                  source->config.source_index,
                  source->runtime.dropped_frames);
        return 0;
    }

    slot = &source->runtime.slots[slot_idx];
    media_frame_reset(&slot->frame);
    media_frame_copy_ref(&slot->frame, src_frame);
    slot->frame.metrics.frame_source_publish_copy_us = 0;
    slot->seq = source->runtime.next_seq++;
    slot->valid = 1;
    source->runtime.latest_slot = slot_idx;

    frame_source_drop_stale_slots(source, slot_idx);
    pthread_cond_signal(&source->runtime.cond);
    publish_done_us = frame_source_now_us();
    slot->frame.metrics.frame_source_publish_us = publish_done_us - publish_start_us;
    pthread_mutex_unlock(&source->runtime.lock);
    return 0;
}

/**
 * @brief 采集线程主函数，负责串行处理控制命令、采集视频帧并发布最新帧缓存。
 *
 * 该线程是 MediaFrameSource 的唯一采集执行上下文，所有直接操作 V4L2 采集设备的动作
 * 都在这里完成，包括 DQBUF 取帧和运行期控制命令（例如动态帧率调整）。这样可以避免主循环、
 * 编码线程或其它线程同时访问同一个 V4L2 句柄导致竞态。
 *
 * 线程循环的主要职责：
 * 1. 在取帧前后尝试处理命令队列，保证控制命令不会长期阻塞主循环；
 * 2. 从 V4L2 获取一帧数据，并以引用方式发布到 latest-frame 槽位；
 * 3. 统计采集链路耗时，供后续性能分析使用；
 * 4. 连续采集失败达到阈值时设置 fatal_error，并唤醒等待帧的消费者。
 *
 * @param arg MediaFrameSource 指针，由 media_frame_source_start 创建线程时传入。
 * @return 线程退出时固定返回 NULL。
 */
static void *frame_source_thread(void *arg) {
    MediaFrameSource *source = (MediaFrameSource *)arg;
    uint64_t capture_start_us = 0;
    uint64_t capture_end_us = 0;
    MediaFrame frame = {0}; /* 存放采集的一帧数据 */

    if (!source)
    {
        LOG_ERROR("frame_source_thread failed: arg is NULL");
        return NULL;
    }

    util_set_thread_name(source->config.thread_name);
    while (frame_source_should_run(source)) 
    {
        /* V4L2 控制命令必须由采集线程在两次 DQBUF 之间串行执行。 */
        frame_source_process_commands(source);
        media_frame_init(&frame);
        capture_start_us = frame_source_now_us();
        /* 采集一帧视频数据并接管 V4L2 buffer 引用，发布前不做整帧拷贝。 */
        if (v4l2_capture_acquire_frame(source->config.capture,
                                       &frame.raw_frame,
                                       &frame.raw_len,
                                       &frame.frame_id,
                                       &frame.dqbuf_ts_us,
                                       &frame.metrics.camera_buffer_wait_us,
                                       &frame.metrics.dqbuf_ioctl_duration_us,
                                       &frame.capture_buffer) != 0)
        {
            source->runtime.consecutive_failures++;
            if (source->runtime.consecutive_failures >= source->config.max_consecutive_failures) {
                pthread_mutex_lock(&source->runtime.lock);
                source->runtime.fatal_error = 1;
                source->runtime.running = 0;
                pthread_cond_broadcast(&source->runtime.cond);
                pthread_mutex_unlock(&source->runtime.lock);
                LOG_ERROR("frame source failed continuously count=%d limit=%d",
                          source->runtime.consecutive_failures,
                          source->config.max_consecutive_failures);
                break;
            }
            usleep((useconds_t)source->config.retry_ms * 1000U);
            continue;
        }
        /* 统计 V4L2 零拷贝取帧接口耗时。 */
        capture_end_us = frame_source_now_us();
        frame.metrics.capture_call_duration_us = capture_end_us - capture_start_us;
        frame.metrics.mmap_to_frame_cache_copy_us = 0;
        source->runtime.consecutive_failures = 0;

        if (!frame_source_should_run(source)) 
        {
            media_frame_reset(&frame);
            break;
        }
        /* 将采集帧引用发布到 latest-frame 槽位。 */
        if (frame_source_publish_frame(source, &frame) != 0) 
        {
            LOG_ERROR("frame_source_thread failed: publish frame source=%d frame=%" PRIu64,
                      source->config.source_index,
                      frame.frame_id);
            media_frame_reset(&frame);
            break;
        }
        media_frame_reset(&frame);
        /*
         * 当前帧发布完成后再执行控制命令，保证已经 DQBUF 的旧帧先按原顺序
         * 进入下游，同时把命令等待时间限制在一个帧周期左右。
         */
        frame_source_process_commands(source);
    }

    pthread_mutex_lock(&source->runtime.lock);
    source->runtime.running = 0;
    pthread_cond_broadcast(&source->runtime.cond);
    pthread_mutex_unlock(&source->runtime.lock);
    return NULL;
}

/**
 * @brief 初始化采集源对象。
 *
 * 初始化采集源配置、运行期状态、同步对象和控制命令队列。初始化成功后还不会启动线程，
 * 调用方需要继续调用 media_frame_source_start。
 *
 * @param source 待初始化的采集源对象。
 * @param capture 已打开的 V4L2 采集上下文。
 * @param thread_name 采集线程名称前缀。
 * @param retry_ms 采集失败后的重试等待时间，单位毫秒。
 * @param max_consecutive_failures 连续采集失败阈值。
 * @param source_index 采集源编号，用于日志和结果回传。
 * @param result_queue gateway 统一结果队列。
 * @return 成功返回 0，失败返回 -1。
 */
int media_frame_source_init(MediaFrameSource *source,
                            V4L2CaptureCtx *capture,
                            const char *thread_name,
                            int retry_ms,
                            int max_consecutive_failures,
                            int source_index,
                            ThreadMessageQueue *result_queue) {
    int queue_ret = 0;
    int mutex_ret = 0;
    int cond_ret = 0;

    if (!source || !capture || !result_queue)
    {
        LOG_ERROR("media_frame_source_init failed: source=%p capture=%p result_queue=%p source_index=%d",
                  (void *)source,
                  (void *)capture,
                  (void *)result_queue,
                  source_index);
        return -1;
    }
    memset(source, 0, sizeof(*source));
    source->config.capture = capture;
    snprintf(source->config.thread_name,
             sizeof(source->config.thread_name),
             "cap-%.11s",
             (thread_name && thread_name[0] != '\0') ? thread_name : "video");
    source->config.retry_ms = (retry_ms > 0) ? retry_ms : 5;
    source->config.max_consecutive_failures =
        (max_consecutive_failures > 0) ? max_consecutive_failures : 30;
    source->config.source_index = source_index;
    source->runtime.latest_slot = -1;
    source->runtime.next_seq = 1;
    source->messaging.result_queue = result_queue;
    mutex_ret = pthread_mutex_init(&source->runtime.lock, NULL);
    if (mutex_ret != 0) {
        LOG_ERROR("media_frame_source_init failed: pthread_mutex_init ret=%d(%s)",
                  mutex_ret,
                  strerror(mutex_ret));
        return -1;
    }
    cond_ret = pthread_cond_init(&source->runtime.cond, NULL);
    if (cond_ret != 0) {
        LOG_ERROR("media_frame_source_init failed: pthread_cond_init ret=%d(%s)",
                  cond_ret,
                  strerror(cond_ret));
        pthread_mutex_destroy(&source->runtime.lock);
        return -1;
    }
    queue_ret = thread_message_queue_init(&source->messaging.command_queue,
                                          MEDIA_FRAME_SOURCE_COMMAND_QUEUE_CAPACITY);
    if (queue_ret != 0)
    {
        pthread_cond_destroy(&source->runtime.cond);
        pthread_mutex_destroy(&source->runtime.lock);
        LOG_ERROR("media_frame_source_init failed: init command queue source=%d ret=%d",
                  source_index,
                  queue_ret);
        return -1;
    }
    return 0;
}

/**
 * @description: 非阻塞向采集线程控制队列投递命令。
 */
int media_frame_source_submit_command(MediaFrameSource *source,
                                      const ThreadMessage *command)
{
    int ret = 0;

    if (!source || !command)
    {
        LOG_ERROR("media_frame_source_submit_command failed: source=%p command=%p",
                  (void *)source,
                  (const void *)command);
        return -1;
    }
    ret = thread_message_queue_push_copy(&source->messaging.command_queue, command);
    if (ret != 0)
    {
        LOG_ERROR("media_frame_source_submit_command failed: source=%d type=%u request=%" PRIu64 " ret=%d",
                  source->config.source_index,
                  command->type,
                  command->request_id,
                  ret);
    }
    return ret;
}

/**
 * @brief 启动采集线程。
 *
 * 设置运行标志并创建采集线程。采集线程启动后会持续取帧，并异步处理提交到命令队列的控制命令。
 *
 * @param source 采集源对象。
 * @return 成功返回 0，失败返回 -1。
 */
int media_frame_source_start(MediaFrameSource *source) {
    int thread_ret = 0;

    if (!source || !source->config.capture) {
        LOG_ERROR("media_frame_source_start failed: source=%p capture=%p",
                  (void *)source,
                  source ? (void *)source->config.capture : NULL);
        return -1;
    }
    /* 防止重复开启采集线程 */
    if (source->runtime.started) {
        LOG_ERROR("media_frame_source_start failed: already started source=%d",
                  source->config.source_index);
        return -1;
    }

    source->runtime.running = 1;
    /* 创建视频采集线程 */
    thread_ret = pthread_create(&source->runtime.thread, NULL, frame_source_thread, source);
    if (thread_ret != 0) {
        source->runtime.running = 0;
        LOG_ERROR("media_frame_source_start failed: pthread_create source=%d ret=%d(%s)",
                  source->config.source_index,
                  thread_ret,
                  strerror(thread_ret));
        return -1;
    }
    source->runtime.started = 1;
    LOG_INFO("frame source started");
    return 0;
}

/**
 * @brief 等待并获取最新的一帧采集数据。
 *
 * 该函数面向编码线程等消费者使用，会在指定超时时间内等待新帧。成功获取后返回的是帧引用，
 * 调用方处理完成后必须调用 media_frame_source_release 释放对应槽位。
 *
 * @param source 采集源对象。
 * @param frame 输出的帧引用。
 * @param slot_index 输出的槽位下标，后续释放时使用。
 * @param timeout_ms 等待最新帧的超时时间，单位毫秒。
 * @return 1 表示获取到新帧；0 表示超时或当前无新帧；-1 表示发生错误。
 */
int media_frame_source_acquire_latest(MediaFrameSource *source,
                                      MediaFrame *frame,
                                      int *slot_index,
                                      int timeout_ms) {
    struct timespec ts = {0};
    int wait_ret = 0;
    int idx = -1;

    if (!source || !frame || !slot_index) {
        LOG_ERROR("media_frame_source_acquire_latest failed: source=%p frame=%p slot_index=%p timeout_ms=%d",
                  (void *)source,
                  (void *)frame,
                  (void *)slot_index,
                  timeout_ms);
        return -1;
    }
    *slot_index = -1;
    media_frame_init(frame);
    frame_source_make_abs_timeout(&ts, timeout_ms);

    pthread_mutex_lock(&source->runtime.lock);
    /* TODO:这里视频帧没有最新帧时，会阻塞音频帧放入编码队列吗，有必要优化吗 */
    while (!source->runtime.fatal_error && source->runtime.running &&
           (source->runtime.latest_slot < 0 ||
            !source->runtime.slots[source->runtime.latest_slot].valid ||
            source->runtime.slots[source->runtime.latest_slot].seq == source->runtime.consumed_seq)) {
        wait_ret = pthread_cond_timedwait(&source->runtime.cond,
                                          &source->runtime.lock,
                                          &ts);
        if (wait_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&source->runtime.lock);
            return 0;
        }
        if (wait_ret != 0)
        {
            LOG_ERROR("media_frame_source_acquire_latest failed: pthread_cond_timedwait source=%d ret=%d(%s)",
                      source->config.source_index,
                      wait_ret,
                      strerror(wait_ret));
            pthread_mutex_unlock(&source->runtime.lock);
            return -1;
        }
    }

    if (source->runtime.fatal_error) {
        LOG_ERROR("media_frame_source_acquire_latest failed: fatal error source=%d failures=%d",
                  source->config.source_index,
                  source->runtime.consecutive_failures);
        pthread_mutex_unlock(&source->runtime.lock);
        return -1;
    }

    if (source->runtime.latest_slot < 0 ||
        !source->runtime.slots[source->runtime.latest_slot].valid) {
        pthread_mutex_unlock(&source->runtime.lock);
        return 0;
    }

    idx = source->runtime.latest_slot;
    frame_source_drop_stale_slots(source, idx);
    source->runtime.slots[idx].in_use = 1;
    source->runtime.slots[idx].valid = 0;
    source->runtime.consumed_seq = source->runtime.slots[idx].seq;
    source->runtime.latest_slot = -1;
    media_frame_copy_ref(frame, &source->runtime.slots[idx].frame);
    *slot_index = idx;
    pthread_mutex_unlock(&source->runtime.lock);
    return 1;
}

/**
 * @brief 释放消费者占用的 latest-frame 槽位。
 *
 * 消费者完成编码或其它处理后调用该函数，释放槽位中的帧引用，并唤醒可能等待可写槽位的采集线程。
 *
 * @param source 采集源对象。
 * @param slot_index 需要释放的槽位下标。
 */
void media_frame_source_release(MediaFrameSource *source, int slot_index) {
    if (!source || slot_index < 0 || slot_index >= MEDIA_FRAME_SOURCE_SLOTS)
    {
        LOG_ERROR("media_frame_source_release failed: source=%p slot_index=%d valid_range=[0,%d)",
                  (void *)source,
                  slot_index,
                  MEDIA_FRAME_SOURCE_SLOTS);
        return;
    }
    pthread_mutex_lock(&source->runtime.lock);
    source->runtime.slots[slot_index].in_use = 0;
    media_frame_reset(&source->runtime.slots[slot_index].frame);
    pthread_cond_signal(&source->runtime.cond);
    pthread_mutex_unlock(&source->runtime.lock);
}

/**
 * @brief 停止采集线程。
 *
 * 清除运行标志、唤醒等待者、停止命令队列，并等待采集线程退出。该函数可在反初始化流程中重复调用。
 *
 * @param source 采集源对象。
 */
void media_frame_source_stop(MediaFrameSource *source) {
    int join_ret = 0;

    if (!source)
    {
        LOG_ERROR("media_frame_source_stop failed: source is NULL");
        return;
    }
    pthread_mutex_lock(&source->runtime.lock);
    source->runtime.running = 0;
    pthread_cond_broadcast(&source->runtime.cond);
    pthread_mutex_unlock(&source->runtime.lock);
    thread_message_queue_stop(&source->messaging.command_queue);

    if (source->runtime.started) {
        join_ret = pthread_join(source->runtime.thread, NULL);
        if (join_ret != 0)
        {
            LOG_ERROR("media_frame_source_stop failed: pthread_join source=%d ret=%d(%s)",
                      source->config.source_index,
                      join_ret,
                      strerror(join_ret));
            return;
        }
        source->runtime.started = 0;
        LOG_INFO("frame source stopped");
    }
}

/**
 * @brief 反初始化采集源对象。
 *
 * 停止采集线程，释放所有帧槽位引用，销毁同步对象和命令队列，最后清空采集源结构体。
 *
 * @param source 采集源对象。
 */
void media_frame_source_deinit(MediaFrameSource *source) {
    int i = 0;
    int cond_ret = 0;
    int mutex_ret = 0;

    if (!source)
    {
        LOG_ERROR("media_frame_source_deinit failed: source is NULL");
        return;
    }
    media_frame_source_stop(source);
    if (source->runtime.started)
    {
        LOG_ERROR("media_frame_source_deinit failed: worker still started source=%d",
                  source->config.source_index);
        return;
    }
    for (i = 0; i < MEDIA_FRAME_SOURCE_SLOTS; ++i)
        media_frame_reset(&source->runtime.slots[i].frame);
    cond_ret = pthread_cond_destroy(&source->runtime.cond);
    if (cond_ret != 0)
    {
        LOG_ERROR("media_frame_source_deinit failed: pthread_cond_destroy source=%d ret=%d(%s)",
                  source->config.source_index,
                  cond_ret,
                  strerror(cond_ret));
    }
    mutex_ret = pthread_mutex_destroy(&source->runtime.lock);
    if (mutex_ret != 0)
    {
        LOG_ERROR("media_frame_source_deinit failed: pthread_mutex_destroy source=%d ret=%d(%s)",
                  source->config.source_index,
                  mutex_ret,
                  strerror(mutex_ret));
    }
    thread_message_queue_deinit(&source->messaging.command_queue);
    memset(source, 0, sizeof(*source));
}
