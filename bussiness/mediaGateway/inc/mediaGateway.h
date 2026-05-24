#ifndef __MEDIA_GATEWAY_H__
#define __MEDIA_GATEWAY_H__

#include <pthread.h>
#include <stdio.h>

#include "mppEncoder.h"
#include "v4l2Capture.h"
#include "mediaOutput.h"
#include "audioCapture.h"
#include "g711Encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_GATEWAY_MAX_STREAMS 2
#define MEDIA_GATEWAY_MAX_OUTPUTS 8
#define MEDIA_GATEWAY_MAX_CAPTURE_SOURCES MEDIA_GATEWAY_MAX_STREAMS

typedef struct {
    int enabled;                     /* 该采集源是否启用。 */
    const char *name;                /* 采集源名称，例如 main_path/self_path。 */
    const char *device_path;         /* V4L2 设备节点，例如 /dev/video0。 */
    int width;                       /* 采集宽度。 */
    int height;                      /* 采集高度。 */
    uint32_t pixelformat;            /* V4L2 像素格式，例如 V4L2_PIX_FMT_NV12。 */
    int buffer_count;                /* V4L2 mmap buffer 数量。 */
} CaptureSourceConfig;

typedef struct {
    int enabled;                     /* 是否启用音频采集与编码。 */
    const char *device_name;         /* ALSA 采集设备，例如 default/hw:0,0。 */
    int sample_rate;                 /* 音频采样率，G711 常用 8000。 */
    int channels;                    /* 声道数，当前 G711 编码路径要求 mono。 */
    AudioSampleFormat format;        /* 采集 PCM 格式，当前支持 S16LE。 */
    int period_frames;               /* 每个采集周期的单声道采样数。 */
    int buffer_periods;              /* ALSA 设备缓冲周期数。 */
    int source_slots;                /* audioFrameSource ring buffer 槽位数。 */
    int retry_ms;                    /* 音频采集失败后的重试间隔。 */
    int max_consecutive_failures;    /* 连续音频采集失败阈值。 */
    G711EncoderMode g711_mode;       /* G711 A-law / mu-law。 */
    int bind_stream_index;           /* 音频包投递到哪一路码流绑定的输出。 */
} AudioSourceConfig;

typedef struct {
    int enabled;                     /* 该码流是否启用。 */
    const char *name;                /* 码流名称，建议 main/sub。 */
    int source_index;                /* 绑定的采集源下标。 */
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

    int enable_rtsp;                 /* 该码流是否启用 RTSP 输出。 */
    int enable_rtmp;                 /* 该码流是否启用 RTMP 输出。 */
    int enable_gb28181;              /* 该码流是否启用 GB28181 输出。 */
    MediaOutputRtspConfig rtsp;      /* 该码流 RTSP 输出配置。 */
    MediaOutputRtmpConfig rtmp;      /* 该码流 RTMP 输出配置。 */
    MediaOutputGb28181Config gb28181;/* 该码流 GB28181 输出配置。 */
} MediaGatewayStreamConfig;

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
    int bench_enable;                /* 是否开启性能测试埋点日志。 */
    int bench_sample_every;          /* 性能埋点每隔多少帧采样一次。 */
    int bench_print_interval_sec;    /* 性能埋点日志打印周期，单位秒。 */
    int log_level;                   /* 全局日志等级：0 debug, 1 info, 2 warn, 3 error。 */
    int capture_source_count;        /* 采集源数量。 */
    CaptureSourceConfig capture_sources[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 采集源配置。 */
    AudioSourceConfig audio;         /* 音频采集与编码配置。 */
    int stream_count;                /* 流配置数量，<=0 表示使用兼容模式自动生成 main 流。 */
    MediaGatewayStreamConfig streams[MEDIA_GATEWAY_MAX_STREAMS]; /* 多路码流配置。 */
    MediaOutputRtspConfig rtsp;      /* RTSP 协议专用配置块。 */
    MediaOutputRtmpConfig rtmp;      /* RTMP 协议专用配置块。 */
    MediaOutputGb28181Config gb28181;/* GB28181/SIP+RTP 协议专用配置块。 */
} MediaGatewayConfig;

typedef struct {
    double fps;            /* 当前统计窗口内的平均帧率。 */
    double bitrate_kbps;   /* 当前统计窗口内的平均码率，单位 kbps。 */
    uint64_t frames;       /* 当前统计窗口内累计处理帧数。 */
    uint64_t bytes;        /* 当前统计窗口内累计处理字节数。 */
} MediaGatewayThroughput;

