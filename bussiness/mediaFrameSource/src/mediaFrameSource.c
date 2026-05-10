#include "mediaFrameSource.h"

#include "logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t frame_source_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int frame_source_should_run(MediaFrameSource *source) {
    int running;
    if (!source) return 0;
    pthread_mutex_lock(&source->lock);
    running = source->running;
    pthread_mutex_unlock(&source->lock);
    return running;
}

static void frame_source_make_abs_timeout(struct timespec *ts, int timeout_ms) {
    long nsec;
    clock_gettime(CLOCK_REALTIME, ts);
    if (timeout_ms < 0) timeout_ms = 0;
    ts->tv_sec += timeout_ms / 1000;
    nsec = ts->tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    ts->tv_sec += nsec / 1000000000L;
    ts->tv_nsec = nsec % 1000000000L;
}

static int frame_source_slot_ensure_capacity(MediaFrameSourceSlot *slot, size_t need_size) {
    uint8_t *new_data;
    if (!slot) {
        LOG_ERROR("frame source ensure slot capacity failed: slot is NULL need=%zu", need_size);
        return -1;
    }
    if (slot->capacity >= need_size) return 0;

    new_data = (uint8_t *)realloc(slot->data, need_size);
    if (!new_data) {
        LOG_ERROR("frame source realloc slot failed need=%zu", need_size);
        return -1;
    }
    slot->data = new_data;
    slot->capacity = need_size;
    return 0;
}

static int frame_source_find_write_slot(MediaFrameSource *source) {
    int i;

    /* (1)刚启动，还没有采集到帧; (2)某个槽位之前被消费完，并且已经 release */
    for (i = 0; i < MEDIA_FRAME_SOURCE_SLOTS; ++i) 
    {
        if (!source->slots[i].in_use && !source->slots[i].valid) 
        {
            return i;
        }
    }

    /* 当前有一帧最新帧还没被编码线程拿走，但这个槽位没有正在被使用 */
    if (source->latest_slot >= 0 &&
        source->latest_slot < MEDIA_FRAME_SOURCE_SLOTS &&
        !source->slots[source->latest_slot].in_use) 
    {
        source->dropped_frames++;
        return source->latest_slot;
    }

    /* 如果编码线程正在使用最新可消费的槽位，则会 */
    for (i = 0; i < MEDIA_FRAME_SOURCE_SLOTS; ++i) 
    {
        if (!source->slots[i].in_use) 
        {
            if (source->slots[i].valid)
            {
                source->dropped_frames++;
            }
            return i;
        }
    }

    LOG_ERROR("frame source find writable slot failed: no writable slot");
    return -1;
}

static void frame_source_drop_stale_slots(MediaFrameSource *source, int keep_slot) {
    int i;
    for (i = 0; i < MEDIA_FRAME_SOURCE_SLOTS; ++i) {
        if (i == keep_slot) {
            continue;
        }
        if (!source->slots[i].in_use && source->slots[i].valid) {
            source->slots[i].valid = 0;
            source->dropped_frames++;
        }
    }
}

static int frame_source_publish_frame(MediaFrameSource *source, const MediaFrame *src_frame) {
    int slot_idx;
    MediaFrameSourceSlot *slot;
    size_t copy_len;

    if (!source || !src_frame || !src_frame->raw_frame || src_frame->raw_len <= 0) {
        LOG_ERROR("frame source publish frame failed: invalid arguments");
        return -1;
    }
    copy_len = (size_t)src_frame->raw_len;

    pthread_mutex_lock(&source->lock);
    slot_idx = frame_source_find_write_slot(source);
    if (slot_idx < 0) {
        source->dropped_frames++;
        pthread_mutex_unlock(&source->lock);
        return 0;
    }

    slot = &source->slots[slot_idx];
    if (frame_source_slot_ensure_capacity(slot, copy_len) != 0) {
        LOG_ERROR("frame source publish frame failed: ensure slot capacity slot=%d size=%zu", slot_idx, copy_len);
        source->fatal_error = 1;
        source->running = 0;
        pthread_cond_broadcast(&source->cond);
        pthread_mutex_unlock(&source->lock);
        return -1;
    }

    memcpy(slot->data, src_frame->raw_frame, copy_len); /* 从源帧复制数据到采集线程的队列 */
    slot->frame = *src_frame;
    slot->frame.raw_frame = slot->data;
    slot->seq = source->next_seq++;
    slot->valid = 1;
    source->latest_slot = slot_idx;

    frame_source_drop_stale_slots(source, slot_idx);
    pthread_cond_signal(&source->cond);
    pthread_mutex_unlock(&source->lock);
    return 0;
}

static void *frame_source_thread(void *arg) {
    MediaFrameSource *source = (MediaFrameSource *)arg;
    uint64_t capture_start_us;
    uint64_t capture_end_us;
    MediaFrame frame; /* 存放采集的一帧数据 */

    while (frame_source_should_run(source)) 
    {
        memset(&frame, 0, sizeof(frame));
        capture_start_us = frame_source_now_us();
        if (v4l2_capture_frame(source->capture,
                               &frame.raw_frame,
                               &frame.raw_len,
                               &frame.frame_id,
                               &frame.dqbuf_ts_us,
                               &frame.driver_to_dqbuf_us,
                               &frame.dqbuf_ioctl_us,
                               &frame.frame_copy_us) != 0) 
        {
            source->consecutive_failures++;
            if (source->consecutive_failures >= source->max_consecutive_failures) {
                pthread_mutex_lock(&source->lock);
                source->fatal_error = 1;
                source->running = 0;
                pthread_cond_broadcast(&source->cond);
                pthread_mutex_unlock(&source->lock);
                LOG_ERROR("frame source failed continuously count=%d limit=%d",
                          source->consecutive_failures,
                          source->max_consecutive_failures);
                break;
            }
            usleep((useconds_t)source->retry_ms * 1000U);
            continue;
        }
        /* 统计v4l2_capture_frame接口耗时 */
        capture_end_us = frame_source_now_us();
        frame.capture_call_us = capture_end_us - capture_start_us;
        source->consecutive_failures = 0;

        if (!frame_source_should_run(source)) 
        {
            break;
        }

        if (frame_source_publish_frame(source, &frame) != 0) 
        {
            break;
        }
    }

    pthread_mutex_lock(&source->lock);
    source->running = 0;
    pthread_cond_broadcast(&source->cond);
    pthread_mutex_unlock(&source->lock);
    return NULL;
}

