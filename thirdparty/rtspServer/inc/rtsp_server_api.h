/*** 
 * @Author: huangkelong
 * @Date: 2026-03-03 23:10:24
 * @LastEditTime: 2026-07-13 22:45:20
 * @LastEditors: huangkelong
 * @Description: rtsp server对外头文件
 * @FilePath: \Fork\RKMediaGateway\thirdparty\rtspServer\inc\rtsp_server_api.h
 * @可以输入预定的版权声明、个性签名、空行等
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

/*** 
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
 * 设置指定 session 的视频 RTP 包级 pacing 码率。
 * pacing_rate_bps <= 0 表示关闭；音频 RTP 不受该接口影响。
 */
int rtspSetSessionVideoPacingRate(void *context, int pacing_rate_bps);

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
