#ifndef __MEDIA_GATEWAY_H__
#define __MEDIA_GATEWAY_H__

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

#include "mppEncoder.h"
#include "v4l2Capture.h"
#include "mediaOutput.h"
#include "mediaControlMessage.h"
#include "audioCapture.h"
#include "audioEncoder.h"
#include "ispController.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_GATEWAY_MAX_STREAMS 2
#define MEDIA_GATEWAY_MAX_OUTPUTS 8
#define MEDIA_GATEWAY_MAX_CAPTURE_SOURCES MEDIA_GATEWAY_MAX_STREAMS
#define MEDIA_GATEWAY_MAX_AUDIO_ENCODER_GROUPS 8
#define MEDIA_GATEWAY_AUDIO_ENCODER_GROUP_NAME_SIZE 64

typedef struct {
    int normal_fps;                  /* 普通场景目标帧率。 */
    int low_light_fps;               /* 低照度场景目标帧率，OV13850 可用 15。 */
    int bright_fps;                  /* 亮光/低延迟场景目标帧率，OV13850 可用 60。 */
} MediaGatewayLightFpsTargets;

typedef struct {
    int min_switch_interval_ms;      /* 两次目标帧率切换的最小间隔。 */
    int evaluate_interval_ms;        /* 策略评估周期。 */
    int bright_confirm_ms;           /* 亮光场景候选持续多久后允许切换。 */
    int low_light_confirm_ms;        /* 低照场景候选持续多久后允许切换。 */
    int ae_scene_confirm_ms;         /* AE 三态候选默认确认时间。 */
} MediaGatewayLightFpsTiming;

typedef struct {
    float bright_max_exposure_us;    /* 亮光进入 60fps 的最大 AE 曝光时间，单位微秒。 */
    float bright_max_analog_gain;    /* 亮光进入 60fps 的最大模拟增益。 */
    float bright_min_mean_luma;      /* 亮光进入 60fps 的最小平均亮度。 */
    float low_light_min_exposure_ratio; /* 低照进入 15fps 的曝光/当前帧周期比例阈值。 */
    float low_light_min_analog_gain; /* 低照进入 15fps 的最小模拟增益。 */
    float low_light_max_mean_luma;   /* 低照进入 15fps 的最大平均亮度。 */
} MediaGatewayLightFpsAeConfig;

typedef struct {
    int enabled;                     /* 是否启用亮度/AE 感知帧率策略。 */
    MediaGatewayLightFpsTargets targets; /* 目标帧率集合。 */
    MediaGatewayLightFpsTiming timing;   /* 评估、确认和切换节流参数。 */
    MediaGatewayLightFpsAeConfig ae;     /* AE 曝光/增益/亮度三态判定阈值。 */
} MediaGatewayLightFpsPolicyConfig;

typedef enum {
    MEDIA_GATEWAY_SCENE_NORMAL = 0,     /* 常规光照场景。 */
    MEDIA_GATEWAY_SCENE_LOW_LIGHT = 1,  /* 低照场景。 */
    MEDIA_GATEWAY_SCENE_BRIGHT = 2      /* 亮光/低延迟场景。 */
} MediaGatewaySceneState;

typedef enum {
    MEDIA_GATEWAY_NETWORK_GOOD = 0,      /* 网络良好。 */
    MEDIA_GATEWAY_NETWORK_NORMAL = 1,    /* 网络一般。 */
    MEDIA_GATEWAY_NETWORK_BAD = 2,       /* 网络较差。 */
    MEDIA_GATEWAY_NETWORK_VERY_BAD = 3   /* 网络很差。 */
} MediaGatewayNetworkState;

typedef struct {
    int max_fps;                      /* 当前网络等级允许的最高帧率。 */
    int bitrate_percent;              /* 当前网络等级码率系数，百分比。 */
    int pacing_percent;               /* 当前网络等级 RTP pacing 系数，百分比。 */
    int min_pacing_rate_bps;          /* 当前网络等级 RTP pacing 最小发送速率，单位 bit/s。 */
    int keyframe_interval_ms;         /* 当前网络等级关键帧时间间隔，单位毫秒。 */
} MediaGatewayNetworkLevelConfig;