int media_frame_source_init(MediaFrameSource *source,
                            V4L2CaptureCtx *capture,
                            int retry_ms,
                            int max_consecutive_failures) {
    if (!source || !capture) 
    {
        LOG_ERROR("frame source init failed: invalid arguments");
        return -1;
    }
    memset(source, 0, sizeof(*source));
    source->capture = capture;
    source->retry_ms = (retry_ms > 0) ? retry_ms : 5;
    source->max_consecutive_failures = (max_consecutive_failures > 0) ? max_consecutive_failures : 30;
    source->latest_slot = -1;
    source->next_seq = 1;
    if (pthread_mutex_init(&source->lock, NULL) != 0) {
        LOG_ERROR("frame source init failed: pthread_mutex_init");
        return -1;
    }
    if (pthread_cond_init(&source->cond, NULL) != 0) {
        pthread_mutex_destroy(&source->lock);
        LOG_ERROR("frame source init failed: pthread_cond_init");
        return -1;
    }
    return 0;
}

int media_frame_source_start(MediaFrameSource *source) {
    if (!source || !source->capture) {
        LOG_ERROR("frame source start failed: invalid arguments");
        return -1;
    }
    if (source->started) {
        LOG_ERROR("frame source start failed: already started");
        return -1;
    }

    source->running = 1;
    if (pthread_create(&source->thread, NULL, frame_source_thread, source) != 0) {
        source->running = 0;
        LOG_ERROR("frame source start failed: pthread_create");
        return -1;
    }
    source->started = 1;
    LOG_INFO("frame source started");
    return 0;
}

int media_frame_source_acquire_latest(MediaFrameSource *source,
                                      MediaFrame *frame,
                                      int *slot_index,
                                      int timeout_ms) {
    struct timespec ts;
    int wait_ret = 0;
    int idx;

    if (!source || !frame || !slot_index) {
        LOG_ERROR("frame source acquire_latest failed: invalid arguments");
        return -1;
    }
    *slot_index = -1;
    memset(frame, 0, sizeof(*frame));
    frame_source_make_abs_timeout(&ts, timeout_ms);

    pthread_mutex_lock(&source->lock);
    /* TODO:这里视频帧没有最新帧时，会阻塞音频帧放入编码队列吗，有必要优化吗 */
    while (!source->fatal_error && source->running &&
           (source->latest_slot < 0 ||
            !source->slots[source->latest_slot].valid ||
            source->slots[source->latest_slot].seq == source->consumed_seq)) {
        wait_ret = pthread_cond_timedwait(&source->cond, &source->lock, &ts);
        if (wait_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&source->lock);
            return 0;
        }
    }

    if (source->fatal_error) {
        LOG_ERROR("frame source acquire_latest failed: fatal error");
        pthread_mutex_unlock(&source->lock);
        return -1;
    }

    if (source->latest_slot < 0 || !source->slots[source->latest_slot].valid) {
        pthread_mutex_unlock(&source->lock);
        return 0;
    }

    idx = source->latest_slot;
    frame_source_drop_stale_slots(source, idx);
    source->slots[idx].in_use = 1;
    source->slots[idx].valid = 0;
    source->consumed_seq = source->slots[idx].seq;
    source->latest_slot = -1;
    *frame = source->slots[idx].frame;
    *slot_index = idx;
    pthread_mutex_unlock(&source->lock);
    return 1;
}

void media_frame_source_release(MediaFrameSource *source, int slot_index) {
    if (!source || slot_index < 0 || slot_index >= MEDIA_FRAME_SOURCE_SLOTS) return;
    pthread_mutex_lock(&source->lock);
    source->slots[slot_index].in_use = 0;
    pthread_cond_signal(&source->cond);
    pthread_mutex_unlock(&source->lock);
}

void media_frame_source_stop(MediaFrameSource *source) {
    if (!source) return;
    pthread_mutex_lock(&source->lock);
    source->running = 0;
    pthread_cond_broadcast(&source->cond);
    pthread_mutex_unlock(&source->lock);

    if (source->started) {
        pthread_join(source->thread, NULL);
        source->started = 0;
        LOG_INFO("frame source stopped");
    }
}

void media_frame_source_deinit(MediaFrameSource *source) {
    int i;
    if (!source) return;
    media_frame_source_stop(source);
    for (i = 0; i < MEDIA_FRAME_SOURCE_SLOTS; ++i) {
        free(source->slots[i].data);
        source->slots[i].data = NULL;
        source->slots[i].capacity = 0;
    }
    pthread_cond_destroy(&source->cond);
    pthread_mutex_destroy(&source->lock);
    memset(source, 0, sizeof(*source));
}
