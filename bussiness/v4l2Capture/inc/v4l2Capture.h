#ifndef __V4L2CAPTURE_H__
#define __V4L2CAPTURE_H__

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// Capture defaults
#define CAM_DEV_PATH "/dev/video0"
#define CAPTURE_WIDTH 1920
#define CAPTURE_HEIGHT 1080
#define CAPTURE_FORMAT V4L2_PIX_FMT_NV12
#define SAVE_FRAME_COUNT 300
#define OUTPUT_FILE "capture_nv12.yuv"

#define V4L2_CAPTURE_BUFFER_COUNT 4
#define V4L2_CAPTURE_MAX_PLANES 1

typedef struct {
    const char *device_path; /* V4L2 设备节点，例如 /dev/video0。 */
    int width;              /* 采集宽度。 */
    int height;             /* 采集高度。 */
    uint32_t pixelformat;   /* V4L2 像素格式，例如 V4L2_PIX_FMT_NV12。 */
    int buffer_count;       /* mmap buffer 数量，<=0 使用默认值。 */
} V4L2CaptureConfig;

typedef struct V4L2CaptureCtx V4L2CaptureCtx;

typedef struct {
    V4L2CaptureCtx *capture; /* 所属 V4L2 采集上下文。 */
    int index;               /* 对应驱动 buffer 下标。 */
    int refs;                /* 仍持有该 buffer 的上层引用数，归零后 QBUF 回驱动。 */
} V4L2CaptureBufferRef;

typedef struct {
    uint32_t bytesperline;   /* 该 plane 协商后的单行步进。 */
    uint32_t sizeimage;      /* 该 plane 协商后的完整 buffer 容量。 */
} V4L2CapturePlaneLayout;

typedef struct {
    int width;               /* 协商后的采集宽度。 */
    int height;              /* 协商后的采集高度。 */
    uint32_t pixelformat;    /* 协商后的 V4L2 像素格式。 */
    uint32_t num_planes;     /* 协商后的 plane 数量。 */
    uint32_t field;          /* 协商后的 V4L2 field 类型，驱动可能改写请求值。 */
    V4L2CapturePlaneLayout planes[VIDEO_MAX_PLANES]; /* 各 plane 的协商布局。 */
} V4L2CaptureFormat;

struct V4L2CaptureCtx {
    char device_path[PATH_MAX];        /* 当前采集 video node，例如 /dev/video0。 */
    char sensor_subdev_path[PATH_MAX]; /* 绑定的 sensor subdev，例如 /dev/v4l-subdev3。 */
    int fd;                 /* 摄像头设备文件描述符。 */
    void *buf[V4L2_CAPTURE_BUFFER_COUNT]; /* 驱动 mmap 出来的采集缓冲区地址。 */
    int buf_len[V4L2_CAPTURE_BUFFER_COUNT]; /* 每个采集缓冲区的长度。 */
    int buf_dmabuf_fd[V4L2_CAPTURE_BUFFER_COUNT]; /* 由 MMAP buffer 导出的 DMA-BUF fd。 */
    V4L2CaptureBufferRef buffer_refs[V4L2_CAPTURE_BUFFER_COUNT]; /* 零拷贝路径的采集 buffer 引用状态。 */
    pthread_mutex_t buffer_lock; /* 保护 buffer_refs 和归还 QBUF 的引用计数锁。 */
    int buffer_lock_ready; /* buffer_lock 是否初始化成功。 */
    int buf_count;          /* 实际申请到的驱动缓冲区数量。 */
    V4L2CaptureFormat format; /* V4L2 S_FMT 协商后的输入格式和 plane 布局。 */
    uint64_t frame_id;      /* 已采集帧计数。 */

