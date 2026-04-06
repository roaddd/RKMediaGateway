#ifndef __MPP_ENCODER_H__
#define __MPP_ENCODER_H__

#include <stddef.h>
#include <stdint.h>

#include "rk_mpi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    MppCtx ctx;                 /* MPP 编码上下文句柄。 */
    MppApi *mpi;                /* MPP 提供的接口函数表。 */
    MppEncCfg cfg;              /* 编码配置对象。 */

    MppBufferGroup frame_group; /* 输入帧缓冲组。 */
    MppBuffer frame_buffer;     /* 当前输入帧对应的 MPP Buffer。 */
    MppFrame frame;             /* 当前送入编码器的帧对象。 */

    int width;                  /* 输入图像宽度。 */
    int height;                 /* 输入图像高度。 */
    int hor_stride;             /* 水平 stride。 */
    int ver_stride;             /* 垂直 stride。 */
    int fps;                    /* 编码帧率。 */
    int bitrate;                /* 目标码率。 */
    int gop;                    /* GOP 长度。 */
    int64_t pts;                /* 送入 MPP 的时间戳计数。 */

    uint8_t *packet_cache;      /* 编码输出缓存，保存导出的 Annex-B 码流。 */
    size_t packet_cache_size;   /* packet_cache 当前容量。 */
} MppEncoderCtx;

typedef struct {
    int rc_mode;        /* MPP_ENC_RC_MODE_*；<=0 表示使用默认 CBR。 */
    int h264_profile;   /* H264 profile，例如 66/77/100；<=0 表示默认 100。 */
    int h264_level;     /* H264 level，例如 40；<=0 表示默认 40。 */
    int h264_cabac_en;  /* 是否启用 CABAC；<0 表示默认 1。 */
    int qp_init;        /* 初始 QP；<=0 表示使用 MPP 默认值。 */
    int qp_min;         /* P/B 帧最小 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max;         /* P/B 帧最大 QP；<=0 表示使用 MPP 默认值。 */
    int qp_min_i;       /* I 帧最小 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max_i;       /* I 帧最大 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max_step;    /* 相邻帧最大 QP 变化步长；<=0 表示使用 MPP 默认值。 */
} MppEncoderOptions;

int mpp_encoder_init(MppEncoderCtx *enc,
                     int width,
                     int height,
                     int fps,
                     int bitrate,
                     int gop,
                     const MppEncoderOptions *options);
void mpp_encoder_deinit(MppEncoderCtx *enc);

/*
 * 编码一帧 NV12 数据，输出 Annex-B H264 码流。
 * nv12_data/nv12_len: 输入原始帧数据。
 * h264_data/h264_len: 输出码流指针和长度（内存由编码器内部管理，下次调用可能被覆盖）。
 * is_key_frame: 是否关键帧（可选，传 NULL 表示不关心）。
 */
int mpp_encoder_encode_frame(MppEncoderCtx *enc,
                             const uint8_t *nv12_data,
                             size_t nv12_len,
                             uint64_t frame_id,
                             uint8_t **h264_data,
                             size_t *h264_len,
                             int *is_key_frame,
                             uint64_t *encode_put_ts_us,
                             uint64_t *encode_get_ts_us);

#ifdef __cplusplus
}
#endif

#endif
