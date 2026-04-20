#include "rtspSink.h"

#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

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
    int last_client_count;         /* 上一次观察到的 RTSP 总客户端数。 */
    atomic_int pending_external_idr;                        /* 新客户端接入后的一次性 IDR 请求标记。 */
    atomic_int awaiting_first_keyframe_after_external_idr;  /* 已请求外部IDR，等待首个关键帧发出。 */
    atomic_ullong new_client_detect_ts_us;                 /* 检测到新客户端接入时刻。 */
    atomic_ullong external_idr_request_ts_us;              /* 外部IDR请求被消费并准备向编码器请求时刻。 */
    int pending_send_cached_sps_pps;                        /* 新客户端接入后，是否待发送缓存的 SPS/PPS。 */
    uint8_t *cached_sps;                                    /* 最近缓存的 SPS NALU（不含起始码）。 */
    size_t cached_sps_len;
    uint8_t *cached_pps;                                    /* 最近缓存的 PPS NALU（不含起始码）。 */
    size_t cached_pps_len;
} RtspSinkImpl;

static RtspSharedServer g_rtsp_shared_server;
static pthread_mutex_t g_rtsp_shared_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_rtsp_shared_ref_count = 0;

/* 单调时钟微秒时间戳，用于时延统计，避免系统时间跳变带来的误差。 */
static uint64_t now_us(void) {
#if defined(__linux__) || defined(__linux)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#else
    return 0;
#endif
}

/* 查询当前 sink 对应 session 的客户端数，不可用时返回 -1。 */
static int query_rtsp_session_client_count(void *session) {
    return rtspSessionGetClientNum(session);
}

/*
 * 轮询“当前 session 的客户端总数是否上升”。
 * 只要该 session 新增观看端，就置位外部 IDR 请求。
 * 这样可避免 live_main 与 live_sub 之间互相误触发关键帧。
 */
static void rtsp_sink_probe_new_client(RtspSinkImpl *impl) {
    int cur_count;
    uint64_t detect_ts_us;
    if (!impl || !impl->session) {
        return;
    }
    cur_count = query_rtsp_session_client_count(impl->session);
    if (cur_count < 0) {
        return;
    }
    if (cur_count > impl->last_client_count && cur_count > 0) {
        detect_ts_us = now_us();
        atomic_store(&impl->new_client_detect_ts_us, detect_ts_us);
        atomic_store(&impl->pending_external_idr, 1);
        if (impl->config.immediate_sps_pps_on_new_client) {
            impl->pending_send_cached_sps_pps = 1;
        }
        printf("[E2E] event=new_client_detected session=%s clients=%d ts_us=%" PRIu64 "\n",
               impl->config.session_name ? impl->config.session_name : "unknown",
               cur_count,
               detect_ts_us);
    }
    impl->last_client_count = cur_count;
}

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

/* 缓存最新 SPS/PPS，供“新客户端立刻补参数集”模式使用。 */
static void cache_h264_parameter_set(RtspSinkImpl *impl, int nalu_type, const uint8_t *nalu, size_t nalu_len) {
    uint8_t *new_buf;
    if (!impl || !nalu || nalu_len == 0) {
        return;
    }
    if (nalu_type != 7 && nalu_type != 8) {
        return;
    }
    new_buf = (uint8_t *)malloc(nalu_len);
    if (!new_buf) {
        return;
    }
    memcpy(new_buf, nalu, nalu_len);
    if (nalu_type == 7) {
        free(impl->cached_sps);
        impl->cached_sps = new_buf;
        impl->cached_sps_len = nalu_len;
    } else {
        free(impl->cached_pps);
        impl->cached_pps = new_buf;
        impl->cached_pps_len = nalu_len;
    }
}

