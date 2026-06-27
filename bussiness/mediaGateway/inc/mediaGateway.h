#ifndef __MEDIA_GATEWAY_H__
#define __MEDIA_GATEWAY_H__

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

#include "mppEncoder.h"
#include "v4l2Capture.h"
#include "mediaOutput.h"
#include "audioCapture.h"
#include "g711Encoder.h"
#include "aacEncoder.h"
#include "ispController.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_GATEWAY_MAX_STREAMS 2
#define MEDIA_GATEWAY_MAX_OUTPUTS 8
#define MEDIA_GATEWAY_MAX_CAPTURE_SOURCES MEDIA_GATEWAY_MAX_STREAMS

typedef struct {
    int normal_fps;                  /* 普通场景目标帧率。 */
    int low_light_fps;               /* 低照度场景目标帧率，OV13850 可用 15。 */
    int bright_fps;                  /* 亮光/低延迟场景目标帧率，OV13850 可用 60。 */
} DynamicFrameRateTargets;

typedef struct {
    int min_switch_interval_ms;      /* 两次目标帧率切换的最小间隔。 */
    int evaluate_interval_ms;        /* 策略评估周期。 */
    int bright_confirm_ms;           /* 亮光场景候选持续多久后允许切换。 */
    int low_light_confirm_ms;        /* 低照场景候选持续多久后允许切换。 */
    int ae_scene_confirm_ms;         /* AE 三态候选默认确认时间。 */
} DynamicFrameRateTiming;

typedef struct {
    float bright_max_exposure_us;    /* 亮光进入 60fps 的最大 AE 曝光时间，单位微秒。 */
    float bright_max_analog_gain;    /* 亮光进入 60fps 的最大模拟增益。 */
    float bright_min_mean_luma;      /* 亮光进入 60fps 的最小平均亮度。 */
    float low_light_min_exposure_ratio; /* 低照进入 15fps 的曝光/当前帧周期比例阈值。 */
    float low_light_min_analog_gain; /* 低照进入 15fps 的最小模拟增益。 */
    float low_light_max_mean_luma;   /* 低照进入 15fps 的最大平均亮度。 */
} DynamicFrameRateAeConfig;

typedef struct {
    int enabled;                     /* 是否启用动态帧率策略。 */
    DynamicFrameRateTargets targets; /* 目标帧率集合。 */
    DynamicFrameRateTiming timing;   /* 评估、确认和切换节流参数。 */
    DynamicFrameRateAeConfig ae;     /* AE 曝光/增益/亮度三态判定阈值。 */
} DynamicFrameRateConfig;

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
    int sample_rate;                 /* 音频采样率，G711 常用 8000，AAC 可按设备能力配置。 */
    int channels;                    /* 声道数，G711 要求 mono，AAC 支持 mono/stereo。 */
    AudioSampleFormat format;        /* 采集 PCM 格式，当前支持 S16LE。 */
    int period_frames;               /* 每个采集周期的单声道采样数。 */
    int buffer_periods;              /* ALSA 设备缓冲周期数。 */
    int source_slots;                /* audioFrameSource ring buffer 槽位数。 */
    int retry_ms;                    /* 音频采集失败后的重试间隔。 */
    int max_consecutive_failures;    /* 连续音频采集失败阈值。 */
    MediaCodecType codec;            /* 音频编码格式：MEDIA_CODEC_G711A/G711U/AAC。 */
    G711EncoderMode g711_mode;       /* G711 A-law / mu-law。 */
    int aac_bitrate;                 /* AAC 目标码率，单位 bit/s。 */
    int aac_profile;                 /* AAC object type，2 表示 AAC-LC。 */
    int bind_stream_index;           /* 音频包投递到哪一路码流绑定的输出。 */
} AudioSourceConfig;

typedef struct {
    int enabled;                     /* 是否启用 RKAIQ/ISP 控制链路。 */
    const char *sensor_name;         /* sensor media entity 名称；为空时按 video_device 自动反查。 */
    const char *video_device;        /* 用于反查 sensor 的 video node，例如 /dev/video0。 */
    const char *iq_dir;              /* IQ 文件目录。 */
    const char *force_iq_file;       /* 可选：强制使用指定 IQ 文件。 */
    int width;                       /* RKAIQ prepare 宽度，<=0 时使用主采集源宽度。 */
    int height;                      /* RKAIQ prepare 高度，<=0 时使用主采集源高度。 */
    int working_mode;                /* rk_aiq_working_mode_t，0 表示 normal。 */
    int keep_external_hw_state;      /* stop 时是否保留外部补光/IRCut 等硬件状态。 */
    int fallback_on_error;           /* ISP 初始化失败时是否降级继续运行。 */
    IspControllerImageControls controls; /* ISP runtime image controls. */
    int health_check_enable;         /* 是否启用 ISP/RKAIQ 健康诊断。 */
    int meta_timeout_ms;             /* started 后 metas 回调允许停滞的最长时间。 */
    int max_error_count;             /* RKAIQ error 回调累计诊断阈值。 */
    int restart_on_fault;            /* 预留：健康异常时是否允许上层执行协同重启。 */
    IspControllerLowLightConfig low_light; /* 低照度自动优化策略配置。 */
} IspSourceConfig;

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
} MediaGatewayOutputSwitchConfig;