typedef struct {
    uint8_t min_fraction_lost;        /* 进入当前网络等级的最小 RTCP fraction lost，0 表示不按丢包判定。 */
    uint32_t min_rtt_ms;              /* 进入当前网络等级的最小 RTT，单位毫秒，0 表示不按 RTT 判定。 */
    uint32_t min_jitter_ms;           /* 进入当前网络等级的最小 RTCP jitter，单位毫秒，0 表示不按 jitter 判定。 */
    int min_queue_depth;              /* 进入当前网络等级的最小输出队列深度，0 表示不按队列深度判定。 */
} MediaGatewayNetworkDetectLevelConfig;

typedef struct {
    MediaGatewayNetworkDetectLevelConfig normal;   /* NORMAL 网络等级判定阈值。 */
    MediaGatewayNetworkDetectLevelConfig bad;      /* BAD 网络等级判定阈值。 */
    MediaGatewayNetworkDetectLevelConfig very_bad; /* VERY_BAD 网络等级判定阈值。 */
} MediaGatewayNetworkDetectorConfig;

typedef struct {
    MediaGatewayNetworkLevelConfig good;     /* GOOD 网络等级的编码和 pacing 约束。 */
    MediaGatewayNetworkLevelConfig normal;   /* NORMAL 网络等级的编码和 pacing 约束。 */
    MediaGatewayNetworkLevelConfig bad;      /* BAD 网络等级的编码和 pacing 约束。 */
    MediaGatewayNetworkLevelConfig very_bad; /* VERY_BAD 网络等级的编码和 pacing 约束。 */
} MediaGatewayNetworkActionConfig;

typedef struct {
    MediaGatewayNetworkDetectorConfig detector; /* 网络等级判定阈值配置。 */
    MediaGatewayNetworkActionConfig action;     /* 网络等级对应的编码和 RTP pacing 输出动作。 */
} MediaGatewayNetworkAdaptiveConfig;

typedef struct {
    int enabled;                      /* 是否启用 RTCP/队列反馈驱动的网络自适应编码参数控制。 */
    int pacing_enabled;               /* 是否启用 RTCP/队列反馈生成的 RTP pacing 下发，可独立关闭用于定位卡顿来源。 */
    int pacing_update_interval_ms;    /* pacing 下发最小间隔，避免频繁重置 RTSP pacer 节奏。 */
    int pacing_update_change_percent; /* pacing rate 变化超过该百分比时允许提前下发。 */
    int network_downgrade_confirm_count; /* 网络状态降级需要连续命中的次数。 */
    int network_upgrade_confirm_count;   /* 网络状态升级需要连续命中的次数。 */
    int base_fps;                     /* 码率按帧率缩放时使用的基准帧率。 */
    int min_bitrate;                  /* 自适应控制允许的最低目标码率，bit/s。 */
    int max_bitrate;                  /* 自适应控制允许的最高目标码率，bit/s。 */
    MediaGatewayNetworkAdaptiveConfig network; /* 网络状态分级和约束配置。 */
} MediaGatewayNetworkEncodePolicyConfig;

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
    const char *device_name;         /* ALSA 采集设备，例如 default/hw:0,0。 */
    int sample_rate;                 /* 音频采样率，G711 常用 8000，Opus/WebRTC 使用 48000。 */
    int channels;                    /* ALSA 硬件采集声道数，不等同于编码输出声道数。 */
    AudioSampleFormat format;        /* 采集 PCM 格式，当前支持 S16LE。 */
    int period_frames;               /* 每个采集周期的 PCM frame 数；每个 frame 包含所有采集声道。 */
    int buffer_periods;              /* ALSA 设备缓冲周期数。 */
} MediaGatewayAudioCaptureConfig;

typedef struct {
    int source_slots;                /* audioFrameSource ring buffer 槽位数。 */
    int retry_ms;                    /* 音频采集失败后的重试间隔。 */
    int max_consecutive_failures;    /* 连续音频采集失败阈值。 */
} MediaGatewayAudioRuntimeConfig;

typedef struct {
    int bitrate;                     /* AAC 目标码率，单位 bit/s。 */
    int profile;                     /* AAC object type，2 表示 AAC-LC。 */
} MediaGatewayAudioAacConfig;

typedef struct {
    int bitrate;                     /* Opus 目标码率，单位 bit/s。 */
    int complexity;                  /* Opus 编码复杂度 0..10。 */
    int vbr;                         /* 是否启用 Opus VBR。 */
    int fec;                         /* 是否启用 Opus 带内 FEC。 */
    int dtx;                         /* 是否启用 Opus DTX。 */
    int packet_loss_percent;         /* Opus 预期丢包率 0..100。 */
} MediaGatewayAudioOpusConfig;

