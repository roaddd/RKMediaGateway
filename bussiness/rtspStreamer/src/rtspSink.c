#include "rtspSink.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtsp_server_api.h"

#define DEFAULT_RTSP_SINK_NAME "rtsp"
#define DEFAULT_RTSP_SESSION "live"
#define DEFAULT_RTSP_IP "0.0.0.0"
#define DEFAULT_RTSP_PORT 8554
#define DEFAULT_RTSP_USER "admin"
#define DEFAULT_RTSP_PASSWORD "123456"

typedef struct {
    RtspSinkConfig config;    /* RTSP sink 自身配置的本地副本，避免依赖外部配置对象生命周期。 */
    void *session;            /* rtspAddSession() 创建的推流会话句柄，发送视频数据时会用到。 */
    pthread_t server_thread;  /* 后台 RTSP 服务线程，内部会阻塞在服务循环/accept。 */
    int server_started;       /* 服务线程是否已经成功创建，便于 stop 时决定是否 join。 */
    int server_result;        /* rtspStartServer() 的返回结果，便于后续排查服务启动失败原因。 */
    int module_ready;         /* RTSP 模块是否已初始化成功，避免重复释放底层模块资源。 */
} RtspSinkImpl;

/**
 * @description: 判断当前位置是否为 H264 起始码并返回长度
 * @param {const uint8_t *} data
 * @param {size_t} len
 * @return {static int}
 */
static int start_code_len(const uint8_t *data, size_t len) {
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        return 4;
    }
    if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return 3;
    }
    return 0;
}

/**
 * @description: 查找下一个 H264 起始码位置
 * @param {const uint8_t *} data
 * @param {size_t} len
 * @param {size_t} offset
 * @param {size_t *} pos
 * @param {int *} code_len
 * @return {static int}
 */
static int find_start_code(const uint8_t *data, size_t len, size_t offset, size_t *pos, int *code_len) {
    size_t i;

    for (i = offset; i + 3 <= len; ++i) {
        int cur_len = start_code_len(data + i, len - i);
        if (cur_len > 0) {
            *pos = i;
            *code_len = cur_len;
            return 0;
        }
    }
    return -1;
}

/**
 * @description: 将 Annex-B 码流按 NALU 发送到 RTSP 会话
 * @param {void *} session
 * @param {const uint8_t *} data
 * @param {size_t} len
 * @return {static int}
 */
static int rtsp_send_annexb(void *session, const uint8_t *data, size_t len) {
    size_t nalu_start = 0;
    int code_len = 0;

    if (find_start_code(data, len, 0, &nalu_start, &code_len) != 0) {
        return sessionSendVideoData(session, (unsigned char *)data, (int)len);
    }

    while (nalu_start < len) {
        size_t payload_start = nalu_start + (size_t)code_len;
        size_t next_start = len;
        int next_code_len = 0;

        if (payload_start >= len) {
            break;
        }

        find_start_code(data, len, payload_start, &next_start, &next_code_len);
        if (next_start > payload_start) {
            if (sessionSendVideoData(session,
                                     (unsigned char *)(data + payload_start),
                                     (int)(next_start - payload_start)) < 0) {
                return -1;
            }
        }
        if (next_start >= len) {
            break;
        }
        nalu_start = next_start;
        code_len = next_code_len;
    }

    return 0;
}

/**
 * @description: RTSP 服务线程入口函数
 * @param {void *} arg
 * @return {static void *}
 */
static void *rtsp_server_thread(void *arg) {
    RtspSinkImpl *impl = (RtspSinkImpl *)arg;
    /* 线程直接复用 impl 里的认证、监听地址和端口配置来启动 RTSP 服务。 */
    impl->server_result = rtspStartServer(impl->config.auth_enable,
                                          impl->config.server_ip,
                                          impl->config.server_port,
                                          impl->config.user,
                                          impl->config.password);
    return NULL;
}

/**
 * @description: 启动 RTSP 推流通道
 * @param {MediaSink *} sink
 * @return {static int}
 */
