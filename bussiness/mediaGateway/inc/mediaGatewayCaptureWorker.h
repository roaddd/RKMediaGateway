#ifndef __MEDIA_GATEWAY_CAPTURE_WORKER_H__
#define __MEDIA_GATEWAY_CAPTURE_WORKER_H__

#include <pthread.h>
#include <stddef.h>

#include "mediaGateway.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_GATEWAY_CAPTURE_WORKER_SLOTS 3

typedef struct {
    uint8_t *data;                  /* 槽位内保存的一帧 NV12 数据副本。 */
    size_t capacity;                /* 当前槽位已分配容量。 */
    MediaGatewayCapturedFrame frame; /* 与 data 对应的一帧采集元信息。 */
    uint64_t seq;                   /* 槽位帧序号，用于区分新旧帧。 */
    int valid;                      /* 槽位是否保存了尚未消费的新帧。 */
    int in_use;                     /* 编码线程是否正在使用该槽位。 */
} MediaGatewayCaptureSlot;

typedef struct {
    V4L2CaptureCtx *capture;        /* 外部传入的 V4L2 采集上下文，生命周期由 mediaGateway 管理。 */
    pthread_t thread;               /* 采集线程句柄。 */
    pthread_mutex_t lock;           /* 保护槽位、运行状态和统计字段。 */
    pthread_cond_t cond;            /* 新帧到达或线程退出时唤醒消费者。 */

    MediaGatewayCaptureSlot slots[MEDIA_GATEWAY_CAPTURE_WORKER_SLOTS]; /* 最新帧三槽缓冲。 */
    int latest_slot;                /* 当前最新可消费槽位下标，-1 表示暂无新帧。 */
    uint64_t next_seq;              /* 下一帧发布序号。 */
    uint64_t consumed_seq;          /* 编码线程最近消费的帧序号。 */
    uint64_t dropped_frames;        /* 因编码线程消费不及时而丢弃的旧帧数。 */

    int retry_ms;                   /* 采集失败后的短暂退避时间。 */
    int max_consecutive_failures;   /* 连续采集失败阈值，达到后 worker 进入 fatal 状态。 */
    int consecutive_failures;       /* 当前连续采集失败次数。 */
    int running;                    /* worker 是否应继续运行。 */
    int started;                    /* 采集线程是否已成功启动。 */
    int fatal_error;                /* 采集线程是否遇到不可恢复错误。 */
} MediaGatewayCaptureWorker;

/**
 * @description: 初始化采集 worker，但不启动线程。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @param {V4L2CaptureCtx *} capture 已初始化的 V4L2 采集上下文。
 * @param {int} retry_ms 采集失败后的退避时间，单位毫秒。
 * @param {int} max_consecutive_failures 连续采集失败阈值。
 * @return {int} 0 成功，-1 失败。
 */
int media_gateway_capture_worker_init(MediaGatewayCaptureWorker *worker,
                                      V4L2CaptureCtx *capture,
                                      int retry_ms,
                                      int max_consecutive_failures);

/**
 * @description: 启动采集线程。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @return {int} 0 成功，-1 失败。
 */
int media_gateway_capture_worker_start(MediaGatewayCaptureWorker *worker);

/**
 * @description: 获取最新采集帧。若暂无新帧，会最多等待 timeout_ms 毫秒。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @param {MediaGatewayCapturedFrame *} frame 输出最新帧元信息，raw_frame 指向 worker 槽位数据。
 * @param {int *} slot_index 输出槽位下标，调用方处理完后必须 release。
 * @param {int} timeout_ms 等待超时时间，单位毫秒。
 * @return {int} 1 获取到新帧；0 超时暂无新帧；-1 worker 发生不可恢复错误或参数非法。
 */
int media_gateway_capture_worker_acquire_latest(MediaGatewayCaptureWorker *worker,
                                                MediaGatewayCapturedFrame *frame,
                                                int *slot_index,
                                                int timeout_ms);

/**
 * @description: 释放 acquire 得到的槽位，允许采集线程复用该缓冲。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @param {int} slot_index 待释放槽位下标。
 * @return {void}
 */
void media_gateway_capture_worker_release(MediaGatewayCaptureWorker *worker, int slot_index);

/**
 * @description: 停止采集线程并等待线程退出。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @return {void}
 */
void media_gateway_capture_worker_stop(MediaGatewayCaptureWorker *worker);

/**
 * @description: 释放采集 worker 内部分配的槽位资源。
 * @param {MediaGatewayCaptureWorker *} worker 采集 worker。
 * @return {void}
 */
void media_gateway_capture_worker_deinit(MediaGatewayCaptureWorker *worker);

#ifdef __cplusplus
}
#endif

#endif
