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

#define VIDEO_MAX_PLANES 1

typedef struct {
    int fd;
    void *buf[4];
    int buf_len[4];
    int buf_count;
    uint8_t *frame_cache;  // 复制一份出队帧，避免 QBUF 后底层缓冲被驱动复用
    int frame_cache_len;   // frame_cache 当前容量，按实际 bytesused 动态扩容
} V4L2CaptureCtx;

#ifdef __cplusplus
extern "C" {
#endif

int v4l2_capture_init(V4L2CaptureCtx *ctx);
void v4l2_capture_deinit(V4L2CaptureCtx *ctx);
int v4l2_capture_frame(V4L2CaptureCtx *ctx, uint8_t **frame_data, int *frame_len);

#ifdef __cplusplus
}
#endif

#endif
