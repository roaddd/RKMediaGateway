#ifndef __RTMP_SINK_H__
#define __RTMP_SINK_H__

#include "mediaSink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;          /* sink 名称，用于日志和统计信息。 */
    const char *publish_url;   /* RTMP 推流地址，例如 rtmp://host/app/stream。 */
    int queue_capacity;        /* 该 sink 独立的发送队列容量。 */
    int reconnect_interval_ms; /* 连接失败后的重连间隔，单位毫秒。 */
    int connect_timeout_ms;    /* 建立 RTMP 连接的超时时间，单位毫秒。 */
    int audio_enabled;         /* 音频通路预留开关，当前主要用于元数据描述。 */
    int video_width;           /* 元数据中的视频宽度。 */
    int video_height;          /* 元数据中的视频高度。 */
    int video_fps;             /* 元数据中的视频帧率。 */
    int video_bitrate;         /* 元数据中的视频码率。 */
    const char *video_codec_name; /* 元数据中的视频编码名称。 */
    const char *encoder_name;     /* 元数据中的编码器名称。 */
} RtmpSinkConfig;

int rtmp_sink_setup(MediaSink *sink, const RtmpSinkConfig *config);

#ifdef __cplusplus
}
#endif

#endif