typedef struct {
    int fps;                         /* 顶层默认编码帧率；多码流配置以 streams[i].fps 为准。 */
    int bitrate;                     /* 顶层默认编码目标码率，单位 bit/s；多码流配置以 streams[i].bitrate 为准。 */
    int gop;                         /* 顶层默认 GOP 长度；多码流配置以 streams[i].gop 为准。 */
    int rc_mode;                     /* 默认码率控制模式，取值来自 MPP_ENC_RC_MODE_*。 */
    int h264_profile;                /* 默认 H264 profile 配置，例如 66/77/100。 */
    int h264_level;                  /* 默认 H264 level 配置，例如 40。 */
    int h264_cabac_en;               /* 默认是否启用 CABAC。 */
    int qp_init;                     /* 默认初始 QP；<=0 表示使用 MPP 默认值。 */
    int qp_min;                      /* 默认 P/B 帧最小 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max;                      /* 默认 P/B 帧最大 QP；<=0 表示使用 MPP 默认值。 */
    int qp_min_i;                    /* 默认 I 帧最小 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max_i;                    /* 默认 I 帧最大 QP；<=0 表示使用 MPP 默认值。 */
    int qp_max_step;                 /* 默认相邻帧最大 QP 变化步长；<=0 表示使用 MPP 默认值。 */
} MediaGatewayEncodeDefaultConfig;

typedef struct {
    int low_latency_mode;            /* 低延时模式开关，主要影响调试和日志输出策略。 */
    int stats_interval_sec;          /* 统计信息输出周期，单位秒。 */
    int capture_retry_ms;            /* 采集失败后的重试间隔，单位毫秒。 */
    int max_consecutive_failures;    /* 连续失败达到该阈值时主循环退出。 */
    const char *config_file_path;    /* 预留的配置文件路径钩子。 */
} MediaGatewayRuntimeConfig;

typedef struct {
    const char *file_path;           /* 本地录像文件路径，为空则不录制。 */
    int flush_interval_frames;       /* 本地录像每隔多少帧执行一次 fflush。 */
} MediaGatewayRecordConfig;

typedef struct {
    int enabled;                     /* 是否开启性能测试埋点日志。 */
    int sample_every;                /* 性能埋点每隔多少帧采样一次。 */
    int print_interval_sec;          /* 性能埋点日志打印周期，单位秒。 */
} MediaGatewayBenchConfig;

typedef struct {
    int level;                       /* 全局日志等级：0 debug, 1 info, 2 warn, 3 error。 */
} MediaGatewayLogConfig;

