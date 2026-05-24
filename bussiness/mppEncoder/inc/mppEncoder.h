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

typedef struct {
    uint64_t encoder_input_buffer_copy_us; /* NV12 copy into MPP input buffer. */
    uint64_t encoder_submit_frame_call_us; /* encode_put_frame call duration. */
    uint64_t encoder_poll_packet_call_us;  /* encode_get_packet call duration. */
    uint64_t encoder_packet_copy_us;       /* encoded packet copy duration. */
    uint64_t encode_frame_total_us;        /* Whole mpp_encoder_encode_frame duration. */
} MppEncoderTiming;

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
                             uint64_t *encode_start_ts_us,
                             uint64_t *encode_done_ts_us,
                             MppEncoderTiming *timing);

/*
 * 使用外部 DMA-BUF 作为一帧 NV12 输入，避免把采集帧再拷贝到 MPP 输入 buffer。
 * dmabuf_fd/dmabuf_size: V4L2 等上游模块导出的 DMA-BUF fd 及其完整 buffer 容量。
 * 外部 buffer 的 NV12 排布必须匹配 enc 中配置的 format/hor_stride/ver_stride；
 * 若上游给的是紧凑 NV12 或 stride 不一致，应先转换布局或回退 copy 编码接口。
 * 其余输出参数语义与 mpp_encoder_encode_frame 一致。
 */
int mpp_encoder_encode_dmabuf(MppEncoderCtx *enc,
                              int dmabuf_fd,
                              size_t dmabuf_size,
                              uint64_t frame_id,
                              uint8_t **h264_data,
                              size_t *h264_len,
                              int *is_key_frame,
                              uint64_t *encode_start_ts_us,
                              uint64_t *encode_done_ts_us,
                              MppEncoderTiming *timing);

/*
 * 请求编码器将下一帧编码为 IDR。
 * 常用于会话刚建立时快速给下游提供可解码起点。
 */
int mpp_encoder_request_idr(MppEncoderCtx *enc);

#ifdef __cplusplus
}
#endif

#endif
