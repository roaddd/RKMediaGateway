#ifndef __RTSP_STREAMER_H__
#define __RTSP_STREAMER_H__

#include "mppEncoder.h"
#include "v4l2Capture.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *session_name;          /* RTSP URL 中的 session 名称。 */
    const char *server_ip;             /* RTSP 服务监听地址。 */
    int server_port;                   /* RTSP 服务监听端口。 */
    int auth_enable;                   /* 是否启用 RTSP 鉴权。 */
    const char *user;                  /* RTSP 鉴权用户名。 */
    const char *password;              /* RTSP 鉴权密码。 */
    int fps;                           /* 编码帧率。 */
    int bitrate;                       /* 编码目标码率，单位 bit/s。 */
    int gop;                           /* GOP 长度，影响关键帧间隔和恢复速度。 */
    int rc_mode;                       /* 编码码率控制模式。 */
    int h264_profile;                  /* H264 profile 配置。 */
    int h264_level;                    /* H264 level 配置。 */
    int h264_cabac_en;                 /* 是否启用 CABAC。 */
    int low_latency_mode;              /* 低延时模式开关，主要影响日志和调试策略。 */
    int stats_interval_sec;            /* 统计信息输出周期，单位秒。 */
    int capture_retry_ms;              /* 采集失败后的重试间隔，单位毫秒。 */
    int max_consecutive_failures;      /* 连续失败达到该阈值时主循环退出。 */
    const char *record_file_path;      /* 本地录像文件路径，为空则不录制。 */
    int record_flush_interval_frames;  /* 本地录像每隔多少帧执行一次 fflush。 */
} RtspStreamerConfig;

typedef struct {
    V4L2CaptureCtx capture;        /* V4L2 采集模块上下文。 */
    MppEncoderCtx encoder;         /* MPP 编码模块上下文。 */
    void *rtsp_session;            /* 当前 RTSP 推流会话句柄。 */
    RtspStreamerConfig config;     /* 归一化后的 RTSP 推流配置副本。 */
    int capture_ready;             /* 采集模块是否已初始化成功。 */
    int encoder_ready;             /* 编码模块是否已初始化成功。 */
    int rtsp_module_ready;         /* RTSP 底层模块是否已初始化成功。 */
    int running;                   /* 主循环是否正在运行。 */
    FILE *record_fp;               /* 本地录像文件句柄。 */
    uint64_t stat_last_ts_us;      /* 上次统计输出时间戳。 */
    uint64_t stat_frames;          /* 当前统计窗口内累计帧数。 */
    uint64_t stat_bytes;           /* 当前统计窗口内累计字节数。 */
} RtspStreamerCtx;

int rtsp_streamer_init(RtspStreamerCtx *ctx, const RtspStreamerConfig *config);
int rtsp_streamer_run(RtspStreamerCtx *ctx);
void rtsp_streamer_stop(RtspStreamerCtx *ctx);
void rtsp_streamer_deinit(RtspStreamerCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif