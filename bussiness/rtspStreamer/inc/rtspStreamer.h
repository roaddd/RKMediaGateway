#ifndef __RTSP_STREAMER_H__
#define __RTSP_STREAMER_H__

#include "mppEncoder.h"
#include "v4l2Capture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *session_name;
    const char *server_ip;
    int server_port;
    int auth_enable;
    const char *user;
    const char *password;
    int fps;
    int bitrate;
    int gop;
} RtspStreamerConfig;

typedef struct {
    V4L2CaptureCtx capture;
    MppEncoderCtx encoder;
    void *rtsp_session;
    RtspStreamerConfig config;
    int capture_ready;
    int encoder_ready;
    int rtsp_module_ready;
    int running;
} RtspStreamerCtx;

int rtsp_streamer_init(RtspStreamerCtx *ctx, const RtspStreamerConfig *config);
int rtsp_streamer_run(RtspStreamerCtx *ctx);
void rtsp_streamer_stop(RtspStreamerCtx *ctx);
void rtsp_streamer_deinit(RtspStreamerCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
