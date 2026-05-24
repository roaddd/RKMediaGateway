#ifndef __MEDIA_FRAME_SOURCE_H__
#define __MEDIA_FRAME_SOURCE_H__

#include <pthread.h>
#include <stddef.h>

#include "v4l2Capture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_FRAME_SOURCE_SLOTS 2

typedef struct {
    uint64_t camera_buffer_wait_us;          /* 摄像头/驱动缓冲等待到 DQBUF 返回的时间。 */
    uint64_t dqbuf_ioctl_duration_us;        /* VIDIOC_DQBUF ioctl 调用耗时。 */
    uint64_t mmap_to_frame_cache_copy_us;    /* mmap buffer 拷贝到 frame_cache 的耗时，零拷贝路径为 0。 */
    uint64_t capture_call_duration_us;       /* V4L2 取帧接口整体调用耗时。 */
    uint64_t frame_source_publish_us;        /* 采集线程发布到 MediaFrameSource 槽位的总耗时。 */
    uint64_t frame_source_publish_copy_us;   /* frame source 发布时的整帧拷贝耗时，零拷贝路径为 0。 */
    uint64_t video_input_publish_copy_us;    /* 发布到视频编码输入槽时的整帧拷贝耗时，零拷贝路径为 0。 */
    uint64_t video_input_acquire_copy_us;    /* 编码线程从视频输入槽取本地副本时的整帧拷贝耗时，零拷贝路径为 0。 */
} MediaFrameMetrics;

typedef struct {
    uint8_t *raw_frame;             /* 当前采集到的 NV12 mmap 地址，生命周期跟随 capture_buffer。 */
    int raw_len;                    /* 当前 NV12 帧有效数据长度。 */
    V4L2CaptureBufferRef *capture_buffer; /* 持有 V4L2 buffer，引用归零后才允许驱动复用。 */
    uint64_t frame_id;              /* 当前采集帧号。 */
    uint64_t dqbuf_ts_us;           /* VIDIOC_DQBUF 返回后的单调时钟时间。 */
    MediaFrameMetrics metrics;      /* 调试用路径耗时指标，不影响帧核心语义。 */
} MediaFrame;

typedef struct {
    MediaFrame frame;     /* 槽位持有的一帧引用，不再保存整帧 NV12 副本。 */
    uint64_t seq;         /* 槽位帧序号，用于区分新旧帧。 */
    int valid;            /* 槽位是否保存了还没被编码线程取走的新帧。 */
    int in_use;           /* 调度线程是否正在使用该槽位。 */
} MediaFrameSourceSlot;

typedef struct {
    V4L2CaptureCtx *capture;        /* 外部传入的 V4L2 采集上下文，生命周期由 mediaGateway 管理。 */
    char thread_name[16];           /* Linux 线程名，包含结尾 '\0'。 */
    pthread_t thread;               /* 采集线程句柄。 */
    pthread_mutex_t lock;           /* 保护槽位、运行状态和统计字段。 */
    pthread_cond_t cond;            /* 新帧到达或线程退出时唤醒消费者。 */

    /* 双槽最新帧缓存：槽位只保存采集 buffer 引用，编码慢时允许丢弃旧帧。 */
    /* latest-frame source，不是 FIFO 队列 */
    MediaFrameSourceSlot slots[MEDIA_FRAME_SOURCE_SLOTS];
    int latest_slot;                /* 当前最新可消费槽位下标，-1 表示暂无新帧。 */
    uint64_t next_seq;              /* 下一帧发布序号。 */
    uint64_t consumed_seq;          /* 调度线程最近消费的帧序号。 */
    uint64_t dropped_frames;        /* 因下游消费不及时而丢弃的旧帧数。 */
    int retry_ms;                   /* 采集失败后的短暂退避时间。 */
    int max_consecutive_failures;   /* 连续采集失败阈值，达到后 frame source 进入 fatal 状态。 */
    int consecutive_failures;       /* 当前连续采集失败次数。 */
    int running;                    /* frame source 是否应继续运行。 */
    int started;                    /* 采集线程是否已成功启动。 */
    int fatal_error;                /* 采集线程是否遇到不可恢复错误。 */
} MediaFrameSource;

/* MediaFrame 引用辅助函数：copy_ref 会增加 capture_buffer 引用，reset 会释放引用。 */
void media_frame_init(MediaFrame *frame);
void media_frame_copy_ref(MediaFrame *dst, const MediaFrame *src);
void media_frame_reset(MediaFrame *frame);

/**
 * @description: 初始化帧源，但不启动采集线程。
 * @param {MediaFrameSource *} source 帧源对象。
 * @param {V4L2CaptureCtx *} capture 已初始化的 V4L2 采集上下文。
 * @param {const char *} thread_name 采集线程名来源。
 * @param {int} retry_ms 采集失败后的退避时间，单位毫秒。
 * @param {int} max_consecutive_failures 连续采集失败阈值。
 * @return {int} 0 成功，-1 失败。
 */
int media_frame_source_init(MediaFrameSource *source,
                            V4L2CaptureCtx *capture,
                            const char *thread_name,
                            int retry_ms,
                            int max_consecutive_failures);
/**
 * @description: 启动后台采集线程。
 * @param {MediaFrameSource *} source 帧源对象。
 * @return {int} 0 成功，-1 失败。
 */
int media_frame_source_start(MediaFrameSource *source);
/**
 * @description: 获取最新采集帧。若暂无新帧，会最多等待 timeout_ms 毫秒。
 * @param {MediaFrameSource *} source 帧源对象。
 * @param {MediaFrame *} frame 输出最新帧引用，调用方处理完后必须 media_frame_reset。
 * @param {int *} slot_index 输出槽位下标，调用方处理完后必须 release。
 * @param {int} timeout_ms 等待超时时间，单位毫秒。
 * @return {int} 1 获取到新帧；0 超时暂无新帧；-1 帧源发生不可恢复错误或参数非法。
 */
int media_frame_source_acquire_latest(MediaFrameSource *source,
                                      MediaFrame *frame,
                                      int *slot_index,
                                      int timeout_ms);
/**
 * @description: 释放 acquire 得到的槽位，允许帧源复用槽位并释放槽位持有的 buffer 引用。
 * @param {MediaFrameSource *} source 帧源对象。
 * @param {int} slot_index 待释放槽位下标。
 * @return {void}
 */
void media_frame_source_release(MediaFrameSource *source, int slot_index);
/**
 * @description: 停止采集线程并等待线程退出。
 * @param {MediaFrameSource *} source 帧源对象。
 * @return {void}
 */
void media_frame_source_stop(MediaFrameSource *source);
/**
 * @description: 释放帧源内部槽位引用和同步资源。
 * @param {MediaFrameSource *} source 帧源对象。
 * @return {void}
 */
void media_frame_source_deinit(MediaFrameSource *source);

#ifdef __cplusplus
}
#endif

#endif