static int rtsp_sink_start(MediaSink *sink) {
    RtspSinkImpl *impl = (RtspSinkImpl *)sink->impl;

    /* 启动顺序：先初始化模块，再创建 session，最后拉起服务线程。 */
    if (rtspModuleInit() < 0) {
        return -1;
    }
    impl->module_ready = 1; /* 标记模块已就绪，stop() 时据此决定是否调用 rtspModuleDel。 */

    /* session 保存在 impl->session 中，后续每帧发送都通过这个句柄写入。 */
    impl->session = rtspAddSession(impl->config.session_name);
    if (!impl->session) {
        return -1;
    }
    if (sessionAddVideo(impl->session, VIDEO_H264) < 0) {
        return -1;
    }
    /* 启动rtsp推流服务 */
    if (pthread_create(&impl->server_thread, NULL, rtsp_server_thread, impl) != 0) {
        return -1;
    }
        
    impl->server_started = 1; /* 只有线程创建成功后才置位，避免 stop() 中错误 join。 */
    printf("[INFO] RTSP sink ready: rtsp://%s:%d/%s\n",
           impl->config.server_ip,
           impl->config.server_port,
           impl->config.session_name);
    return 0;
}

/**
 * @description: 检查 RTSP 推流通道是否可用
 * @param {MediaSink *} sink
 * @return {static int}
 */
static int rtsp_sink_connect(MediaSink *sink) {
    RtspSinkImpl *impl = (RtspSinkImpl *)sink->impl;
    return impl->session ? 0 : -1;
}

/**
 * @description: 发送一个媒体包到 RTSP 通道
 * @param {MediaSink *} sink
 * @param {const MediaPacket *} packet
 * @return {static int}
 */
static int rtsp_sink_send_packet(MediaSink *sink, const MediaPacket *packet) {
    RtspSinkImpl *impl = (RtspSinkImpl *)sink->impl;

    if (!impl || !impl->session || !packet || !packet->buffer) {
        return -1;
    }
    return rtsp_send_annexb(impl->session, packet->buffer->data, packet->buffer->size);
}

/**
 * @description: 断开 RTSP 推流通道
 * @param {MediaSink *} sink
 * @return {static void}
 */
static void rtsp_sink_disconnect(MediaSink *sink) {
    (void)sink;
}

/**
 * @description: 停止 RTSP 推流通道
 * @param {MediaSink *} sink
 * @return {static void}
 */
static void rtsp_sink_stop(MediaSink *sink) {
    RtspSinkImpl *impl = (RtspSinkImpl *)sink->impl;

    if (!impl) {
        return;
    }

    /* 先请求服务线程退出，再按 server_started/module_ready/session 标记逆序清理。 */
    rtspStopServer();
    if (impl->server_started) {
        pthread_join(impl->server_thread, NULL);
        impl->server_started = 0;
    }
    if (impl->session) {
        rtspDelSession(impl->session);
        impl->session = NULL;
    }
    if (impl->module_ready) {
        rtspModuleDel();
        impl->module_ready = 0;
    }
}

/**
 * @description: 根据配置创建 RTSP 推流通道
 * @param {MediaSink *} sink
 * @param {const RtspSinkConfig *} config
 * @return {int}
 */
int rtsp_sink_setup(MediaSink *sink, const RtspSinkConfig *config) {
    static const MediaSinkVTable vtable = {
        rtsp_sink_start,
        rtsp_sink_connect,
        rtsp_sink_send_packet,
        rtsp_sink_disconnect,
        rtsp_sink_stop
    };
    MediaSinkConfig sink_config;
    RtspSinkImpl *impl;

    if (!sink) {
        return -1;
    }

    /* impl 是 RTSP sink 的私有状态，整个 sink 生命周期内一直挂在 sink->impl 上。 */
    impl = (RtspSinkImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        return -1;
    }

    if (config) {
        impl->config = *config;
    }
    if (!impl->config.name) {
        impl->config.name = DEFAULT_RTSP_SINK_NAME;
    }
    if (!impl->config.session_name) {
        impl->config.session_name = DEFAULT_RTSP_SESSION;
    }
    if (!impl->config.server_ip) {
        impl->config.server_ip = DEFAULT_RTSP_IP;
    }
    if (impl->config.server_port <= 0) {
        impl->config.server_port = DEFAULT_RTSP_PORT;
    }
    if (!impl->config.user) {
        impl->config.user = DEFAULT_RTSP_USER;
    }
    if (!impl->config.password) {
        impl->config.password = DEFAULT_RTSP_PASSWORD;
    }

    memset(&sink_config, 0, sizeof(sink_config));
    sink_config.name = impl->config.name;
    sink_config.queue_capacity = (impl->config.queue_capacity > 0) ? impl->config.queue_capacity : 32;
    sink_config.reconnect_interval_ms = 1000;
    sink_config.drop_until_keyframe_after_reconnect = 0;

    if (media_sink_init(sink, &sink_config, &vtable, impl) != 0) {
        free(impl);
        return -1;
    }
    return 0;
}

