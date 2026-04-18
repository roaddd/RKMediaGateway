#ifndef __MEDIA_GATEWAY_H__
#define __MEDIA_GATEWAY_H__

#include <stdio.h>

#include "mppEncoder.h"
#include "v4l2Capture.h"
#include "mediaSink.h"
#include "rtspSink.h"
#include "rtmpSink.h"
#include "gb28181Sink.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_GATEWAY_MAX_STREAMS 2
#define MEDIA_GATEWAY_MAX_SINKS 8

typedef struct {
    int enabled;                     /* 该码流是否启用。 */
    const char *name;                /* 码流名称，建议 main/sub。 */
    int width;                       /* 该码流编码宽度。 */
    int height;                      /* 该码流编码高度。 */
    int fps;                         /* 该码流帧率。 */
    int bitrate;                     /* 该码流目标码率，单位 bit/s。 */
    int gop;                         /* 该码流 GOP 长度。 */
    int rc_mode;                     /* 该码流码率控制模式。 */
    int h264_profile;                /* 该码流 H264 profile。 */
    int h264_level;                  /* 该码流 H264 level。 */
    int h264_cabac_en;               /* 该码流是否启用 CABAC。 */
    int qp_init;                     /* 该码流初始 QP。 */
    int qp_min;                      /* 该码流 P/B 最小 QP。 */
    int qp_max;                      /* 该码流 P/B 最大 QP。 */
    int qp_min_i;                    /* 该码流 I 帧最小 QP。 */
    int qp_max_i;                    /* 该码流 I 帧最大 QP。 */
    int qp_max_step;                 /* 该码流相邻帧最大 QP 变化步长。 */

    int enable_rtsp;                 /* 该码流是否启用 RTSP sink。 */
    int enable_rtmp;                 /* 该码流是否启用 RTMP sink。 */
    int enable_gb28181;              /* 该码流是否启用 GB28181 sink。 */
    RtspSinkConfig rtsp;             /* 该码流 RTSP 配置。 */
    RtmpSinkConfig rtmp;             /* 该码流 RTMP 配置。 */
    Gb28181SinkConfig gb28181;       /* 该码流 GB28181 配置。 */
} MediaGatewayStreamConfig;

/*
 * Benchmark 编译期默认配置。
 * 如需调整，可在编译参数里通过 -D 覆盖这些宏。
 */
#ifndef MEDIA_GATEWAY_BENCH_ENABLE_DEFAULT
#define MEDIA_GATEWAY_BENCH_ENABLE_DEFAULT 0
#endif

#ifndef MEDIA_GATEWAY_BENCH_SAMPLE_EVERY_DEFAULT
#define MEDIA_GATEWAY_BENCH_SAMPLE_EVERY_DEFAULT 30
#endif

#ifndef MEDIA_GATEWAY_BENCH_PRINT_INTERVAL_SEC_DEFAULT
#define MEDIA_GATEWAY_BENCH_PRINT_INTERVAL_SEC_DEFAULT 1
#endif

typedef struct {
    int enable_rtsp;                 /* 是否启用 RTSP 输出链路。 */
    int enable_rtmp;                 /* 是否启用 RTMP 输出链路。 */
    int enable_gb28181;              /* 是否启用 GB28181 设备输出链路。 */
    int fps;                         /* 全局编码帧率，所有输出协议共用。 */
    int bitrate;                     /* 全局编码目标码率，单位 bit/s。 */
    int gop;                         /* GOP 长度，影响关键帧间隔和恢复速度。 */
    int rc_mode;                     /* 编码码率控制模式，取值来自 MPP_ENC_RC_MODE_*。 */
    int h264_profile;                /* H264 profile 配置，例如 66/77/100。 */
    int h264_level;                  /* H264 level 配置，例如 40。 */
    int h264_cabac_en;               /* 是否启用 CABAC。 */
    int qp_init;                     /* 初始 QP；<=0 表示使用 MPP 默认值。 */
    int qp_min;                      /* P/B 帧最小 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max;                      /* P/B 帧最大 QP；<=0 表示使用 MPP 默认值。 */
    int qp_min_i;                    /* I 帧最小 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max_i;                    /* I 帧最大 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max_step;                 /* 相邻帧最大 QP 变化步长；<=0 表示使用 MPP 默认值。 */
    int low_latency_mode;            /* 低延时模式开关，主要影响调试和日志输出策略。 */
    int stats_interval_sec;          /* 统计信息输出周期，单位秒。 */
    int capture_retry_ms;            /* 采集失败后的重试间隔，单位毫秒。 */
    int max_consecutive_failures;    /* 连续失败达到该阈值时主循环退出。 */
    const char *record_file_path;    /* 本地录像文件路径，为空则不录制。 */
    int record_flush_interval_frames;/* 本地录像每隔多少帧执行一次 fflush。 */
    const char *config_file_path;    /* 预留的配置文件路径钩子。 */
    int stream_count;                /* 流配置数量，<=0 表示使用兼容模式自动生成 main 流。 */
    MediaGatewayStreamConfig streams[MEDIA_GATEWAY_MAX_STREAMS]; /* 多路码流配置。 */
    RtspSinkConfig rtsp;             /* RTSP 协议专用配置块。 */
    RtmpSinkConfig rtmp;             /* RTMP 协议专用配置块。 */
    Gb28181SinkConfig gb28181;       /* GB28181/SIP+RTP 协议专用配置块。 */
} MediaGatewayConfig;

