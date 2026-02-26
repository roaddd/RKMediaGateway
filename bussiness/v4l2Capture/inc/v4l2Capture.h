#ifndef __V4L2CAPTURE_H__
#define __V4L2CAPTURE_H__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

// ==================== 配置参数 ====================
#define CAM_DEV_PATH     "/dev/video0"   // 摄像头设备节点
#define CAPTURE_WIDTH    1920            // 采集宽度（建议和摄像头支持的分辨率一致）
#define CAPTURE_HEIGHT   1080           // 采集高度
#define CAPTURE_FORMAT   V4L2_PIX_FMT_NV12 // 采集格式（RKMPP最适配NV12/YUV420SP）
#define SAVE_FRAME_COUNT 300             // 采集300帧（10秒，30fps）
#define OUTPUT_FILE      "capture_nv12.yuv" // 保存的原始数据文件

// ==================== V4L2采集上下文 ====================

#define VIDEO_MAX_PLANES    1

typedef struct {
    int fd;                     // 摄像头设备文件描述符
    void *buf[4];               // mmap映射的缓冲区指针（最多4个）
    int buf_len[4];             // 每个缓冲区的长度
    int buf_count;              // 实际申请的缓冲区数量
} V4L2CaptureCtx;

#ifdef __cplusplus
extern "C"{
#endif

int v4l2_capture_init(V4L2CaptureCtx *ctx);

void v4l2_capture_deinit(V4L2CaptureCtx *ctx);

int v4l2_capture_frame(V4L2CaptureCtx *ctx, uint8_t **frame_data, int *frame_len);

#ifdef __cplusplus
}
#endif

#endif
