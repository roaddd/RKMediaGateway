#ifndef __MPP_ENCODER_H__
#define __MPP_ENCODER_H__

#include <stddef.h>
#include <stdint.h>

#include "rk_mpi.h"
#include "rk_venc_rc.h"
#include "mediaControlMessage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MPP_ENCODER_RC_MODE_VBR = MPP_ENC_RC_MODE_VBR,       /* 变码率。 */
    MPP_ENCODER_RC_MODE_CBR = MPP_ENC_RC_MODE_CBR,       /* 定码率。 */
    MPP_ENCODER_RC_MODE_FIXQP = MPP_ENC_RC_MODE_FIXQP,   /* 固定 QP。 */
    MPP_ENCODER_RC_MODE_AVBR = MPP_ENC_RC_MODE_AVBR,     /* 平均码率控制。 */
    MPP_ENCODER_RC_MODE_BUTT = MPP_ENC_RC_MODE_BUTT      /* 无效边界值。 */
} MppEncoderRcMode;

typedef enum {
    MPP_ENCODER_H264_PROFILE_DEFAULT = 0,                /* 使用编码器默认 High profile。 */
    MPP_ENCODER_H264_PROFILE_BASELINE = 66,              /* H.264 Baseline profile。 */
    MPP_ENCODER_H264_PROFILE_MAIN = 77,                  /* H.264 Main profile。 */
    MPP_ENCODER_H264_PROFILE_HIGH = 100                  /* H.264 High profile。 */
} MppEncoderH264Profile;

typedef enum {
    MPP_ENCODER_H264_LEVEL_DEFAULT = 0,                  /* 使用编码器默认 level 4.0。 */
    MPP_ENCODER_H264_LEVEL_30 = 30,                      /* H.264 level 3.0。 */
    MPP_ENCODER_H264_LEVEL_31 = 31,                      /* H.264 level 3.1。 */
    MPP_ENCODER_H264_LEVEL_40 = 40,                      /* H.264 level 4.0。 */
    MPP_ENCODER_H264_LEVEL_41 = 41,                      /* H.264 level 4.1。 */
    MPP_ENCODER_H264_LEVEL_42 = 42,                      /* H.264 level 4.2。 */
    MPP_ENCODER_H264_LEVEL_50 = 50,                      /* H.264 level 5.0。 */
    MPP_ENCODER_H264_LEVEL_51 = 51,                      /* H.264 level 5.1。 */
    MPP_ENCODER_H264_LEVEL_52 = 52                       /* H.264 level 5.2。 */
} MppEncoderH264Level;

typedef enum {
    MPP_ENCODER_CABAC_DEFAULT = -1,                      /* 使用编码器默认 CABAC 策略。 */
    MPP_ENCODER_CABAC_DISABLED = 0,                      /* 关闭 CABAC。 */
    MPP_ENCODER_CABAC_ENABLED = 1                        /* 开启 CABAC。 */
} MppEncoderCabacMode;

typedef struct {
    MppCtx ctx;                 /* MPP 编码上下文句柄。 */
    MppApi *mpi;                /* MPP 提供的接口函数表。 */
    MppEncCfg cfg;              /* 编码配置对象。 */
} MppEncoderMppObjects;

typedef struct {
    MppBufferGroup frame_group; /* 输入帧缓冲组。 */
    MppBuffer frame_buffer;     /* 当前输入帧对应的 MPP Buffer。 */
    MppFrame frame;             /* 当前送入编码器的帧对象。 */
    int width;                  /* 输入图像宽度。 */
    int height;                 /* 输入图像高度。 */
    int hor_stride;             /* 水平 stride。 */
    int ver_stride;             /* 垂直 stride。 */
} MppEncoderInputFrame;

typedef struct {
    int fps;                    /* 编码帧率。 */
    int bitrate;                /* 目标码率。 */
    int gop;                    /* GOP 长度。 */
    MppEncoderRcMode rc_mode;   /* 当前生效的码控模式。 */
} MppEncoderRcState;

typedef struct {
    int qp_init;                /* QP 初始值。 */
    int qp_min;                 /* P/B 帧最小 QP。 */
    int qp_max;                 /* P/B 帧最大 QP。 */
    int qp_min_i;               /* I 帧最小 QP。 */
    int qp_max_i;               /* I 帧最大 QP。 */
    int qp_max_step;            /* 相邻帧最大 QP 变化步长。 */
} MppEncoderQpValues;

typedef struct {
    MppEncoderQpValues base;    /* 初始化时的 QP 基线，用于低光 FIXQP 参数退出后恢复。 */
    MppEncoderQpValues current; /* 当前下发到 MPP 的 QP 参数。 */
} MppEncoderQpState;

typedef struct {
    int64_t pts;                /* 送入 MPP 的时间戳计数。 */
} MppEncoderRuntimeState;

typedef struct {
    uint8_t *packet_cache;      /* 编码输出缓存，保存导出的 Annex-B 码流。 */
    size_t packet_cache_size;   /* packet_cache 当前容量。 */
} MppEncoderOutputCache;

typedef struct {
    MppEncoderMppObjects mpp;    /* MPP 上下文、接口表和配置对象。 */
    MppEncoderInputFrame input;  /* 输入帧 buffer、MppFrame 和几何信息。 */
    MppEncoderRcState rc;        /* 码控模式、目标码率、帧率和 GOP。 */
    MppEncoderQpState qp;        /* FIXQP/码控 QP 基线和当前参数。 */
    MppEncoderRuntimeState runtime; /* 编码运行时状态，例如递增 PTS。 */
    MppEncoderOutputCache output;   /* 编码输出缓存。 */
} MppEncoderCtx;