/* 解析 Annex-B 并更新 SPS/PPS 缓存。 */
static void update_sps_pps_cache(RtspSinkImpl *impl, const uint8_t *data, size_t len) {
    size_t nalu_start = 0;
    int code_len = 0;
    if (!impl || !data || len == 0) {
        return;
    }
    if (find_start_code(data, len, 0, &nalu_start, &code_len) != 0) {
        return;
    }
    while (nalu_start < len) {
        size_t payload_start = nalu_start + (size_t)code_len;
        size_t next_start = len;
        int next_code_len = 0;
        int nalu_type;
        if (payload_start >= len) {
            break;
        }
        find_start_code(data, len, payload_start, &next_start, &next_code_len);
        if (next_start <= payload_start) {
            break;
        }
        nalu_type = data[payload_start] & 0x1F;
        cache_h264_parameter_set(impl, nalu_type, data + payload_start, next_start - payload_start);
        if (next_start >= len) {
            break;
        }
        nalu_start = next_start;
        code_len = next_code_len;
    }
}

/* 在普通码流发送前补发缓存的 SPS/PPS，帮助新客户端尽快完成解码初始化。 */
static void send_cached_sps_pps_if_needed(RtspSinkImpl *impl) {
    if (!impl || !impl->session) {
        return;
    }
    if (!impl->config.immediate_sps_pps_on_new_client || !impl->pending_send_cached_sps_pps) {
        return;
    }
    if (!impl->cached_sps || !impl->cached_pps || impl->cached_sps_len == 0 || impl->cached_pps_len == 0) {
        return;
    }
    if (sessionSendVideoData(impl->session, (unsigned char *)impl->cached_sps, (int)impl->cached_sps_len) < 0) {
        return;
    }
    if (sessionSendVideoData(impl->session, (unsigned char *)impl->cached_pps, (int)impl->cached_pps_len) < 0) {
        return;
    }
    impl->pending_send_cached_sps_pps = 0;
    printf("[E2E] event=cached_sps_pps_sent session=%s sps=%zu pps=%zu\n",
           impl->config.session_name ? impl->config.session_name : "unknown",
           impl->cached_sps_len,
           impl->cached_pps_len);
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
    if (!impl) {
        fprintf(stderr, "[ERROR] rtsp_sink_start failed: impl is NULL\n");
        return -1;
    }

    if (shared_rtsp_server_acquire(&impl->config) != 0) {
        fprintf(stderr, "[ERROR] rtsp_sink_start failed: acquire shared server ip=%s port=%d session=%s\n",
                impl->config.server_ip ? impl->config.server_ip : "unknown",
                impl->config.server_port,
                impl->config.session_name ? impl->config.session_name : "unknown");
        return -1;
    }
    impl->shared_server_acquired = 1;

    /* 创建 RTSP 会话 */
    impl->session = rtspAddSession(impl->config.session_name);
    if (!impl->session) {
        fprintf(stderr, "[ERROR] rtsp_sink_start failed: rtspAddSession session=%s\n",
                impl->config.session_name ? impl->config.session_name : "unknown");
        shared_rtsp_server_release();
        impl->shared_server_acquired = 0;
        return -1;
    }
    /* 当前session添加视频流 */
    if (sessionAddVideo(impl->session, VIDEO_H264) < 0) {
        fprintf(stderr, "[ERROR] rtsp_sink_start failed: sessionAddVideo session=%s codec=H264\n",
                impl->config.session_name ? impl->config.session_name : "unknown");
        rtspDelSession(impl->session);
        impl->session = NULL;
        shared_rtsp_server_release();
        impl->shared_server_acquired = 0;
        return -1;
    }
    impl->last_client_count = query_rtsp_session_client_count(impl->session);
    if (impl->last_client_count < 0) {
        impl->last_client_count = 0;
    }
    atomic_store(&impl->pending_external_idr, 0);
    atomic_store(&impl->awaiting_first_keyframe_after_external_idr, 0);
    atomic_store(&impl->new_client_detect_ts_us, 0);
    atomic_store(&impl->external_idr_request_ts_us, 0);
    impl->pending_send_cached_sps_pps = 0;

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
        fprintf(stderr, "[ERROR] rtsp_sink_send_packet failed: invalid args session_ready=%d packet=%p buffer=%p\n",
                (impl && impl->session) ? 1 : 0,
                (void *)packet,
                (void *)(packet ? packet->buffer : NULL));
        return -1;
    }
    /* 在发送路径轻量轮询新客户端接入事件，避免额外线程/锁开销。 */
    rtsp_sink_probe_new_client(impl);
    /* 更新参数集缓存，并在开启开关时对新客户端补发 SPS/PPS。 */
    update_sps_pps_cache(impl, packet->buffer->data, packet->buffer->size);
    send_cached_sps_pps_if_needed(impl);
    if (packet->is_key_frame && atomic_exchange(&impl->awaiting_first_keyframe_after_external_idr, 0)) {
        uint64_t now = now_us();
        uint64_t detect_ts = atomic_load(&impl->new_client_detect_ts_us);
        uint64_t idr_req_ts = atomic_load(&impl->external_idr_request_ts_us);
        uint64_t detect_to_idr_req = (detect_ts > 0 && now >= detect_ts) ? (now - detect_ts) : 0;
        uint64_t idr_req_to_send = (idr_req_ts > 0 && now >= idr_req_ts) ? (now - idr_req_ts) : 0;
        uint64_t detect_to_send = (detect_ts > 0 && now >= detect_ts) ? (now - detect_ts) : 0;
        printf("[E2E] event=first_keyframe_sent_after_new_client session=%s frame_id=%" PRIu64
               " detect_to_idr_req_us=%" PRIu64 " idr_req_to_send_us=%" PRIu64 " detect_to_send_us=%" PRIu64 "\n",
               impl->config.session_name ? impl->config.session_name : "unknown",
               packet->frame_id,
               detect_to_idr_req,
               idr_req_to_send,
               detect_to_send);
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
    if (impl->cached_sps) {
        free(impl->cached_sps);
        impl->cached_sps = NULL;
        impl->cached_sps_len = 0;
    }
    if (impl->cached_pps) {
        free(impl->cached_pps);
        impl->cached_pps = NULL;
        impl->cached_pps_len = 0;
    }
    impl->pending_send_cached_sps_pps = 0;
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
        fprintf(stderr, "[ERROR] rtsp_sink_setup failed: sink is NULL\n");
        return -1;
    }

    impl = (RtspSinkImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        fprintf(stderr, "[ERROR] rtsp_sink_setup failed: impl alloc\n");
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
    impl->config.immediate_sps_pps_on_new_client =
        impl->config.immediate_sps_pps_on_new_client ? 1 : 0;

    memset(&sink_config, 0, sizeof(sink_config));
    sink_config.name = impl->config.name;
    sink_config.queue_capacity = (impl->config.queue_capacity > 0) ? impl->config.queue_capacity : 32;
    sink_config.reconnect_interval_ms = 1000;
    sink_config.drop_until_keyframe_after_reconnect = 0;

    if (media_sink_init(sink, &sink_config, &vtable, impl) != 0) {
        fprintf(stderr, "[ERROR] rtsp_sink_setup failed: media_sink_init name=%s session=%s\n",
                impl->config.name ? impl->config.name : "unknown",
                impl->config.session_name ? impl->config.session_name : "unknown");
        free(impl);
        return -1;
    }
    return 0;
}

int rtsp_sink_consume_external_idr_request(MediaSink *sink) {
    RtspSinkImpl *impl;
    uint64_t now;
    uint64_t detect_ts;
    if (!sink) {
        return 0;
    }
    impl = (RtspSinkImpl *)sink->impl;
    if (!impl || !impl->session) {
        return 0;
    }
    /* exchange 保证“同一次接入事件”只消费一次。 */
    if (!atomic_exchange(&impl->pending_external_idr, 0)) {
        return 0;
    }
    now = now_us();
    detect_ts = atomic_load(&impl->new_client_detect_ts_us);
    atomic_store(&impl->external_idr_request_ts_us, now);
    atomic_store(&impl->awaiting_first_keyframe_after_external_idr, 1);
    printf("[E2E] event=external_idr_requested session=%s detect_to_idr_req_us=%" PRIu64 " ts_us=%" PRIu64 "\n",
           impl->config.session_name ? impl->config.session_name : "unknown",
           (detect_ts > 0 && now >= detect_ts) ? (now - detect_ts) : 0,
           now);
    return 1;
}