typedef struct {
    uint64_t last_ts_us;                                   /* 上次吞吐统计输出时间戳。 */
    uint64_t frames;                                       /* 当前统计窗口内累计视频帧数。 */
    uint64_t bytes;                                        /* 当前统计窗口内累计视频字节数。 */
    uint64_t stream_frames[MEDIA_GATEWAY_MAX_STREAMS];     /* 各码流窗口内累计视频帧数。 */
    uint64_t stream_bytes[MEDIA_GATEWAY_MAX_STREAMS];      /* 各码流窗口内累计视频字节数。 */
    uint64_t audio_frames;                                 /* 当前统计窗口内累计音频帧数。 */
    uint64_t audio_bytes;                                  /* 当前统计窗口内累计音频字节数。 */
} MediaGatewayStats;

typedef struct {
    int enable;                                            /* 是否开启 benchmark 埋点。 */
    int sample_every;                                      /* 采样间隔帧数。 */
    int print_interval_sec;                                /* benchmark 输出周期，单位秒。 */
    uint64_t last_ts_us;                                   /* 上次 benchmark 输出时间戳。 */
    uint64_t sample_count;                                 /* 当前窗口内采样帧数。 */
    uint64_t camera_buffer_wait_sum_us;                    /* 摄像头/驱动缓冲等待到 DQBUF 返回的时间累计。 */
    uint64_t camera_buffer_wait_max_us;                    /* 摄像头/驱动缓冲等待到 DQBUF 返回的最大时间。 */
    uint64_t dqbuf_ioctl_duration_sum_us;                  /* VIDIOC_DQBUF ioctl 调用耗时累计。 */
    uint64_t dqbuf_ioctl_duration_max_us;                  /* VIDIOC_DQBUF ioctl 调用最大耗时。 */
    uint64_t capture_call_duration_sum_us;                 /* v4l2_capture_frame 整体调用耗时累计。 */
    uint64_t capture_call_duration_max_us;                 /* v4l2_capture_frame 整体调用最大耗时。 */
    uint64_t mmap_to_frame_cache_copy_sum_us;              /* V4L2 mmap buffer 拷贝到 frame_cache 耗时累计。 */
    uint64_t mmap_to_frame_cache_copy_max_us;              /* V4L2 mmap buffer 拷贝到 frame_cache 最大耗时。 */
    uint64_t frame_source_publish_sum_us;                  /* 采集线程发布到 MediaFrameSource 槽位的总耗时累计。 */
    uint64_t frame_source_publish_max_us;                  /* 采集线程发布到 MediaFrameSource 槽位的总耗时最大值。 */
    uint64_t frame_source_publish_copy_sum_us;             /* frame source 发布时整帧拷贝耗时累计。 */
    uint64_t frame_source_publish_copy_max_us;             /* frame source 发布时整帧拷贝最大值。 */
    uint64_t video_input_publish_copy_sum_us;              /* 发布到视频编码输入槽的整帧拷贝耗时累计。 */
    uint64_t video_input_publish_copy_max_us;              /* 发布到视频编码输入槽的整帧拷贝最大值。 */
    uint64_t video_input_acquire_copy_sum_us;              /* 编码线程从视频输入槽取本地副本的拷贝耗时累计。 */
    uint64_t video_input_acquire_copy_max_us;              /* 编码线程从视频输入槽取本地副本的拷贝最大值。 */
    uint64_t dqbuf_to_encode_start_sum_us;                 /* DQBUF 返回到开始送入编码器的时间累计。 */
    uint64_t dqbuf_to_encode_start_max_us;                 /* DQBUF 返回到开始送入编码器的最大时间。 */
    uint64_t encode_start_to_done_sum_us;                  /* 开始送入编码器到拿到编码包的时间累计。 */
    uint64_t encode_start_to_done_max_us;                  /* 开始送入编码器到拿到编码包的最大时间。 */
    uint64_t encoder_input_buffer_copy_sum_us;             /* NV12 拷贝到 MPP 输入缓冲累计。 */
    uint64_t encoder_input_buffer_copy_max_us;             /* NV12 拷贝到 MPP 输入缓冲最大值。 */
    uint64_t encoder_submit_frame_call_sum_us;             /* encode_put_frame 调用耗时累计。 */
    uint64_t encoder_submit_frame_call_max_us;             /* encode_put_frame 调用最大耗时。 */
    uint64_t encoder_poll_packet_call_sum_us;              /* encode_get_packet 调用耗时累计。 */
    uint64_t encoder_poll_packet_call_max_us;              /* encode_get_packet 调用最大耗时。 */
    uint64_t encoder_packet_copy_sum_us;                   /* H264 packet 拷贝耗时累计。 */
    uint64_t encoder_packet_copy_max_us;                   /* H264 packet 拷贝最大耗时。 */
    uint64_t encode_frame_total_sum_us;                    /* mpp_encoder_encode_frame 总耗时累计。 */
    uint64_t encode_frame_total_max_us;                    /* mpp_encoder_encode_frame 总耗时最大值。 */
    uint64_t dqbuf_to_encode_done_sum_us;                  /* DQBUF 返回到拿到编码包的时间累计。 */
    uint64_t dqbuf_to_encode_done_max_us;                  /* DQBUF 返回到拿到编码包的最大时间。 */
    uint64_t dqbuf_to_output_queued_sum_us;                /* DQBUF 返回到输出队列入队完成的时间累计。 */
    uint64_t dqbuf_to_output_queued_max_us;                /* DQBUF 返回到输出队列入队完成的最大时间。 */
} MediaGatewayBenchmarkStats;