    uint8_t *frame_cache;   /* copy API 的稳定用户态帧缓存，供调用方读取。 */
    int frame_cache_len;    /* frame_cache 当前可用容量。 */
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @description: 使用默认设备、分辨率、格式和 buffer 数量初始化 V4L2 采集模块。
 * @param {V4L2CaptureCtx *} ctx 待初始化的采集上下文。
 * @return {int} 0 成功，-1 失败。
 */
int v4l2_capture_init(V4L2CaptureCtx *ctx);

/**
 * @description: 按指定配置初始化 V4L2 采集模块并启动 streaming。
 * @param {V4L2CaptureCtx *} ctx 待初始化的采集上下文。
 * @param {const V4L2CaptureConfig *} config 采集配置；部分字段无效时使用默认值。
 * @return {int} 0 成功，-1 失败。
 */
int v4l2_capture_init_with_config(V4L2CaptureCtx *ctx, const V4L2CaptureConfig *config);

/**
 * @description: 停止 V4L2 streaming 并释放 mmap、DMA-BUF fd、frame_cache 等资源。
 * @param {V4L2CaptureCtx *} ctx 待释放的采集上下文。
 * @return {void}
 */
void v4l2_capture_deinit(V4L2CaptureCtx *ctx);

int v4l2_capture_set_fps(V4L2CaptureCtx *ctx, int fps);

/**
 * @description: 采集一帧并拷贝到内部 frame_cache，返回稳定的用户态帧指针。
 * @param {V4L2CaptureCtx *} ctx 采集上下文。
 * @param {uint8_t **} frame_data 输出帧数据，指向内部 frame_cache。
 * @param {int *} frame_len 输出帧有效长度。
 * @param {uint64_t *} frame_id 输出递增采集帧号。
 * @param {uint64_t *} dqbuf_ts_us VIDIOC_DQBUF 返回后的单调时钟时间。
 * @param {uint64_t *} camera_buffer_wait_us 驱动时间戳到 DQBUF 返回的等待时间。
 * @param {uint64_t *} dqbuf_ioctl_duration_us VIDIOC_DQBUF ioctl 调用耗时。
 * @param {uint64_t *} mmap_to_frame_cache_copy_us mmap buffer 拷贝到 frame_cache 的耗时。
 * @return {int} 0 成功，-1 失败。
 */
int v4l2_capture_frame(V4L2CaptureCtx *ctx,
                       uint8_t **frame_data,
                       int *frame_len,
                       uint64_t *frame_id,
                       uint64_t *dqbuf_ts_us,
                       uint64_t *camera_buffer_wait_us,
                       uint64_t *dqbuf_ioctl_duration_us,
                       uint64_t *mmap_to_frame_cache_copy_us);

/**
 * @description: 采集一帧并返回驱动 buffer 引用，供 DMA-BUF 零拷贝链路继续传递。
 * @param {V4L2CaptureCtx *} ctx 采集上下文。
 * @param {uint8_t **} frame_data 输出当前 mmap buffer 地址，仅在 buffer_ref 有效期内可用。
 * @param {int *} frame_len 输出帧有效长度。
 * @param {uint64_t *} frame_id 输出递增采集帧号。
 * @param {uint64_t *} dqbuf_ts_us VIDIOC_DQBUF 返回后的单调时钟时间。
 * @param {uint64_t *} camera_buffer_wait_us 驱动时间戳到 DQBUF 返回的等待时间。
 * @param {uint64_t *} dqbuf_ioctl_duration_us VIDIOC_DQBUF ioctl 调用耗时。
 * @param {V4L2CaptureBufferRef **} buffer_ref 输出采集 buffer 引用，调用方最终必须 unref。
 * @return {int} 0 成功，-1 失败。
 */
int v4l2_capture_acquire_frame(V4L2CaptureCtx *ctx,
                               uint8_t **frame_data,
                               int *frame_len,
                               uint64_t *frame_id,
                               uint64_t *dqbuf_ts_us,
                               uint64_t *camera_buffer_wait_us,
                               uint64_t *dqbuf_ioctl_duration_us,
                               V4L2CaptureBufferRef **buffer_ref);
/**
 * @description: 增加一份采集 buffer 引用，延后该 buffer 回到 V4L2 驱动的时机。
 * @param {V4L2CaptureBufferRef *} buffer_ref 待增加引用的采集 buffer。
 * @return {void}
 */
void v4l2_capture_buffer_ref(V4L2CaptureBufferRef *buffer_ref);

/**
 * @description: 释放一份采集 buffer 引用；最后一次释放会将 buffer QBUF 回 V4L2 驱动。
 * @param {V4L2CaptureBufferRef *} buffer_ref 待释放引用的采集 buffer。
 * @return {void}
 */
void v4l2_capture_buffer_unref(V4L2CaptureBufferRef *buffer_ref);

/**
 * @description: 查询采集 buffer 导出的 DMA-BUF fd。
 * @param {const V4L2CaptureBufferRef *} buffer_ref 采集 buffer 引用。
 * @return {int} 有效 DMA-BUF fd；-1 表示引用无效或导出失败。
 */
int v4l2_capture_buffer_dmabuf_fd(const V4L2CaptureBufferRef *buffer_ref);

/**
 * @description: 查询采集 buffer 的完整容量。
 * @param {const V4L2CaptureBufferRef *} buffer_ref 采集 buffer 引用。
 * @return {size_t} buffer 容量；引用无效时返回 0。
 */
size_t v4l2_capture_buffer_size(const V4L2CaptureBufferRef *buffer_ref);

#ifdef __cplusplus
}
#endif

#endif
