#include "mediaGatewayCaptureWorker.h"
#include "logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * @description: 获取单调时钟时间，单位微秒，用于采集链路耗时统计。
 * @return {uint64_t} 当前单调时钟时间戳。
 */
static uint64_t capture_worker_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 线程安全地读取 worker 运行标志。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @return {int} 1 表示继续运行，0 表示应退出。
 */
static int capture_worker_should_run(MediaGatewayCaptureWorker *worker) {
    int running;
    if (!worker) return 0;
    pthread_mutex_lock(&worker->lock);
    running = worker->running;
    pthread_mutex_unlock(&worker->lock);
    return running;
}

/**
 * @description: 将相对超时时间转换成 pthread_cond_timedwait 需要的绝对时间。
 * @param {struct timespec *} ts 输出绝对超时时间。
 * @param {int} timeout_ms 相对超时时间，单位毫秒。
 * @return {void}
 */
static void capture_worker_make_abs_timeout(struct timespec *ts, int timeout_ms) {
    long nsec;
    clock_gettime(CLOCK_REALTIME, ts);
    if (timeout_ms < 0) timeout_ms = 0;
    ts->tv_sec += timeout_ms / 1000;
    nsec = ts->tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    ts->tv_sec += nsec / 1000000000L;
    ts->tv_nsec = nsec % 1000000000L;
}

/**
 * @description: 确保单个槽位有足够空间保存一帧 NV12 数据。
 * @param {MediaGatewayCaptureSlot *} slot 待检查的槽位。
 * @param {size_t} need_size 当前帧所需字节数。
 * @return {int} 0 成功，-1 分配失败。
 */
static int capture_slot_ensure_capacity(MediaGatewayCaptureSlot *slot, size_t need_size) {
    uint8_t *new_data;
    if (!slot) return -1;
    if (slot->capacity >= need_size) return 0;

    new_data = (uint8_t *)realloc(slot->data, need_size);
    if (!new_data) {
        LOG_ERROR("capture worker realloc slot failed need=%zu", need_size);
        return -1;
    }
    slot->data = new_data;
    slot->capacity = need_size;
    return 0;
}

/**
 * @description: 选择采集线程本次写入的槽位。
 * @details 优先使用空闲槽位；没有空闲槽位时覆盖尚未消费的旧帧；正在编码线程使用的槽位不会被覆盖。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @return {int} 可写槽位下标，-1 表示所有槽位都正在被消费。
 */
static int capture_worker_find_write_slot(MediaGatewayCaptureWorker *worker) {
    int i;

    for (i = 0; i < MEDIA_GATEWAY_CAPTURE_WORKER_SLOTS; ++i) {
        if (!worker->slots[i].in_use && !worker->slots[i].valid) {
            return i;
        }
    }

    if (worker->latest_slot >= 0 &&
        worker->latest_slot < MEDIA_GATEWAY_CAPTURE_WORKER_SLOTS &&
        !worker->slots[worker->latest_slot].in_use) {
        worker->dropped_frames++;
        return worker->latest_slot;
    }

    for (i = 0; i < MEDIA_GATEWAY_CAPTURE_WORKER_SLOTS; ++i) {
        if (!worker->slots[i].in_use) {
            if (worker->slots[i].valid) {
                worker->dropped_frames++;
            }
            return i;
        }
    }

    return -1;
}

/**
 * @description: 丢弃除 keep_slot 外的旧帧，只保留最新帧给编码线程。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @param {int} keep_slot 需要保留的最新槽位。
 * @return {void}
 */
static void capture_worker_drop_stale_slots(MediaGatewayCaptureWorker *worker, int keep_slot) {
    int i;
    for (i = 0; i < MEDIA_GATEWAY_CAPTURE_WORKER_SLOTS; ++i) {
        if (i == keep_slot) continue;
        if (!worker->slots[i].in_use && worker->slots[i].valid) {
            worker->slots[i].valid = 0;
            worker->dropped_frames++;
        }
    }
}

/**
 * @description: 将 V4L2 采到的一帧发布到 worker 槽位，并唤醒等待编码的主线程。
 * @details v4l2Capture 内部 frame_cache 会被下一次采集复用，所以这里必须复制一份到 worker 槽位。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @param {MediaGatewayCapturedFrame *} src_frame 本次采集帧元信息和数据指针。
 * @return {int} 0 成功或本帧被丢弃，-1 出现不可恢复错误。
 */
