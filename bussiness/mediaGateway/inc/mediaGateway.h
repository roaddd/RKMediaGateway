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

#define MEDIA_GATEWAY_MAX_SINKS 4

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
    MppEncoderCtx encoder;                     /* 编码模块上下文。 */
    MediaSink sinks[MEDIA_GATEWAY_MAX_SINKS];  /* 已启用的输出通道集合。 */
    int sink_count;                            /* 当前启用的 sink 数量。 */
    MediaGatewayConfig config;                 /* 归一化后的网关配置副本。 */
    int gb28181_sink_index;                    /* gb28181 sink 在 sinks[] 中的索引，-1 表示未启用。 */
    int capture_ready;                         /* 采集模块是否已初始化成功。 */
    int encoder_ready;                         /* 编码模块是否已初始化成功。 */
    int running;                               /* 主循环是否正在运行。 */
    FILE *record_fp;                           /* 本地录像文件句柄。 */
    uint64_t stat_last_ts_us;                  /* 上次统计输出时间戳。 */
    uint64_t stat_frames;                      /* 当前统计窗口内累计帧数。 */
    uint64_t stat_bytes;                       /* 当前统计窗口内累计字节数。 */
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