typedef struct {
    double fps;            /* 当前统计窗口内的平均帧率。 */
    double bitrate_kbps;   /* 当前统计窗口内的平均码率，单位 kbps。 */
    uint64_t frames;       /* 当前统计窗口内累计处理帧数。 */
    uint64_t bytes;        /* 当前统计窗口内累计处理字节数。 */
} MediaGatewayThroughput;

typedef struct {
    V4L2CaptureCtx capture;                    /* 采集模块上下文。 */
    MppEncoderCtx encoders[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流编码模块上下文。 */
    int stream_enabled[MEDIA_GATEWAY_MAX_STREAMS];     /* 各码流是否启用。 */
    MediaSink sinks[MEDIA_GATEWAY_MAX_SINKS];  /* 已启用的输出通道集合。 */
    int sink_stream_index[MEDIA_GATEWAY_MAX_SINKS]; /* 每个 sink 绑定的 stream 下标。stream:main、sub等；sink:gb28181Sink、rtmpSink、rtspSink等 */
    int sink_count;                            /* 当前启用的 sink 数量。 */
    MediaGatewayConfig config;                 /* 归一化后的网关配置副本。 */
    int gb28181_sink_index[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流 gb28181 sink 索引。 */
    int capture_ready;                         /* 采集模块是否已初始化成功。 */
    int encoder_ready[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流编码模块是否已初始化成功。 */
    int running;                               /* 主循环是否正在运行。 */
    FILE *record_fp;                           /* 本地录像文件句柄。 */
    uint64_t stat_last_ts_us;                  /* 上次统计输出时间戳。 */
    uint64_t stat_frames;                      /* 当前统计窗口内累计帧数。 */
    uint64_t stat_bytes;                       /* 当前统计窗口内累计字节数。 */
    uint64_t stream_stat_frames[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流窗口内累计帧数。 */
    uint64_t stream_stat_bytes[MEDIA_GATEWAY_MAX_STREAMS];  /* 各码流窗口内累计字节数。 */
    uint8_t *scaled_frame_cache[MEDIA_GATEWAY_MAX_STREAMS]; /* 缩放后的 NV12 帧缓存。 */
    size_t scaled_frame_cache_size[MEDIA_GATEWAY_MAX_STREAMS]; /* 缩放缓存容量。 */

    /* Benchmark 埋点开关与统计窗口（默认值来自头文件宏）。 */
    int bench_enable;                          /* 是否开启 benchmark 埋点。 */
    int bench_sample_every;                    /* 采样间隔帧数。 */
    int bench_print_interval_sec;              /* benchmark 输出周期，单位秒。 */
    uint64_t bench_last_ts_us;                 /* 上次 benchmark 输出时间戳。 */
    uint64_t bench_sample_count;               /* 当前窗口内采样帧数。 */

    uint64_t bench_driver_to_dqbuf_sum_us;     /* driver_ts -> dqbuf 时间累计。 */
    uint64_t bench_driver_to_dqbuf_max_us;     /* driver_ts -> dqbuf 最大值。 */
    uint64_t bench_dqbuf_to_put_sum_us;        /* dqbuf -> encode_put 时间累计。 */
    uint64_t bench_dqbuf_to_put_max_us;        /* dqbuf -> encode_put 最大值。 */
    uint64_t bench_put_to_get_sum_us;          /* encode_put -> encode_get 时间累计。 */
    uint64_t bench_put_to_get_max_us;          /* encode_put -> encode_get 最大值。 */
    uint64_t bench_dqbuf_to_get_sum_us;        /* dqbuf -> encode_get 时间累计。 */
    uint64_t bench_dqbuf_to_get_max_us;        /* dqbuf -> encode_get 最大值。 */
    uint64_t bench_dqbuf_to_fanout_sum_us;     /* dqbuf -> fanout 完成累计。 */
    uint64_t bench_dqbuf_to_fanout_max_us;     /* dqbuf -> fanout 完成最大值。 */
} MediaGatewayCtx;

int media_gateway_init(MediaGatewayCtx *ctx, const MediaGatewayConfig *config);
int media_gateway_run(MediaGatewayCtx *ctx);
void media_gateway_stop(MediaGatewayCtx *ctx);
void media_gateway_deinit(MediaGatewayCtx *ctx);
void media_gateway_get_throughput(MediaGatewayCtx *ctx, MediaGatewayThroughput *throughput);

#ifdef __cplusplus
}
#endif

#endif