typedef struct {
    MediaGatewayOutputSwitchConfig output; /* 顶层输出开关配置；多码流配置以 streams[i] 为准。 */
    MediaGatewayEncodeDefaultConfig encode; /* 顶层编码默认值；多码流配置以 streams[i] 为准。 */
    MediaGatewayRuntimeConfig runtime; /* 网关运行期公共配置。 */
    MediaGatewayRecordConfig record;  /* 本地录像配置。 */
    MediaGatewayBenchConfig bench;    /* 性能埋点配置。 */
    MediaGatewayLogConfig log;        /* 日志配置。 */
    DynamicFrameRateConfig dynamic_fps; /* 动态帧率策略配置。 */
    int capture_source_count;        /* 采集源数量。 */
    CaptureSourceConfig capture_sources[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 采集源配置。 */
    IspSourceConfig isp;             /* RKAIQ/ISP 初始化与生命周期配置。 */
    AudioSourceConfig audio;         /* 音频采集与编码配置。 */
    int stream_count;                /* 流配置数量，<=0 表示不初始化视频流。 */
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
    uint64_t sum_us;                                       /* 当前窗口内耗时累计。 */
    uint64_t max_us;                                       /* 当前窗口内最大耗时。 */
} benchmarkMetric;

typedef struct {
    uint64_t sample_count;                                 /* 当前窗口内采样帧数。 */
    benchmarkMetric camera_buffer_wait;        /* 摄像头/驱动缓冲等待到 DQBUF 返回的时间。 */
    benchmarkMetric dqbuf_ioctl_duration;      /* VIDIOC_DQBUF ioctl 调用耗时。 */
    benchmarkMetric capture_call_duration;     /* v4l2_capture_frame 整体调用耗时。 */
    benchmarkMetric mmap_to_frame_cache_copy;  /* V4L2 mmap buffer 拷贝到 frame_cache 耗时。 */
    benchmarkMetric frame_source_publish;      /* 采集线程发布到 MediaFrameSource 槽位的总耗时。 */
    benchmarkMetric frame_source_publish_copy; /* frame source 发布时整帧拷贝耗时。 */
    benchmarkMetric video_input_publish_copy;  /* 发布到视频编码输入槽的整帧拷贝耗时。 */
    benchmarkMetric video_input_acquire_copy;  /* 编码线程从视频输入槽取本地副本的拷贝耗时。 */
    benchmarkMetric dqbuf_to_encode_start;     /* DQBUF 返回到开始送入编码器的时间。 */
    benchmarkMetric encode_start_to_done;      /* 开始送入编码器到拿到编码包的时间。 */
    benchmarkMetric encoder_input_buffer_copy; /* NV12 拷贝到 MPP 输入缓冲耗时。 */
    benchmarkMetric encoder_submit_frame_call; /* encode_put_frame 调用耗时。 */
    benchmarkMetric encoder_poll_packet_call;  /* encode_get_packet 调用耗时。 */
    benchmarkMetric encoder_packet_copy;       /* H264 packet 拷贝耗时。 */
    benchmarkMetric encode_frame_total;        /* mpp_encoder_encode_frame 总耗时。 */
    benchmarkMetric dqbuf_to_encode_done;      /* DQBUF 返回到拿到编码包的时间。 */
    benchmarkMetric dqbuf_to_output_queued;    /* DQBUF 返回到输出队列入队完成的时间。 */
} MediaGatewayBenchmarkWindow;

typedef struct {
    int enable;                                            /* 是否开启 benchmark 埋点。 */
    int sample_every;                                      /* 采样间隔帧数。 */
    int print_interval_sec;                                /* benchmark 输出周期，单位秒。 */
    uint64_t last_ts_us;                                   /* 上次 benchmark 输出时间戳。 */
    MediaGatewayBenchmarkWindow streams[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流 benchmark 统计窗口。 */
} MediaGatewayBenchmarkStats;

typedef struct {
    int current_fps;                                      /* 当前已应用/初始帧率。 */
    int target_fps;                                       /* 当前策略目标帧率。 */
    int last_logged_target_fps;                           /* 上次日志记录的目标帧率。 */
    int low_light_active;                                 /* 最近一次策略评估使用的低照状态。 */
    int bright_active;                                    /* 最近一次策略评估使用的亮光状态。 */
    uint64_t last_evaluate_ts_us;                         /* 上次策略评估时间。 */
    uint64_t last_switch_ts_us;                           /* 上次目标帧率切换时间。 */
    int pending_fps;                                      /* AE 候选目标帧率。 */
    uint64_t pending_since_ts_us;                         /* AE 候选目标开始持续时间。 */
    char reason[128];                                     /* 最近一次策略决策原因。 */
} MediaGatewayDynamicFpsState;

typedef struct {
    V4L2CaptureCtx captures[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 各采集源 V4L2 上下文。 */
    IspControllerCtx isp;                       /* RKAIQ/ISP 控制上下文。 */
    int isp_ready;                              /* ISP 控制链路是否已启动。 */
    AudioCaptureCtx audio_capture;               /* 音频采集上下文。 */
    G711EncoderCtx audio_encoder;                /* G711 音频编码上下文。 */
    AacEncoderCtx aac_encoder;                    /* AAC 音频编码上下文。 */
    MppEncoderCtx encoders[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流编码模块上下文。 */
    int stream_enabled[MEDIA_GATEWAY_MAX_STREAMS];     /* 各码流是否启用。 */
    int capture_ready[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 各采集源是否已初始化成功。 */
    int audio_capture_ready;                 /* 音频采集是否已初始化。 */
    int audio_encoder_ready;                 /* 音频编码器是否已初始化。 */
    MediaOutput outputs[MEDIA_GATEWAY_MAX_OUTPUTS];  /* 已启用的输出通道集合。 */
    int output_stream_index[MEDIA_GATEWAY_MAX_OUTPUTS]; /* 每个输出通道绑定的 stream 下标。 */
    int output_count;                           /* 当前启用的输出通道数量。 */
    MediaGatewayConfig config;                 /* 配置参数 */
    int rtsp_output_index[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流 RTSP 输出索引。 */
    int gb28181_output_index[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流 GB28181 输出索引。 */
    int encoder_ready[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流编码模块是否已初始化成功。 */
    int low_light_bitrate_active;                 /* 当前低照度编码联动策略是否已应用；用于进入/退出时恢复码率或 QP profile。 */
    uint64_t last_isp_policy_ts_us;               /* 上次运行 ISP/低照度策略的时间戳，避免策略跟随日志周期。 */
    MediaGatewayDynamicFpsState dynamic_fps_state; /* 动态帧率策略运行状态。 */
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
