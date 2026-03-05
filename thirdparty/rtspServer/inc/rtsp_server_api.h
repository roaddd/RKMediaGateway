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

int rtspModuleInit(void);
void rtspModuleDel(void);

int rtspConfigSession(int file_reloop_flag, const char *mp4_file_path);

void* rtspAddSession(const char* session_name);
void rtspDelSession(void *context);

int rtspStartServer(int auth, const char *server_ip, int server_port, const char *user, const char *password);
void rtspStopServer(void);

int sessionAddVideo(void *context, enum VIDEO_e type);
int sessionAddAudio(void *context, enum AUDIO_e type, int profile, int sample_rate, int channels);

int sessionSendVideoData(void *context, uint8_t *data, int data_len);
int sessionSendAudioData(void *context, uint8_t *data, int data_len);

#ifdef __cplusplus
}
#endif

#endif