typedef struct {
    MppEncoderRcMode rc_mode;           /* 码控模式；无 options 时使用默认 CBR，0 是有效 VBR。 */
    MppEncoderH264Profile h264_profile; /* H264 profile；DEFAULT 表示默认 High。 */
    MppEncoderH264Level h264_level;     /* H264 level；DEFAULT 表示默认 4.0。 */
    MppEncoderCabacMode h264_cabac_en;  /* CABAC 开关；DEFAULT 表示使用默认开启策略。 */
    int qp_init;        /* 初始 QP；<=0 表示使用 MPP 默认值。 */
    int qp_min;         /* P/B 帧最小 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max;         /* P/B 帧最大 QP；<=0 表示使用 MPP 默认值。 */
    int qp_min_i;       /* I 帧最小 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max_i;       /* I 帧最大 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max_step;    /* 相邻帧最大 QP 变化步长；<=0 表示使用 MPP 默认值。 */
    int input_hor_stride; /* 输入 buffer 水平 stride；<=0 表示默认 16 对齐。 */
    int input_ver_stride; /* 输入 buffer 垂直 stride；<=0 表示默认 16 对齐。 */
} MppEncoderOptions;

typedef struct {
    uint64_t encoder_input_buffer_copy_us; /* NV12 copy into MPP input buffer. */
    uint64_t encoder_submit_frame_call_us; /* encode_put_frame call duration. */
    uint64_t encoder_poll_packet_call_us;  /* encode_get_packet call duration. */
    uint64_t encoder_packet_copy_us;       /* encoded packet copy duration. */
    uint64_t encode_frame_total_us;        /* Whole mpp_encoder_encode_frame duration. */
} MppEncoderTiming;

typedef struct {
    int prep_width;             /* MPP 当前配置中的输入宽度。 */
    int prep_height;            /* MPP 当前配置中的输入高度。 */
    int prep_hor_stride;        /* MPP 当前配置中的水平 stride。 */
    int prep_ver_stride;        /* MPP 当前配置中的垂直 stride。 */
    int rc_mode;                /* MPP 当前配置中的码控模式。 */
    int rc_gop;                 /* MPP 当前配置中的 GOP。 */
    int fps_in_num;             /* MPP 当前配置中的输入帧率分子。 */
    int fps_in_denorm;          /* MPP 当前配置中的输入帧率分母。 */
    int fps_out_num;            /* MPP 当前配置中的输出帧率分子。 */
    int fps_out_denorm;         /* MPP 当前配置中的输出帧率分母。 */
    int bps_target;             /* MPP 当前配置中的目标码率。 */
    int bps_max;                /* MPP 当前配置中的最大码率。 */
    int bps_min;                /* MPP 当前配置中的最小码率。 */
    int qp_init;                /* MPP 当前配置中的初始 QP。 */
    int qp_min;                 /* MPP 当前配置中的 P/B 最小 QP。 */
    int qp_max;                 /* MPP 当前配置中的 P/B 最大 QP。 */
    int qp_min_i;               /* MPP 当前配置中的 I 帧最小 QP。 */
    int qp_max_i;               /* MPP 当前配置中的 I 帧最大 QP。 */
    int qp_max_step;            /* MPP 当前配置中的相邻帧最大 QP 变化步长。 */
    int h264_profile;           /* MPP 当前配置中的 H.264 profile。 */
    int h264_level;             /* MPP 当前配置中的 H.264 level。 */
    int h264_cabac_en;          /* MPP 当前配置中的 CABAC 开关。 */
} MppEncoderRealtimeParams;

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

/*
 * 运行时更新编码目标码率。
 * 低照度噪声升高、场景复杂度变化时可通过该接口联动码率控制。
 */
int mpp_encoder_set_bitrate(MppEncoderCtx *enc, int bitrate);

int mpp_encoder_set_fps(MppEncoderCtx *enc, int fps);

/*
 * 查询编码器当前缓存的运行参数。
 * 这些值在 MPP 初始化或 MPP_ENC_SET_CFG 成功后更新，用于调试当前编码器实际生效目标。
 */
int mpp_encoder_get_video_encode_params(MppEncoderCtx *enc, MediaVideoEncodeParams *params);

/*
 * 从 MPP 编码器实时查询当前配置。
 * 该接口会执行 MPP_ENC_GET_CFG，再读取 MppEncCfg 中的关键编码字段。
 */
int mpp_encoder_query_realtime_params(MppEncoderCtx *enc, MppEncoderRealtimeParams *params);

/*
 * 运行时应用完整视频编码档位。
 * 用于动态帧率切换时同步更新 fps、码率、GOP、RC 和 QP 参数。
 * H.264 profile/level 属于编码器能力边界，应在初始化时按最高档位配置。
 */
int mpp_encoder_apply_video_encode_params(MppEncoderCtx *enc, const MediaVideoEncodeParams *params);

/*
 * 运行时更新 FIXQP 相关 QP 参数。
 * qp_delta 以初始化时的 QP 配置为基线，负数降低 QP 提升画质，0 恢复基线。
 * 该接口主要用于低照度策略在 FIXQP 模式下的兼容联动。
 */
int mpp_encoder_set_qp_delta(MppEncoderCtx *enc, int qp_delta);

#ifdef __cplusplus
}
#endif

#endif