typedef struct {
    V4L2CaptureCtx captures[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 各采集源 V4L2 上下文。 */
    AudioCaptureCtx audio_capture;               /* 音频采集上下文。 */
    G711EncoderCtx audio_encoder;                /* G711 音频编码上下文。 */
    MppEncoderCtx encoders[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流编码模块上下文。 */
    int stream_enabled[MEDIA_GATEWAY_MAX_STREAMS];     /* 各码流是否启用。 */
    int capture_ready[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 各采集源是否已初始化成功。 */
    int audio_capture_ready;                 /* 音频采集是否已初始化。 */
    int audio_encoder_ready;                 /* 音频编码器是否已初始化。 */
    MediaOutput outputs[MEDIA_GATEWAY_MAX_OUTPUTS];  /* 已启用的输出通道集合。 */
    int output_stream_index[MEDIA_GATEWAY_MAX_OUTPUTS]; /* 每个输出通道绑定的 stream 下标。 */
    int output_count;                           /* 当前启用的输出通道数量。 */
    MediaGatewayConfig config;                 /* 归一化后的网关配置副本。 */
    int rtsp_output_index[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流 RTSP 输出索引。 */
    int gb28181_output_index[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流 GB28181 输出索引。 */
    int encoder_ready[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流编码模块是否已初始化成功。 */
    int running;                               /* 主循环是否正在运行。 */
    FILE *record_fp;                           /* 本地录像文件句柄。 */
    pthread_mutex_t stats_lock;                /* 保护吞吐、benchmark 和本地录像写入。 */
    int stats_lock_ready;                      /* stats_lock 是否已初始化。 */
    MediaGatewayStats stats;                   /* 吞吐统计窗口。 */
    MediaGatewayBenchmarkStats bench;          /* benchmark 埋点配置与统计窗口。 */
    uint8_t *scaled_frame_cache[MEDIA_GATEWAY_MAX_STREAMS]; /* 缩放后的 NV12 帧缓存。 */
    size_t scaled_frame_cache_size[MEDIA_GATEWAY_MAX_STREAMS]; /* 缩放缓存容量。 */
} MediaGatewayCtx;

typedef struct {
    int consecutive_encode_fail[MEDIA_GATEWAY_MAX_STREAMS]; /* 每路连续编码失败次数。 */
    int rga_fallback_warned[MEDIA_GATEWAY_MAX_STREAMS];     /* 每路 CPU 缩放 fallback 告警是否已打印。 */
    int dmabuf_direct_logged[MEDIA_GATEWAY_MAX_STREAMS];    /* 每路 DMA-BUF 直通判断日志是否已打印。 */
} MediaGatewayRunState;

/**
 * @description: 初始化 media gateway 长生命周期资源和归一化配置。
 */
int media_gateway_init(MediaGatewayCtx *ctx, const MediaGatewayConfig *config);

/**
 * @description: 运行 gateway 主循环，启动采集源和音视频编码流水线。
 */
int media_gateway_run(MediaGatewayCtx *ctx);

/**
 * @description: 请求 gateway 主循环退出。
 */
void media_gateway_stop(MediaGatewayCtx *ctx);

/**
 * @description: 释放 media gateway 持有的全部资源。
 */
void media_gateway_deinit(MediaGatewayCtx *ctx);

/**
 * @description: 读取当前吞吐统计窗口的估算值。
 */
void media_gateway_get_throughput(MediaGatewayCtx *ctx, MediaGatewayThroughput *throughput);

#ifdef __cplusplus
}
#endif

#endif
