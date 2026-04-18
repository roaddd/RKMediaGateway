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
    int in_use;                    /* 共享 RTSP 服务是否已被启用。 */
    int module_ready;              /* rtspModuleInit 是否成功。 */
    int server_started;            /* 服务线程是否创建成功。 */
    int auth_enable;               /* 鉴权开关。 */
    int server_port;               /* 共享服务监听端口。 */
    char server_ip[64];            /* 共享服务监听地址。 */
    char user[64];                 /* 鉴权用户名。 */
    char password[64];             /* 鉴权密码。 */
    pthread_t server_thread;       /* 共享服务线程句柄。 */
} RtspSharedServer;

typedef struct {
    RtspSinkConfig config;         /* 当前 sink 的配置副本。 */
    void *session;                 /* 当前 sink 绑定的 RTSP session。 */
    int shared_server_acquired;    /* 是否已持有共享服务引用计数。 */
} RtspSinkImpl;

static RtspSharedServer g_rtsp_shared_server;
static pthread_mutex_t g_rtsp_shared_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_rtsp_shared_ref_count = 0;

/* 安全复制字符串到固定长度缓冲区，保证以 '\0' 结束。 */
static void copy_string_field(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* 比较两个字符串是否相等（支持 NULL 安全比较）。 */
static int string_equals(const char *a, const char *b) {
    if (!a && !b) {
        return 1;
    }
    if (!a || !b) {
        return 0;
    }
    return strcmp(a, b) == 0;
}

/* 判断当前位置是否为 H264 起始码，并返回起始码长度。 */
static int start_code_len(const uint8_t *data, size_t len) {
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        return 4;
    }
    if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return 3;
    }
    return 0;
}

/* 在缓冲区中从 offset 开始查找下一个 H264 起始码位置。 */
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

