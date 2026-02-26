#ifndef __MPP_ENCODER_H__
#define __MPP_ENCODER_H__

#include <stddef.h>
#include <stdint.h>

#include "rk_mpi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppEncCfg cfg;

    MppBufferGroup frame_group;
    MppBuffer frame_buffer;
    MppFrame frame;

    int width;
    int height;
    int hor_stride;
    int ver_stride;
    int fps;
    int bitrate;
    int gop;
    int64_t pts;

    uint8_t *packet_cache;
    size_t packet_cache_size;
} MppEncoderCtx;

// 初始化 RK MPP H264 编码器
// width/height: 输入图像分辨率（与采集一致）
// fps: 目标帧率
// bitrate: 目标码率（bit/s）
// gop: I 帧间隔（通常为 fps 的 1~2 倍）
int mpp_encoder_init(MppEncoderCtx *enc, int width, int height, int fps, int bitrate, int gop);
void mpp_encoder_deinit(MppEncoderCtx *enc);

// 编码一帧 NV12 数据，输出 Annex-B H264 码流
// nv12_data/nv12_len: 输入原始帧
// h264_data/h264_len: 输出码流指针和长度（内存由编码器内部管理，下次调用可能被覆盖）
// is_key_frame: 是否关键帧（可选，传 NULL 则不关心）
int mpp_encoder_encode_frame(MppEncoderCtx *enc,
                             const uint8_t *nv12_data,
                             size_t nv12_len,
                             uint8_t **h264_data,
                             size_t *h264_len,
                             int *is_key_frame);

#ifdef __cplusplus
}
#endif

#endif
