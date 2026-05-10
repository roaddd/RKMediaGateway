#include "audioFrameSource.h"

#include "logger.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void audio_frame_source_make_abs_timeout(struct timespec *ts, int timeout_ms) {
    long nsec;
    clock_gettime(CLOCK_REALTIME, ts);
    if (timeout_ms < 0) timeout_ms = 0;
    ts->tv_sec += timeout_ms / 1000;
    nsec = ts->tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    ts->tv_sec += nsec / 1000000000L;
    ts->tv_nsec = nsec % 1000000000L;
}

/* 在线程外读取 running 标志，统一加锁，避免 stop 和采集线程竞争。 */
static int audio_frame_source_should_run(AudioFrameSource *source) {
    int running;
    if (!source) return 0;
    pthread_mutex_lock(&source->lock);
    running = source->running;
    pthread_mutex_unlock(&source->lock);
    return running;
}

/*
 * 寻找可写槽位。
 * 优先使用空槽；ring 满时覆盖最旧且未被 gateway 使用的帧，保持实时性优先。
 */
static int audio_frame_source_find_write_slot(AudioFrameSource *source) {
    int i;
    int oldest_idx = -1;
    uint64_t oldest_seq = 0;

    for (i = 0; i < source->slot_count; ++i) {
        if (!source->slots[i].valid && !source->slots[i].in_use) {
            return i;
        }
    }

    for (i = 0; i < source->slot_count; ++i) {
        if (source->slots[i].in_use || !source->slots[i].valid) {
            continue;
        }
        if (oldest_idx < 0 || source->slots[i].seq < oldest_seq) {
            oldest_idx = i;
            oldest_seq = source->slots[i].seq;
        }
    }

    if (oldest_idx >= 0) {
        source->dropped_frames++;
    }
    return oldest_idx;
}

/* 更新 read_slot 为当前最旧可读帧，保证消费者按音频时间顺序 drain。 */
static int audio_frame_source_update_read_slot(AudioFrameSource *source) {
    int i;
    int best = -1;
    uint64_t best_seq = 0;

    for (i = 0; i < source->slot_count; ++i) {
        if (!source->slots[i].valid || source->slots[i].in_use) {
            continue;
        }
        if (best < 0 || source->slots[i].seq < best_seq) {
            best = i;
            best_seq = source->slots[i].seq;
        }
    }
    source->read_slot = best;
    return best;
}

/* 将 AudioCaptureFrame 拷贝到 ring slot，并发布给 gateway 消费。 */
static int audio_frame_source_publish(AudioFrameSource *source, const AudioCaptureFrame *capture_frame) {
    int slot_idx;
    AudioFrameSourceSlot *slot;

    if (!source || !capture_frame || !capture_frame->data || capture_frame->size == 0) {
        LOG_ERROR("audio frame source publish failed: invalid frame");
        return -1;
    }

    pthread_mutex_lock(&source->lock);
    slot_idx = audio_frame_source_find_write_slot(source);
    if (slot_idx < 0) {
        source->dropped_frames++;
        pthread_mutex_unlock(&source->lock);
        return 0;
    }

    slot = &source->slots[slot_idx];
    if (capture_frame->size > slot->capacity) {
        source->fatal_error = 1;
        source->running = 0;
        pthread_cond_broadcast(&source->cond);
        pthread_mutex_unlock(&source->lock);
        LOG_ERROR("audio frame source slot too small size=%zu capacity=%zu", capture_frame->size, slot->capacity);
        return -1;
    }

    /*
     * 这里复制一次 PCM 到 ring slot。
     * 这样底层 AudioCaptureCtx 的 period_buffer 可以立刻复用，不会被上层消费速度拖住。
     */
    memcpy(slot->data, capture_frame->data, capture_frame->size);
    memset(&slot->frame, 0, sizeof(slot->frame));
    slot->frame.data = slot->data;
    slot->frame.size = capture_frame->size;
    slot->frame.frame_id = capture_frame->frame_id;
    slot->frame.pts_us = capture_frame->pts_us;
    slot->frame.sample_rate = capture_frame->sample_rate;
    slot->frame.channels = capture_frame->channels;
    slot->frame.format = capture_frame->format;
    slot->frame.samples_per_channel = capture_frame->samples_per_channel;
    slot->frame.capture_call_us = capture_frame->capture_call_us;
    slot->frame.read_us = capture_frame->read_us;
    slot->frame.xrun_count = capture_frame->xrun_count;
    slot->seq = source->next_seq++;
    slot->valid = 1;

    /* 更新可读槽位为最旧的帧，原因是人耳对音频连续性比较敏感？ */
    audio_frame_source_update_read_slot(source);

    /* 唤醒等待的消费者 */
    pthread_cond_signal(&source->cond);
    pthread_mutex_unlock(&source->lock);
    return 0;
}

