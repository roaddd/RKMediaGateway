/**
 * @file mediaFrameSource.h
 * @brief 视频采集线程、最新帧缓存及采集控制命令接口。
 *
 * 每个 MediaFrameSource 拥有独立命令队列，控制命令由采集线程执行，
 * 执行结果发送到外部提供的统一结果队列。
 */

#ifndef __MEDIA_FRAME_SOURCE_H__
#define __MEDIA_FRAME_SOURCE_H__

#include <pthread.h>
#include <stddef.h>

#include "v4l2Capture.h"
#include "threadMessageQueue.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 帧源环形槽位数。
 *
 * 从 2 提升到 4，在高运动场景下为编码线程提供更多缓冲：
 * - 2 槽时，采集线程只能保留 1 帧最新帧 + 1 帧正在被编码线程消费；
 *   编码耗时抖动时新帧直接丢弃，导致卡顿。
 * - 4 槽可容忍编码线程短时滞后，降低丢帧率。
 * 每个槽位只保存 V4L2 buffer 引用，内存开销极小。
 */
#define MEDIA_FRAME_SOURCE_SLOTS 4

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
    int in_use;           /* 视频编码线程是否正在取得该槽位。 */
} MediaFrameSourceSlot;

typedef struct {
    V4L2CaptureCtx *capture;      /* 外部传入的 V4L2 采集上下文。 */
    char thread_name[16];         /* Linux 线程名，包含结尾 '\0'。 */
    int source_index;             /* 当前采集源下标。 */
    int retry_ms;                 /* 采集失败后的短暂退避时间。 */
    int max_consecutive_failures; /* 连续采集失败阈值。 */
} MediaFrameSourceConfig;

typedef struct {
    pthread_t thread;      /* 采集线程句柄。 */
    pthread_mutex_t lock;  /* 保护跨线程共享的槽位和生命周期状态。 */
    int frame_event_fd;    /* 新帧、停止或致命错误通知 fd，由视频编码线程通过 poll 等待。 */

    /* latest-frame 缓存：只保存采集 buffer 引用，消费滞后时丢弃旧帧。 */
    MediaFrameSourceSlot slots[MEDIA_FRAME_SOURCE_SLOTS];
    int latest_slot;              /* 当前最新可消费槽位下标，-1 表示暂无新帧。 */
    uint64_t next_seq;            /* 下一帧发布序号。 */
    uint64_t consumed_seq;        /* 视频编码线程最近消费的帧序号。 */
    uint64_t dropped_frames;      /* 因下游消费不及时而丢弃的旧帧数。 */
    int consecutive_failures;     /* 当前连续采集失败次数。 */
    int running;                  /* frame source 是否应继续运行。 */
    int started;                  /* 采集线程是否已成功启动。 */
    int fatal_error;              /* 采集线程是否遇到不可恢复错误。 */
} MediaFrameSourceRuntime;

typedef struct {
    ThreadMessageQueue command_queue; /* 仅由当前采集线程消费的控制命令队列。 */
    ThreadMessageQueue *result_queue; /* 工作结果统一回传到 gateway 主循环。 */
} MediaFrameSourceMessaging;

typedef struct {
    MediaFrameSourceConfig config;       /* 初始化后保持稳定的采集配置。 */
    MediaFrameSourceRuntime runtime;     /* 线程、帧槽和运行期统计状态。 */
    MediaFrameSourceMessaging messaging; /* 控制命令和执行结果通道。 */
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
 * @param {int} source_index 当前采集源下标。
 * @param {ThreadMessageQueue *} result_queue worker 共用的结果回传队列。
 * @return {int} 0 成功，-1 失败。
 */
int media_frame_source_init(MediaFrameSource *source,
                            V4L2CaptureCtx *capture,
                            const char *thread_name,
                            int retry_ms,
                            int max_consecutive_failures,
                            int source_index,
                            ThreadMessageQueue *result_queue);

/**
 * @description: 非阻塞投递一条采集线程控制命令。
 * @return 0 成功，其余值表示队列错误。
 */
int media_frame_source_submit_command(MediaFrameSource *source,
                                      const ThreadMessage *command);
/**
 * @description: 启动后台采集线程。
 * @param {MediaFrameSource *} source 帧源对象。
 * @return {int} 0 成功，-1 失败。
 */
int media_frame_source_start(MediaFrameSource *source);
/**
 * @description: 非阻塞获取最新采集帧，由调用方在 frame event fd 可读后调用。
 * @param {MediaFrameSource *} source 帧源对象。
 * @param {MediaFrame *} frame 输出最新帧引用，调用方处理完后必须 media_frame_reset。
 * @param {int *} slot_index 输出槽位下标，调用方处理完后必须 release。
 * @return {int} 1 获取到新帧；0 当前暂无新帧；-1 帧源发生不可恢复错误或参数非法。
 */
int media_frame_source_try_acquire_latest(MediaFrameSource *source,
                                          MediaFrame *frame,
                                          int *slot_index);

/**
 * @description: 获取用于等待新帧、停止和致命错误事件的 eventfd。
 * @return eventfd 文件描述符，失败返回 -1。
 */
int media_frame_source_get_event_fd(const MediaFrameSource *source);

/**
 * @description: 清空 frame eventfd 中已经累积的通知计数。
 * @return 0 成功，-1 失败。
 */
int media_frame_source_drain_event(MediaFrameSource *source);
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
