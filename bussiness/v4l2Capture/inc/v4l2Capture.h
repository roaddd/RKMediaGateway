#ifndef __V4L2CAPTURE_H__
#define __V4L2CAPTURE_H__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

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

typedef struct {
    int fd;                 /* 摄像头设备文件描述符。 */
    void *buf[V4L2_CAPTURE_BUFFER_COUNT]; /* 驱动 mmap 出来的采集缓冲区地址。 */
    int buf_len[V4L2_CAPTURE_BUFFER_COUNT]; /* 每个采集缓冲区的长度。 */
    int buf_count;          /* 实际申请到的驱动缓冲区数量。 */
    int width;              /* 实际采集宽度。 */
    int height;             /* 实际采集高度。 */
    uint32_t pixelformat;   /* 实际采集像素格式。 */
    uint64_t frame_id;      /* 已采集帧计数。 */
    uint8_t *frame_cache;   /* 拷贝后的稳定用户态帧缓存，供调用方读取。 */
    int frame_cache_len;    /* frame_cache 当前可用容量。 */
} V4L2CaptureCtx;

#ifdef __cplusplus
extern "C" {
#endif

int v4l2_capture_init(V4L2CaptureCtx *ctx);
int v4l2_capture_init_with_config(V4L2CaptureCtx *ctx, const V4L2CaptureConfig *config);
void v4l2_capture_deinit(V4L2CaptureCtx *ctx);
int v4l2_capture_frame(V4L2CaptureCtx *ctx,
                       uint8_t **frame_data,
                       int *frame_len,
                       uint64_t *frame_id,
                       uint64_t *dqbuf_ts_us,
                       uint64_t *driver_to_dqbuf_us,
                       uint64_t *dqbuf_ioctl_us,
                       uint64_t *frame_copy_us);

#ifdef __cplusplus
}
#endif

#endif