/* 后台采集线程：只负责从设备取 PCM 并发布到 ring，不做编码或协议封装。 */
static void *audio_frame_source_thread(void *arg) {
    AudioFrameSource *source = (AudioFrameSource *)arg;
    AudioCaptureFrame frame;

    while (audio_frame_source_should_run(source)) {
        memset(&frame, 0, sizeof(frame));
        if (audio_capture_read_frame(source->capture, &frame) != 0) {
            source->consecutive_failures++;
            if (source->consecutive_failures >= source->max_consecutive_failures) {
                pthread_mutex_lock(&source->lock);
                source->fatal_error = 1;
                source->running = 0;
                pthread_cond_broadcast(&source->cond);
                pthread_mutex_unlock(&source->lock);
                LOG_ERROR("audio frame source failed continuously count=%d limit=%d",
                          source->consecutive_failures,
                          source->max_consecutive_failures);
                break;
            }
            usleep((useconds_t)source->retry_ms * 1000U);
            continue;
        }

        source->consecutive_failures = 0;
        if (!audio_frame_source_should_run(source)) {
            break;
        }
        if (audio_frame_source_publish(source, &frame) != 0) {
            break;
        }
    }

    pthread_mutex_lock(&source->lock);
    source->running = 0;
    pthread_cond_broadcast(&source->cond);
    pthread_mutex_unlock(&source->lock);
    return NULL;
}

/* 初始化 ring buffer。所有 slot 在启动前预分配，避免实时采集路径动态扩容。 */
int audio_frame_source_init(AudioFrameSource *source,
                            AudioCaptureCtx *capture,
                            int slot_count,
                            int retry_ms,
                            int max_consecutive_failures) {
    int i;
    size_t slot_capacity;

    if (!source || !capture || !capture->period_buffer_size) {
        LOG_ERROR("audio frame source init failed: invalid arguments");
        return -1;
    }
    memset(source, 0, sizeof(*source));
    source->capture = capture;
    source->slot_count = (slot_count > 0) ? slot_count : AUDIO_FRAME_SOURCE_DEFAULT_SLOTS;
    if (source->slot_count > AUDIO_FRAME_SOURCE_MAX_SLOTS) {
        source->slot_count = AUDIO_FRAME_SOURCE_MAX_SLOTS;
    }
    source->retry_ms = (retry_ms > 0) ? retry_ms : 5;
    source->max_consecutive_failures = (max_consecutive_failures > 0) ? max_consecutive_failures : 30;
    source->read_slot = -1;
    source->next_seq = 1;
    slot_capacity = capture->period_buffer_size;

    source->slots = (AudioFrameSourceSlot *)calloc((size_t)source->slot_count, sizeof(*source->slots));
    if (!source->slots) {
        LOG_ERROR("audio frame source init failed: slots alloc count=%d", source->slot_count);
        return -1;
    }
    for (i = 0; i < source->slot_count; ++i) {
        /* 为每个 slot 分配数据缓冲区 */
        source->slots[i].data = (uint8_t *)malloc(slot_capacity);
        if (!source->slots[i].data) {
            audio_frame_source_deinit(source);
            LOG_ERROR("audio frame source init failed: slot data alloc index=%d size=%zu", i, slot_capacity);
            return -1;
        }
        source->slots[i].capacity = slot_capacity;
    }
    if (pthread_mutex_init(&source->lock, NULL) != 0) {
        audio_frame_source_deinit(source);
        LOG_ERROR("audio frame source init failed: pthread_mutex_init");
        return -1;
    }
    source->mutex_ready = 1;
    if (pthread_cond_init(&source->cond, NULL) != 0) {
        pthread_mutex_destroy(&source->lock);
        source->mutex_ready = 0;
        audio_frame_source_deinit(source);
        LOG_ERROR("audio frame source init failed: pthread_cond_init");
        return -1;
    }
    source->cond_ready = 1;
    return 0;
}