static int capture_worker_publish_frame(MediaGatewayCaptureWorker *worker,
                                        const MediaGatewayCapturedFrame *src_frame) {
    int slot_idx;
    MediaGatewayCaptureSlot *slot;
    size_t copy_len;

    if (!worker || !src_frame || !src_frame->raw_frame || src_frame->raw_len <= 0) {
        LOG_ERROR("capture worker publish frame failed: invalid arguments");
        return -1;
    }
    copy_len = (size_t)src_frame->raw_len;

    pthread_mutex_lock(&worker->lock);
    slot_idx = capture_worker_find_write_slot(worker);
    if (slot_idx < 0) {
        worker->dropped_frames++;
        pthread_mutex_unlock(&worker->lock);
        return 0;
    }

    slot = &worker->slots[slot_idx];
    // 确保槽位有足够容量保存当前帧数据，失败则标记 worker 进入 fatal 状态。
    if (capture_slot_ensure_capacity(slot, copy_len) != 0) {
        worker->fatal_error = 1;
        worker->running = 0;
        pthread_cond_broadcast(&worker->cond);
        pthread_mutex_unlock(&worker->lock);
        return -1;
    }

    memcpy(slot->data, src_frame->raw_frame, copy_len);
    slot->frame = *src_frame;
    slot->frame.raw_frame = slot->data;
    slot->seq = worker->next_seq++;
    slot->valid = 1;
    worker->latest_slot = slot_idx;

    capture_worker_drop_stale_slots(worker, slot_idx);
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->lock);
    return 0;
}

/**
 * @description: 采集线程入口，持续从 V4L2 取帧并发布最新帧。
 * @details 该线程独立承担 VIDIOC_DQBUF 等帧、QBUF 和采集拷贝，使主线程编码时不再阻塞下一帧采集。
 * @param {void *} arg MediaGatewayCaptureWorker 指针。
 * @return {void *} pthread 线程返回值。
 */
static void *capture_worker_thread(void *arg) {
    MediaGatewayCaptureWorker *worker = (MediaGatewayCaptureWorker *)arg;
    uint64_t capture_start_us;
    uint64_t capture_end_us;
    MediaGatewayCapturedFrame frame;

    while (capture_worker_should_run(worker)) {
        memset(&frame, 0, sizeof(frame));
        capture_start_us = capture_worker_now_us();
        if (v4l2_capture_frame(worker->capture,
                               &frame.raw_frame,
                               &frame.raw_len,
                               &frame.frame_id,
                               &frame.dqbuf_ts_us,
                               &frame.driver_to_dqbuf_us,
                               &frame.dqbuf_ioctl_us,
                               &frame.frame_copy_us) != 0) {
            worker->consecutive_failures++;
            if (worker->consecutive_failures >= worker->max_consecutive_failures) {
                pthread_mutex_lock(&worker->lock);
                worker->fatal_error = 1;
                worker->running = 0;
                pthread_cond_broadcast(&worker->cond);
                pthread_mutex_unlock(&worker->lock);
                LOG_ERROR("capture worker failed continuously count=%d limit=%d",
                          worker->consecutive_failures,
                          worker->max_consecutive_failures);
                break;
            }
            usleep((useconds_t)worker->retry_ms * 1000U);
            continue;
        }

        capture_end_us = capture_worker_now_us();
        frame.capture_call_us = capture_end_us - capture_start_us;
        worker->consecutive_failures = 0;
        if (!capture_worker_should_run(worker)) {
            break;
        }
        if (capture_worker_publish_frame(worker, &frame) != 0) {
            break;
        }
    }

    pthread_mutex_lock(&worker->lock);
    worker->running = 0;
    pthread_cond_broadcast(&worker->cond);
    pthread_mutex_unlock(&worker->lock);
    return NULL;
}

/**
 * @description: 初始化采集 worker 的锁、条件变量和运行参数。
 */
int media_gateway_capture_worker_init(MediaGatewayCaptureWorker *worker,
                                      V4L2CaptureCtx *capture,
                                      int retry_ms,
                                      int max_consecutive_failures) {
    if (!worker || !capture) {
        LOG_ERROR("capture worker init failed: invalid arguments");
        return -1;
    }
    memset(worker, 0, sizeof(*worker));
    worker->capture = capture;
    worker->retry_ms = (retry_ms > 0) ? retry_ms : 5;
    worker->max_consecutive_failures = (max_consecutive_failures > 0) ? max_consecutive_failures : 30;
    worker->latest_slot = -1;
    worker->next_seq = 1;
    if (pthread_mutex_init(&worker->lock, NULL) != 0) {
        LOG_ERROR("capture worker init failed: pthread_mutex_init");
        return -1;
    }
    if (pthread_cond_init(&worker->cond, NULL) != 0) {
        pthread_mutex_destroy(&worker->lock);
        LOG_ERROR("capture worker init failed: pthread_cond_init");
        return -1;
    }
    return 0;
}