/* 将 Annex-B 数据按 NALU 切分后发送到 RTSP session。 */
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
            if (sessionSendVideoData(session, (unsigned char *)(data + payload_start), (int)(next_start - payload_start)) < 0) {
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

/* 共享 RTSP 服务线程入口：阻塞运行 rtspStartServer。 */
static void *shared_rtsp_server_thread(void *arg) {
    RtspSharedServer *shared = (RtspSharedServer *)arg;
    rtspStartServer(shared->auth_enable,
                    shared->server_ip,
                    shared->server_port,
                    shared->user,
                    shared->password);
    return NULL;
}

/* 校验新 sink 的 server 参数是否与共享 server 一致。 */
static int shared_rtsp_config_compatible(const RtspSinkConfig *cfg) {
    if (!cfg) {
        return 0;
    }
    if (g_rtsp_shared_server.auth_enable != cfg->auth_enable) {
        return 0;
    }
    if (g_rtsp_shared_server.server_port != cfg->server_port) {
        return 0;
    }
    if (!string_equals(g_rtsp_shared_server.server_ip, cfg->server_ip)) {
        return 0;
    }
    if (!string_equals(g_rtsp_shared_server.user, cfg->user)) {
        return 0;
    }
    if (!string_equals(g_rtsp_shared_server.password, cfg->password)) {
        return 0;
    }
    return 1;
}

/**
 * @description: 管理“共享 RTSP 服务器进程内实例”
 * 第一次调用：启动 RTSP 模块 + 起 server 线程（监听 ip:port）
 * 后续调用：不重复启动，只增加引用计数
 * 并检查新 sink 的 server 参数是否和已启动的一致（端口/鉴权等）
 * 确保 main/sub 共用同一个 8554 服务端
 * @param {RtspSinkConfig} *cfg
 * @return {*}
 */

static int shared_rtsp_server_acquire(const RtspSinkConfig *cfg) {
    int ret = 0;
    pthread_mutex_lock(&g_rtsp_shared_lock);

    if (g_rtsp_shared_ref_count == 0) {
        memset(&g_rtsp_shared_server, 0, sizeof(g_rtsp_shared_server));
        g_rtsp_shared_server.auth_enable = cfg->auth_enable;
        g_rtsp_shared_server.server_port = cfg->server_port;
        copy_string_field(g_rtsp_shared_server.server_ip, sizeof(g_rtsp_shared_server.server_ip), cfg->server_ip);
        copy_string_field(g_rtsp_shared_server.user, sizeof(g_rtsp_shared_server.user), cfg->user);
        copy_string_field(g_rtsp_shared_server.password, sizeof(g_rtsp_shared_server.password), cfg->password);
        /* 初始化rtspServer */
        if (rtspModuleInit() < 0) {
            pthread_mutex_unlock(&g_rtsp_shared_lock);
            return -1;
        }
        g_rtsp_shared_server.module_ready = 1;
        g_rtsp_shared_server.in_use = 1;
        /* 启动rtsp服务线程 */
        ret = pthread_create(&g_rtsp_shared_server.server_thread, NULL, shared_rtsp_server_thread, &g_rtsp_shared_server);
        if (ret != 0) {
            rtspModuleDel();
            memset(&g_rtsp_shared_server, 0, sizeof(g_rtsp_shared_server));
            pthread_mutex_unlock(&g_rtsp_shared_lock);
            return -1;
        }
        g_rtsp_shared_server.server_started = 1;
    } else if (!shared_rtsp_config_compatible(cfg)) {
        fprintf(stderr, "[ERROR] RTSP shared server config conflict: expect %s:%d auth=%d user=%s\n",
                g_rtsp_shared_server.server_ip,
                g_rtsp_shared_server.server_port,
                g_rtsp_shared_server.auth_enable,
                g_rtsp_shared_server.user);
        pthread_mutex_unlock(&g_rtsp_shared_lock);
        return -1;
    }

    g_rtsp_shared_ref_count++;
    pthread_mutex_unlock(&g_rtsp_shared_lock);
    return 0;
}

/* 释放共享 RTSP server：最后一个引用释放时才停止并反初始化。 */
static void shared_rtsp_server_release(void) {
    pthread_mutex_lock(&g_rtsp_shared_lock);

    if (g_rtsp_shared_ref_count <= 0) {
        pthread_mutex_unlock(&g_rtsp_shared_lock);
        return;
    }

    g_rtsp_shared_ref_count--;
    if (g_rtsp_shared_ref_count == 0) {
        rtspStopServer();
        if (g_rtsp_shared_server.server_started) {
            pthread_join(g_rtsp_shared_server.server_thread, NULL);
        }
        if (g_rtsp_shared_server.module_ready) {
            rtspModuleDel();
        }
        memset(&g_rtsp_shared_server, 0, sizeof(g_rtsp_shared_server));
    }

    pthread_mutex_unlock(&g_rtsp_shared_lock);
}

/* sink 启动：挂载共享 server，并创建当前 sink 的 session。 */
static int rtsp_sink_start(MediaSink *sink) {
    RtspSinkImpl *impl = (RtspSinkImpl *)sink->impl;

    if (shared_rtsp_server_acquire(&impl->config) != 0) {
        return -1;
    }
    impl->shared_server_acquired = 1;

    /* 创建 RTSP 会话 */
    impl->session = rtspAddSession(impl->config.session_name);
    if (!impl->session) {
        shared_rtsp_server_release();
        impl->shared_server_acquired = 0;
        return -1;
    }
    /* 当前session添加视频流 */
    if (sessionAddVideo(impl->session, VIDEO_H264) < 0) {
        rtspDelSession(impl->session);
        impl->session = NULL;
        shared_rtsp_server_release();
        impl->shared_server_acquired = 0;
        return -1;
    }

    printf("[INFO] RTSP sink ready: rtsp://%s:%d/%s\n",
           impl->config.server_ip,
           impl->config.server_port,
           impl->config.session_name);
    return 0;
}

/* sink 连接检查：session 已就绪即可视为可用。 */
static int rtsp_sink_connect(MediaSink *sink) {
    RtspSinkImpl *impl = (RtspSinkImpl *)sink->impl;
    return impl->session ? 0 : -1;
}

/* sink 发送：将 H264 包转为 RTSP 可发送单元。 */
static int rtsp_sink_send_packet(MediaSink *sink, const MediaPacket *packet) {
    RtspSinkImpl *impl = (RtspSinkImpl *)sink->impl;
    if (!impl || !impl->session || !packet || !packet->buffer) {
        return -1;
    }
    return rtsp_send_annexb(impl->session, packet->buffer->data, packet->buffer->size);
}

/* 当前实现无需主动断链，保留该钩子用于接口一致性。 */
static void rtsp_sink_disconnect(MediaSink *sink) {
    (void)sink;
}

/* sink 停止：先删 session，再释放共享 server 引用。 */
static void rtsp_sink_stop(MediaSink *sink) {
    RtspSinkImpl *impl = (RtspSinkImpl *)sink->impl;
    if (!impl) {
        return;
    }
    if (impl->session) {
        rtspDelSession(impl->session);
        impl->session = NULL;
    }
    if (impl->shared_server_acquired) {
        shared_rtsp_server_release();
        impl->shared_server_acquired = 0;
    }
}

/* 构建并初始化一个 RTSP sink。 */
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