/* 启动采集线程；线程和 AudioCaptureCtx 绑定，一路音频源一个线程。 */
int audio_frame_source_start(AudioFrameSource *source) {
    if (!source || !source->capture || !source->slots) {
        LOG_ERROR("audio frame source start failed: invalid arguments");
        return -1;
    }
    if (source->started) {
        LOG_ERROR("audio frame source start failed: already started");
        return -1;
    }
    source->running = 1;
    if (pthread_create(&source->thread, NULL, audio_frame_source_thread, source) != 0) {
        source->running = 0;
        LOG_ERROR("audio frame source start failed: pthread_create");
        return -1;
    }
    source->started = 1;
    LOG_INFO("audio frame source started slots=%d", source->slot_count);
    return 0;
}

/* 获取最旧可读音频帧；返回后调用方必须 release 对应 slot。 */
int audio_frame_source_acquire(AudioFrameSource *source,
                               AudioFrame *frame,
                               int *slot_index,
                               int timeout_ms) {
    struct timespec ts;
    int wait_ret;
    int idx;

    if (!source || !frame || !slot_index) {
        LOG_ERROR("audio frame source acquire failed: invalid arguments");
        return -1;
    }
    *slot_index = -1;
    memset(frame, 0, sizeof(*frame));
    /* 计算绝对超时时间 */
    audio_frame_source_make_abs_timeout(&ts, timeout_ms);

    pthread_mutex_lock(&source->lock);
    audio_frame_source_update_read_slot(source);
    while (!source->fatal_error && source->running && source->read_slot < 0) {
        wait_ret = pthread_cond_timedwait(&source->cond, &source->lock, &ts);
        if (wait_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&source->lock);
            return 0;
        }
        audio_frame_source_update_read_slot(source);
    }

    if (source->fatal_error) {
        LOG_ERROR("audio frame source acquire failed: fatal error");
        pthread_mutex_unlock(&source->lock);
        return -1;
    }
    idx = source->read_slot;
    if (idx < 0) {
        pthread_mutex_unlock(&source->lock);
        return 0;
    }
    source->slots[idx].in_use = 1;
    source->slots[idx].valid = 0;
    *frame = source->slots[idx].frame; /* 只是拷贝frame结构体，音频数据没有拷贝 */
    *slot_index = idx;
    audio_frame_source_update_read_slot(source);
    pthread_mutex_unlock(&source->lock);
    return 1;
}

/* 释放消费中的 slot，采集线程之后可以覆盖复用。 */
void audio_frame_source_release(AudioFrameSource *source, int slot_index) {
    if (!source || slot_index < 0 || slot_index >= source->slot_count) return;
    pthread_mutex_lock(&source->lock);
    source->slots[slot_index].in_use = 0;
    pthread_cond_signal(&source->cond);
    pthread_mutex_unlock(&source->lock);
}

/* 停止线程，等待 read loop 退出。 */
void audio_frame_source_stop(AudioFrameSource *source) {
    if (!source) return;
    if (source->mutex_ready) {
        pthread_mutex_lock(&source->lock);
        source->running = 0;
        if (source->cond_ready) {
            pthread_cond_broadcast(&source->cond);
        }
        pthread_mutex_unlock(&source->lock);
    } else {
        source->running = 0;
    }
    if (source->started) {
        pthread_join(source->thread, NULL);
        source->started = 0;
        LOG_INFO("audio frame source stopped");
    }
}

/* 释放 ring 和同步原语，内部会先 stop。 */
void audio_frame_source_deinit(AudioFrameSource *source) {
    int i;
    if (!source) return;
    audio_frame_source_stop(source);
    if (source->slots) {
        for (i = 0; i < source->slot_count; ++i) {
            free(source->slots[i].data);
        }
        free(source->slots);
    }
    if (source->cond_ready) {
        pthread_cond_destroy(&source->cond);
    }
    if (source->mutex_ready) {
        pthread_mutex_destroy(&source->lock);
    }
    memset(source, 0, sizeof(*source));
}

/* 线程安全读取丢帧计数，用于统计和压测。 */
uint64_t audio_frame_source_get_dropped_frames(AudioFrameSource *source) {
    uint64_t dropped;
    if (!source) return 0;
    pthread_mutex_lock(&source->lock);
    dropped = source->dropped_frames;
    pthread_mutex_unlock(&source->lock);
    return dropped;
}