typedef struct {
    MediaCodecType codec;            /* 音频编码格式：G711A/G711U/AAC/OPUS。 */
    int sample_rate;                 /* 编码输出采样率；可低于 ALSA 采集率。 */
    int channels;                    /* 编码输入及码流声道数，与 ALSA 采集声道数独立配置。 */
    int input_channel;               /* 采集多声道转编码单声道时选择的输入声道，下标从 0 开始。 */
    MediaGatewayAudioAacConfig aac;
    MediaGatewayAudioOpusConfig opus;
} MediaGatewayAudioEncoderConfig;

typedef struct {
    char name[MEDIA_GATEWAY_AUDIO_ENCODER_GROUP_NAME_SIZE]; /* 配置中引用的稳定编码组名称。 */
    MediaGatewayAudioEncoderConfig encoder;                 /* 该名称对应的编码格式和参数。 */
} MediaGatewayAudioEncoderGroupConfig;

typedef struct {
    int enabled;                     /* 是否启用音频采集与编码。 */
    MediaGatewayAudioCaptureConfig capture; /* ALSA 设备及 PCM 帧参数。 */
    MediaGatewayAudioRuntimeConfig runtime; /* 帧源队列和失败重试参数。 */
    int encoder_group_count;        /* 配置的具名音频编码组数量。 */
    MediaGatewayAudioEncoderGroupConfig encoder_groups[MEDIA_GATEWAY_MAX_AUDIO_ENCODER_GROUPS];
} AudioSourceConfig;

typedef struct {
    const char *encoder_group;       /* 输出引用的具名音频编码配置；空字符串表示不发送音频。 */
} MediaGatewayOutputAudioBinding;

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
    MediaVideoEncodeParams low_light; /* 低照场景使用的完整编码参数档位。 */
    MediaVideoEncodeParams normal;    /* 常规场景使用的完整编码参数档位。 */
    MediaVideoEncodeParams bright;    /* 亮光/低延迟场景使用的完整编码参数档位。 */
} MediaGatewayDynamicProfiles;

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
    MediaGatewayDynamicProfiles dynamic_profiles; /* 每路码流按场景切换的完整编码参数档位。 */

    int enable_rtsp;                 /* 该码流是否启用 RTSP 输出。 */
    int enable_rtmp;                 /* 该码流是否启用 RTMP 输出。 */
    int enable_gb28181;              /* 该码流是否启用 GB28181 输出。 */
    int enable_webrtc;               /* 该码流是否启用 WebRTC 浏览器输出。 */
    MediaOutputRtspConfig rtsp;      /* 该码流 RTSP 输出配置。 */
    MediaOutputRtmpConfig rtmp;      /* 该码流 RTMP 输出配置。 */
    MediaOutputGb28181Config gb28181;/* 该码流 GB28181 输出配置。 */
    MediaOutputWebRtcConfig webrtc;  /* 该码流 WebRTC 输出配置。 */
    MediaGatewayOutputAudioBinding rtsp_audio;   /* RTSP 到音频编码组的显式绑定。 */
    MediaGatewayOutputAudioBinding rtmp_audio;   /* RTMP 到音频编码组的显式绑定。 */
    MediaGatewayOutputAudioBinding gb28181_audio;/* GB28181 到音频编码组的显式绑定。 */
    MediaGatewayOutputAudioBinding webrtc_audio; /* WebRTC 到音频编码组的显式绑定。 */
} MediaGatewayStreamConfig;

typedef struct {
    int enable_rtsp;                 /* 是否启用 RTSP 输出链路。 */
    int enable_rtmp;                 /* 是否启用 RTMP 输出链路。 */
    int enable_gb28181;              /* 是否启用 GB28181 设备输出链路。 */
    int enable_webrtc;               /* 是否启用 WebRTC 浏览器输出链路。 */
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
    MediaGatewayRuntimeConfig runtime; /* 网关运行期公共配置。 */
    MediaGatewayRecordConfig record;  /* 本地录像配置。 */
    MediaGatewayBenchConfig bench;    /* 性能埋点配置。 */
    MediaGatewayLogConfig log;        /* 日志配置。 */
} MediaGatewaySystemConfig;

typedef struct {
    int capture_source_count;        /* 采集源数量。 */
    CaptureSourceConfig capture_sources[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 采集源配置。 */
    IspSourceConfig isp;             /* RKAIQ/ISP 初始化与生命周期配置。 */
} MediaGatewayInputConfig;

typedef struct {
    MediaGatewayEncodeDefaultConfig encode; /* 顶层编码默认值；多码流配置以 streams[i] 为准。 */
    int stream_count;                /* 流配置数量，<=0 表示不初始化视频流。 */
    MediaGatewayStreamConfig streams[MEDIA_GATEWAY_MAX_STREAMS]; /* 多路码流配置。 */
} MediaGatewayVideoConfig;

typedef struct {
    AudioSourceConfig source;        /* 音频采集与编码配置。 */
} MediaGatewayAudioConfig;

typedef struct {
    MediaGatewayOutputSwitchConfig switches; /* 顶层输出开关配置；多码流配置以 streams[i] 为准。 */
    MediaOutputRtspConfig rtsp;      /* RTSP 协议专用配置块。 */
    MediaOutputRtmpConfig rtmp;      /* RTMP 协议专用配置块。 */
    MediaOutputGb28181Config gb28181;/* GB28181/SIP+RTP 协议专用配置块。 */
    MediaOutputWebRtcConfig webrtc;  /* WebRTC 协议专用配置块。 */
} MediaGatewayOutputConfig;

typedef struct {
    MediaGatewayLightFpsPolicyConfig light_fps; /* 环境亮度调帧率策略配置，可独立开关。 */
    MediaGatewayNetworkEncodePolicyConfig network_encode; /* RTCP/队列反馈调编码参数策略配置，可独立开关。 */
} MediaGatewayPolicyConfig;

typedef struct {
    MediaGatewaySystemConfig system; /* 运行期、日志、统计和录像配置。 */
    MediaGatewayInputConfig input;   /* 视频采集源与 ISP 配置。 */
    MediaGatewayVideoConfig video;   /* 视频编码默认值与多码流配置。 */
    MediaGatewayAudioConfig audio;   /* 音频采集与编码配置。 */
    MediaGatewayOutputConfig output; /* 输出协议开关与协议默认配置。 */
    MediaGatewayPolicyConfig policy; /* 运行期自适应策略配置。 */
} MediaGatewayConfig;

typedef struct {
    uint64_t frames;                                       /* 当前统计窗口内累计视频帧数。 */
    uint64_t bytes;                                        /* 当前统计窗口内累计视频字节数。 */
} MediaGatewayStreamStats;

typedef struct {
    uint64_t frames;                                       /* 当前统计窗口内累计音频帧数。 */
    uint64_t bytes;                                        /* 当前统计窗口内累计音频字节数。 */
} MediaGatewayAudioStats;

typedef struct {
    char name[MEDIA_GATEWAY_AUDIO_ENCODER_GROUP_NAME_SIZE]; /* 第一个映射到该运行时组的配置名称。 */
    AudioEncoderRuntimeGroupId group_id;                   /* 去重后的运行时编码组 ID。 */
    MediaCodecType codec;                                  /* 该运行时组输出的编码格式。 */
    int sample_rate;                                       /* 该运行时组输出采样率。 */
    int channels;                                          /* 该运行时组输出声道数。 */
    uint64_t input_frames;                                 /* 累计送入该组的 PCM 帧数。 */
    uint64_t encoded_packets;                              /* 累计产生的非空编码包数。 */
    uint64_t empty_outputs;                                /* 编码成功但暂未产生输出的次数，例如 AAC 缓存输入。 */
    uint64_t encode_failures;                              /* 累计编码或 PCM 适配失败次数。 */
    uint64_t encoded_bytes;                                /* 累计编码输出字节数。 */
    uint64_t encode_total_us;                              /* 累计编码耗时，包含 PCM 适配。 */
    uint64_t encode_max_us;                                /* 单次最大编码耗时。 */
} MediaGatewayAudioEncoderGroupStats;

typedef struct {
    double fps;                                            /* 当前统计窗口内的平均帧率。 */
    uint64_t bytes;                                        /* 当前统计窗口内累计视频字节数。 */
} MediaGatewayStreamStatsSnapshot;

typedef struct {
    MediaGatewayStreamStatsSnapshot streams[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流当前统计窗口快照。 */
} MediaGatewayStatsSnapshot;

typedef struct {
    uint64_t last_ts_us;                                   /* 上次吞吐统计输出时间戳。 */
    MediaGatewayStreamStats streams[MEDIA_GATEWAY_MAX_STREAMS]; /* 各码流当前统计窗口。 */
    MediaGatewayAudioStats audio;                          /* 音频当前统计窗口。 */
    size_t audio_group_count;                              /* 去重后的运行时音频编码组数量。 */
    MediaGatewayAudioEncoderGroupStats audio_groups[MEDIA_GATEWAY_MAX_AUDIO_ENCODER_GROUPS];
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
    int manual_override;                                  /* shell 手动固定帧率标志；置 1 时暂停 AE 自动动态帧率策略。 */
    char reason[128];                                     /* 最近一次策略决策原因。 */
} MediaLightFpsState;

typedef struct {
    MediaGatewaySceneState state; /* 最近一次场景状态判定。 */
    int max_fps;                  /* 场景侧给出的帧率上限。 */
} MediaAdaptSceneState;

typedef struct {
    MediaGatewayNetworkState state;     /* 最近一次网络状态判定。 */
    int max_fps;                        /* 网络侧给出的帧率上限。 */
    int max_output_queue_depth;         /* 最近一次评估看到的最大输出队列深度。 */
    uint8_t rtcp_fraction_lost;         /* 最近一次 RTCP RR fraction lost。 */
    uint32_t rtcp_jitter;               /* 最近一次 RTCP RR jitter，原始 RTP timestamp tick。 */
    uint32_t rtcp_jitter_ms;            /* 最近一次 RTCP RR jitter 换算后的毫秒值，用于网络状态判定。 */
    uint32_t rtcp_rtt_ms;               /* 最近一次 RTCP RR RTT，单位毫秒。 */
    uint64_t last_rtcp_feedback_ts_us;  /* 最近一次收到 RTCP 网络反馈的时间。 */
    MediaGatewayNetworkState pending_state; /* 防抖候选网络状态。 */
    int pending_state_count;            /* 候选网络状态连续命中次数。 */
} MediaAdaptNetworkState;

typedef struct {
    int enabled;                    /* 上次实际下发到输出通道的 pacer 开关。 */
    int rate_bps;                   /* 上次实际下发到输出通道的 pacing rate。 */
    uint64_t apply_ts_us;           /* 上次下发 pacing 配置的时间。 */
} MediaAdaptPacingApplyState;

typedef struct {
    int target_fps;               /* min(scene.max_fps, network.max_fps) 后的最终帧率。 */
    int pacing_rate_bps[MEDIA_GATEWAY_MAX_STREAMS]; /* 每路码流根据目标码率和网络状态计算出的 pacing rate。 */
    MediaAdaptPacingApplyState output_pacing[MEDIA_GATEWAY_MAX_OUTPUTS]; /* 每个输出通道的 pacing 下发节流状态。 */
    uint64_t last_decision_ts_us; /* 最近一次统一控制器决策时间。 */
    char reason[192];             /* 最近一次统一控制器决策原因。 */
} MediaAdaptOutputState;

typedef struct {
    MediaVideoEncodeParams base[MEDIA_GATEWAY_MAX_STREAMS];   /* 每路编码器自适应计算基准参数。 */
    MediaVideoEncodeParams target[MEDIA_GATEWAY_MAX_STREAMS]; /* 每路编码器最终目标参数。 */
} MediaAdaptEncodeParamsState;

typedef struct {
    MediaAdaptSceneState scene;          /* 场景侧约束状态。 */
    MediaAdaptNetworkState stream_network[MEDIA_GATEWAY_MAX_STREAMS]; /* 每路码流独立的网络反馈和网络侧约束状态。 */
    MediaAdaptNetworkState aggregate_network; /* 多路码流融合后的网络约束状态，用于统一 fps 决策和日志；单 sensor 下如需主/子码流不同帧率，应在编码/输出前增加按流抽帧。 */
    MediaAdaptOutputState output;        /* 联合控制最终输出状态。 */
    MediaAdaptEncodeParamsState encode_params; /* 每路编码器基准参数和目标参数。 */
} MediaAdaptCtrlState;

/** @description: 单个视频采集源的运行时资源和初始化状态。 */
typedef struct {
    V4L2CaptureCtx capture; /* V4L2 采集上下文。 */
    int ready;              /* 采集源是否已初始化成功。 */
} MediaGatewayVideoCaptureRuntime;

/** @description: 单个视频码流的编码资源、缓存和输出索引。 */
typedef struct {
    MppEncoderCtx encoder;          /* 该码流的 MPP 视频编码器。 */
    int enabled;                    /* 该码流是否启用。 */
    int encoder_ready;              /* 视频编码器是否已初始化成功。 */
    uint8_t *scaled_frame_cache;    /* 缩放后的 NV12 帧缓存。 */
    size_t scaled_frame_cache_size; /* 缩放帧缓存容量，单位字节。 */
    int rtsp_output_index;          /* 该码流绑定的 RTSP 输出索引，-1 表示未绑定。 */
    int gb28181_output_index;       /* 该码流绑定的 GB28181 输出索引，-1 表示未绑定。 */
} MediaGatewayVideoStreamRuntime;

/** @description: 视频采集、ISP 和各码流编码的运行时状态。 */
typedef struct {
    MediaGatewayVideoCaptureRuntime captures[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES];
    MediaGatewayVideoStreamRuntime streams[MEDIA_GATEWAY_MAX_STREAMS];
    IspControllerCtx isp; /* RKAIQ/ISP 控制上下文。 */
    int isp_ready;        /* ISP 控制链路是否已启动。 */
} MediaGatewayVideoRuntime;

/** @description: 音频采集及去重编码组的运行时状态。 */
typedef struct {
    AudioCaptureCtx capture; /* ALSA 音频采集上下文。 */
    int capture_ready;       /* 音频采集是否已初始化成功。 */
    AudioEncoderManagerHandle *encoder_manager; /* 统一音频编码管理器。 */
    int encoder_ready;       /* 至少一个音频编码组是否已初始化成功。 */
    AudioEncoderRuntimeGroupId encoder_group_ids[MEDIA_GATEWAY_MAX_AUDIO_ENCODER_GROUPS]; /* 配置组下标到去重运行时编码组 ID 的映射；未创建的配置组为 AUDIO_ENCODER_INVALID_GROUP_ID。 */
} MediaGatewayAudioRuntime;

/** @description: 单个协议输出通道及其视频、音频路由信息。 */
typedef struct {
    MediaOutput output;                              /* 协议输出通道实例。 */
    int stream_index;                                /* 绑定的视频码流下标。 */
    int audio_encoder_config_index;                  /* 音频配置组下标，-1 表示无音频。 */
    AudioEncoderRuntimeGroupId audio_encoder_group_id; /* 去重后的音频编码组 ID。 */
} MediaGatewayOutputChannelRuntime;

/** @description: 全部协议输出通道及共享网络反馈入口。 */
typedef struct {
    MediaGatewayOutputChannelRuntime channels[MEDIA_GATEWAY_MAX_OUTPUTS];
    int count;                               /* 当前已启用的输出通道数量。 */
    NetFeedbackCbInfo network_feedback_holder; /* 输出层网络反馈回调入口。 */
} MediaGatewayOutputRuntime;

/** @description: 网关吞吐统计、性能埋点及其同步状态。 */
typedef struct {
    pthread_mutex_t lock;          /* 保护吞吐、benchmark 和本地录像写入。 */
    int lock_ready;                /* lock 是否已初始化。 */
    MediaGatewayStats throughput;  /* 吞吐统计窗口。 */
    MediaGatewayBenchmarkStats benchmark; /* benchmark 埋点配置与统计窗口。 */
} MediaGatewayMetricsRuntime;

/** @description: 低照度、动态帧率和网络自适应策略的运行时状态。 */
typedef struct {
    int low_light_bitrate_active;    /* 低照度编码联动策略是否已应用。 */
    uint64_t last_isp_policy_ts_us;  /* 上次运行 ISP/低照度策略的时间戳。 */
    MediaLightFpsState light_fps;    /* 亮度/AE 感知帧率策略状态。 */
    MediaAdaptCtrlState adaptive;    /* 场景与网络联合自适应状态。 */
} MediaGatewayPolicyRuntime;

typedef struct {
    MediaGatewayConfig config;          /* 启动配置及归一化后的有效参数。 */
    MediaGatewayVideoRuntime video;     /* 视频采集、ISP 和码流编码状态。 */
    MediaGatewayAudioRuntime audio;     /* 音频采集和编码组状态。 */
    MediaGatewayOutputRuntime output;   /* 协议输出通道及其路由状态。 */
    MediaGatewayMetricsRuntime metrics; /* 吞吐统计和 benchmark 状态。 */
    MediaGatewayPolicyRuntime policy;   /* 场景和网络自适应策略状态。 */
    int running;                        /* 主循环是否正在运行。 */
    FILE *record_fp;                    /* 本地录像文件句柄。 */
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
 * @description: 读取当前统计窗口快照。
 */
void media_gateway_get_stats_snapshot(MediaGatewayCtx *ctx, MediaGatewayStatsSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif




