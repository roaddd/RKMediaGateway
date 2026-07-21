/**
 * @file rtsp_server_api.h
 * @brief RTSP server 对外 C 接口。
 *
 * 上层通过本文件创建 RTSP session、添加音视频 track、发送编码帧，
 * 并接收 RTCP Receiver Report 网络反馈。
 */
#ifndef _RTSP_SERVER_API_H_
#define _RTSP_SERVER_API_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum VIDEO_e
{
    VIDEO_H264 = 1,
    VIDEO_H265,
    VIDEO_NONE,
};

enum AUDIO_e
{
    AUDIO_AAC = 1,
    AUDIO_PCMA,
    AUDIO_NONE,
};

#ifndef RTSP_VIDEO_PACER_MODE_DEFINED
#define RTSP_VIDEO_PACER_MODE_DEFINED
/*
 * RTSP session 视频 RTP pacer 开关状态。
 * 使用枚举替代裸 int，避免调用层把开关值和 pacing rate 混用。
 */
typedef enum {
    RTSP_VIDEO_PACER_DISABLED = 0, /* 关闭视频 RTP 包级 pacer。 */
    RTSP_VIDEO_PACER_ENABLED = 1   /* 开启视频 RTP 包级 pacer。 */
} RtspVideoPacerMode;
#endif

#ifndef RTSP_VIDEO_PACER_STATS_DEFINED
#define RTSP_VIDEO_PACER_STATS_DEFINED
#define RTSP_VIDEO_PACER_STATS_MAX_CLIENTS 16
typedef struct {
    int in_use;                    /* 客户端槽位是否正在使用。 */
    char client_ip[40];            /* 客户端 IP。 */
    int client_rtp_port;           /* 客户端视频 RTP 端口。 */
    int transport;                 /* RTSP transport 类型。 */
    int rate_bps;                  /* 当前客户端 pacer 目标码率。 */
    uint64_t packet_count;         /* pacer 观察到的视频 RTP 包数量。 */
    uint64_t byte_count;           /* pacer 观察到的视频 RTP 字节数。 */
    uint64_t sleep_count;          /* pacer 实际 sleep 次数。 */
    uint64_t total_sleep_us;       /* pacer 累计 sleep 时间。 */
    uint32_t last_packet_bytes;    /* 最近一次 RTP 包字节数。 */
    uint32_t last_sleep_us;        /* 最近一次 sleep 时间。 */
    uint32_t last_interval_us;     /* 最近一次按 rate 计算的发送间隔。 */
    uint64_t last_window_bps;      /* 最近完成的 100ms 窗口估算码率。 */
    uint64_t max_window_bps;       /* 已观察到的最大 100ms 窗口估算码率。 */
    uint64_t current_window_bps;   /* 当前未完成窗口按已用时间估算的码率。 */
    uint64_t last_window_bytes;    /* 最近完成窗口内 RTP 字节数。 */
    uint32_t last_window_packets;  /* 最近完成窗口内 RTP 包数。 */
    uint64_t last_window_elapsed_us; /* 最近完成窗口持续时间。 */
    uint64_t max_window_bytes;     /* 最大码率窗口内 RTP 字节数。 */
    uint32_t max_window_packets;   /* 最大码率窗口内 RTP 包数。 */
    uint64_t max_window_elapsed_us; /* 最大码率窗口持续时间。 */
    uint64_t reset_count;          /* pacer 时间基准被重置的次数。 */
    uint32_t last_reset_reason;    /* 最近一次重置原因：1=首次/未初始化，2=落后超过保护阈值。 */
    uint64_t last_reset_lag_us;    /* 最近一次落后重置时，当前时间超过计划发送时间的微秒数。 */
    uint64_t last_reset_now_us;    /* 最近一次重置发生时的单调时间。 */
    uint64_t last_reset_next_send_ts_us; /* 最近一次重置前的计划发送时间。 */
    uint64_t last_reset_window_bytes; /* 最近一次重置时当前统计窗口内已经发送的 RTP 字节数。 */
    uint32_t last_reset_window_packets; /* 最近一次重置时当前统计窗口内已经发送的 RTP 包数。 */
} RtspVideoPacerClientStats;

typedef struct {
    RtspVideoPacerMode mode;       /* 当前 session pacer 开关。 */
    int pacing_rate_bps;           /* 当前 session 目标 pacing rate。 */
    int total_client_count;        /* 当前 session 总客户端数量。 */
    int reported_client_count;     /* 本快照实际填充的客户端数量。 */
    RtspVideoPacerClientStats clients[RTSP_VIDEO_PACER_STATS_MAX_CLIENTS];
} RtspVideoPacerStats;
#endif

#ifndef RTSP_MEDIA_FRAME_DEFINED
#define RTSP_MEDIA_FRAME_DEFINED
/*
 * 上游媒体流水线传入的一帧编码数据。
 * data/data_len 指向完整帧缓存。H264 Annex-B 帧内可以包含多个 NALU；
 * RTSP server 负责在内部做 RTP 打包。
 * pts_us 是媒体显示时间戳，单位微秒，不是 RTP tick。
 */
typedef struct RtspMediaFrame {
    uint8_t *data;
    int data_len;
    uint64_t pts_us;
} RtspMediaFrame;
#endif

/* RTCP Receiver Report 解析后的网络反馈快照，由 RTSP server 通过回调通知上层。 */
typedef struct RtspRtcpReceiverReport {
    const char *session_name;     /* RTSP session 名称，用于上层把反馈归属到具体输出流。 */
    const char *client_ip;        /* 反馈来源客户端 IP。 */
    int is_audio;                 /* 0 表示视频 RTCP，1 表示音频 RTCP。 */
    uint8_t fraction_lost;        /* RTCP RR fraction lost，0~255 对应 0~100%。 */
    int32_t cumulative_lost;      /* RTCP RR 累计丢包数，24-bit signed 扩展到 int32。 */
    uint32_t jitter;              /* RTCP RR interarrival jitter，单位为对应 RTP 时钟 tick。 */
    uint32_t rtt_ms;              /* 根据 LSR/DLSR 估算出的 RTT，单位毫秒。 */
    uint64_t rr_count;            /* 当前客户端/媒体方向累计收到的 RR 数量。 */
} RtspRtcpReceiverReport;

/* RTCP RR 反馈回调。RTSP server 只上报指标，不参与上层自适应策略决策。 */
typedef void (*RtspRtcpReportCallback)(const RtspRtcpReceiverReport *report, void *userdata);

int rtspModuleInit(void);
void rtspModuleDel(void);

int rtspConfigSession(int file_reloop_flag, const char *mp4_file_path);

/**
 * @description: 在“已经启动的 RTSP server”里注册一条流路径
 * @param {char*} session_name
 * @return {*}
 */
void* rtspAddSession(const char* session_name);
void rtspDelSession(void *context);

int rtspStartServer(int auth, const char *server_ip, int server_port, const char *user, const char *password);
void rtspStopServer(void);
/* 注册 RTCP RR 反馈回调；传 NULL 可取消注册。 */
void rtspSetRtcpReportCallback(RtspRtcpReportCallback callback, void *userdata);

int sessionAddVideo(void *context, enum VIDEO_e type);
int sessionAddAudio(void *context, enum AUDIO_e type, int profile, int sample_rate, int channels);
/*
 * 设置指定 session 的视频 RTP 包级 pacer。
 * mode 为 RTSP_VIDEO_PACER_DISABLED 表示关闭 pacer，此时忽略 pacing_rate_bps；
 * mode 为 RTSP_VIDEO_PACER_ENABLED 表示开启 pacer，此时 pacing_rate_bps 必须大于 0。
 * 音频 RTP 不受该接口影响。
 */
int rtspSetSessionVideoPacer(void *context, RtspVideoPacerMode mode, int pacing_rate_bps);
int rtspGetSessionVideoPacerStats(void *context, RtspVideoPacerStats *stats);

/*
 * 整帧发送 API。server 会把 frame->pts_us 换算到对应媒体的 RTP 时钟域，
 * 并保证同一帧拆出的所有 RTP 包使用同一个 timestamp。
 */
int sessionSendVideoFrame(void *context, const RtspMediaFrame *frame);
int sessionSendAudioFrame(void *context, const RtspMediaFrame *frame);
int rtspSessionGetClientNum(void *context);

#ifdef __cplusplus
}
#endif

#endif