/**
 * @description: 创建后台采集线程。
 */
int media_gateway_capture_worker_start(MediaGatewayCaptureWorker *worker) {
    if (!worker || !worker->capture) {
        LOG_ERROR("capture worker start failed: invalid arguments");
        return -1;
    }
    if (worker->started) {
        LOG_ERROR("capture worker start failed: already started");
        return -1;
    }

    worker->running = 1;
    if (pthread_create(&worker->thread, NULL, capture_worker_thread, worker) != 0) {
        worker->running = 0;
        LOG_ERROR("capture worker start failed: pthread_create");
        return -1;
    }
    worker->started = 1;
    LOG_INFO("capture worker started");
    return 0;
}

/**
 * @description: 主线程获取最新采集帧，成功后需要调用 release 归还槽位。
 */
int media_gateway_capture_worker_acquire_latest(MediaGatewayCaptureWorker *worker,
                                                MediaGatewayCapturedFrame *frame,
                                                int *slot_index,
                                                int timeout_ms) {
    struct timespec ts;
    int wait_ret = 0;
    int idx;

    if (!worker || !frame || !slot_index) {
        LOG_ERROR("capture worker acquire_latest failed: invalid arguments");
        return -1;
    }
    *slot_index = -1;
    memset(frame, 0, sizeof(*frame));
    capture_worker_make_abs_timeout(&ts, timeout_ms);

    pthread_mutex_lock(&worker->lock);
    while (!worker->fatal_error && worker->running &&
           (worker->latest_slot < 0 ||
            !worker->slots[worker->latest_slot].valid ||
            worker->slots[worker->latest_slot].seq == worker->consumed_seq)) {
        wait_ret = pthread_cond_timedwait(&worker->cond, &worker->lock, &ts);
        if (wait_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&worker->lock);
            return 0;
        }
    }

    if (worker->fatal_error) {
        pthread_mutex_unlock(&worker->lock);
        return -1;
    }

    if (worker->latest_slot < 0 || !worker->slots[worker->latest_slot].valid) {
        pthread_mutex_unlock(&worker->lock);
        return 0;
    }

    idx = worker->latest_slot;
    capture_worker_drop_stale_slots(worker, idx);
    worker->slots[idx].in_use = 1;
    worker->slots[idx].valid = 0;
    worker->consumed_seq = worker->slots[idx].seq;
    worker->latest_slot = -1;
    *frame = worker->slots[idx].frame;
    *slot_index = idx;
    pthread_mutex_unlock(&worker->lock);
    return 1;
}

/**
 * @description: 归还编码线程已经处理完成的槽位。
 */
void media_gateway_capture_worker_release(MediaGatewayCaptureWorker *worker, int slot_index) {
    if (!worker || slot_index < 0 || slot_index >= MEDIA_GATEWAY_CAPTURE_WORKER_SLOTS) return;
    pthread_mutex_lock(&worker->lock);
    worker->slots[slot_index].in_use = 0;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->lock);
}

/**
 * @description: 请求采集线程退出，并等待线程结束。
 */
void media_gateway_capture_worker_stop(MediaGatewayCaptureWorker *worker) {
    if (!worker) return;
    pthread_mutex_lock(&worker->lock);
    worker->running = 0;
    pthread_cond_broadcast(&worker->cond);
    pthread_mutex_unlock(&worker->lock);

    if (worker->started) {
        pthread_join(worker->thread, NULL);
        worker->started = 0;
        LOG_INFO("capture worker stopped");
    }
}

/**
 * @description: 停止采集 worker 并释放所有槽位缓存。
 */
void media_gateway_capture_worker_deinit(MediaGatewayCaptureWorker *worker) {
    int i;
    if (!worker) return;
    media_gateway_capture_worker_stop(worker);
    for (i = 0; i < MEDIA_GATEWAY_CAPTURE_WORKER_SLOTS; ++i) {
        free(worker->slots[i].data);
        worker->slots[i].data = NULL;
        worker->slots[i].capacity = 0;
    }
    pthread_cond_destroy(&worker->cond);
    pthread_mutex_destroy(&worker->lock);
    memset(worker, 0, sizeof(*worker));
}
