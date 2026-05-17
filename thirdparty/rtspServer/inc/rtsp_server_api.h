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
 * RKMediaGateway 传入的一帧编码数据。
 * data/data_len 指向完整帧缓存。pts_us 是媒体 PTS，单位微秒；
 * rtsp-server 会在内部换算成 RTP timestamp。
 */
typedef struct RtspMediaFrame {
    uint8_t *data;
    int data_len;
    uint64_t pts_us;
} RtspMediaFrame;
#endif

int rtspModuleInit(void);
void rtspModuleDel(void);

int rtspConfigSession(int file_reloop_flag, const char *mp4_file_path);

void* rtspAddSession(const char* session_name);
void rtspDelSession(void *context);

int rtspStartServer(int auth, const char *server_ip, int server_port, const char *user, const char *password);
void rtspStopServer(void);

int sessionAddVideo(void *context, enum VIDEO_e type);
int sessionAddAudio(void *context, enum AUDIO_e type, int profile, int sample_rate, int channels);

/* 整帧发送 API。调用前不要预先把 H264 拆成多个 NALU。 */
int sessionSendVideoFrame(void *context, const RtspMediaFrame *frame);
int sessionSendAudioFrame(void *context, const RtspMediaFrame *frame);
int rtspSessionGetClientNum(void *context);

#ifdef __cplusplus
}
#endif

#endif
