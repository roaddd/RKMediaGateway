#ifndef __RTSP_SINK_H__
#define __RTSP_SINK_H__

#include "mediaSink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;         /* sink 名称，用于日志和统计信息。 */
    const char *session_name; /* RTSP URL 中的 session 名称。 */
    const char *server_ip;    /* RTSP 服务监听地址。 */
    int server_port;          /* RTSP 服务监听端口。 */
    int auth_enable;          /* 是否启用 RTSP 鉴权。 */
    const char *user;         /* 鉴权用户名。 */
    const char *password;     /* 鉴权密码。 */
    int queue_capacity;       /* 该 sink 自身的发送队列容量。 */
} RtspSinkConfig;

int rtsp_sink_setup(MediaSink *sink, const RtspSinkConfig *config);

#ifdef __cplusplus
}
#endif

#endif