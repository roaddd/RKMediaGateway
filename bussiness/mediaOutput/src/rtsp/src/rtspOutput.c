#include "../inc/rtspOutput.h"

#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "commonDef.h"
#include "logger.h"
#include "rtsp_server_api.h"
#include "util.h"

#define DEFAULT_RTSP_OUTPUT_NAME "rtsp"
#define DEFAULT_RTSP_SESSION "live"
#define DEFAULT_RTSP_IP "0.0.0.0"
#define DEFAULT_RTSP_PORT 8554
#define DEFAULT_RTSP_USER "admin"
#define DEFAULT_RTSP_PASSWORD "123456"
#define MAX_RTSP_FEEDBACK_OUTPUTS 16

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
    MediaOutputRtspConfig config;         /* 当前 output 的配置副本。 */
    void *session;                 /* 当前 output 绑定的 RTSP session。 */
    int shared_server_acquired;    /* 是否已持有共享服务引用计数。 */
    int last_client_count;         /* 上一次观察到的 RTSP 总客户端数。 */
    atomic_int pending_external_idr;                        /* 新客户端接入后的一次性 IDR 请求标记。 */
    atomic_int awaiting_first_keyframe_after_external_idr;  /* 已请求外部IDR，等待首个关键帧发出。 */
    atomic_ullong new_client_detect_ts_us;                 /* 检测到新客户端接入时刻。 */
    atomic_ullong external_idr_request_ts_us;              /* 外部IDR请求被消费并准备向编码器请求时刻。 */
    int unsupported_audio_warned;                           /* 避免不支持的音频格式反复刷日志。 */
} RtspOutputImpl;

static RtspSharedServer g_rtsp_shared_server;
static pthread_mutex_t g_rtsp_shared_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_rtsp_shared_ref_count = 0;
static pthread_mutex_t g_rtsp_feedback_lock = PTHREAD_MUTEX_INITIALIZER;
static RtspOutputImpl *g_rtsp_feedback_outputs[MAX_RTSP_FEEDBACK_OUTPUTS];

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

/* 查询当前 output 对应 session 的客户端数，不可用时返回 -1。 */
static int query_rtsp_session_client_count(void *session) {
    return rtspSessionGetClientNum(session);
}

/*
 * 轮询“当前 session 的客户端总数是否上升”。
 * 只要该 session 新增观看端，就置位外部 IDR 请求。
 * 这样可避免 live_main 与 live_sub 之间互相误触发关键帧。
 */
static void rtsp_output_probe_new_client(RtspOutputImpl *impl) {
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

/*
 * 判断 RTSP server 上报的 session 名称是否属于当前 output。
 * simple-rtsp-server 内部可能保存带 mp4Dir 前缀的 filename，因此这里同时支持精确匹配和后缀匹配。
 */
static int rtsp_session_name_matches(const char *reported_name, const char *configured_name) {
    size_t reported_len = 0;
    size_t configured_len = 0;

    if (!reported_name || !configured_name) {
        return 0;
    }
    if (strcmp(reported_name, configured_name) == 0) {
        return 1;
    }
    reported_len = strlen(reported_name);
    configured_len = strlen(configured_name);
    return reported_len >= configured_len &&
           strcmp(reported_name + reported_len - configured_len, configured_name) == 0;
}

/*
 * 将当前 RTSP output 注册为 RTCP RR 反馈接收目标。
 * 回调来源是 simple-rtsp-server 的库级通知，output 层负责按 session 再分发到业务层回调。
 */
static void rtsp_output_register_feedback_target(RtspOutputImpl *impl) {
    int i = 0;

    if (!impl || !impl->config.feedback_holder || !impl->config.feedback_holder->callback) {
        return;
    }
    pthread_mutex_lock(&g_rtsp_feedback_lock);
    for (i = 0; i < MAX_RTSP_FEEDBACK_OUTPUTS; ++i) {
        if (g_rtsp_feedback_outputs[i] == impl) {
            pthread_mutex_unlock(&g_rtsp_feedback_lock);
            return;
        }
    }
    for (i = 0; i < MAX_RTSP_FEEDBACK_OUTPUTS; ++i) {
        if (!g_rtsp_feedback_outputs[i]) {
            g_rtsp_feedback_outputs[i] = impl;
            break;
        }
    }
    pthread_mutex_unlock(&g_rtsp_feedback_lock);
}

/* 从 RTCP RR 反馈目标表中移除当前 output，避免 session 删除后仍收到异步反馈。 */
static void rtsp_output_unregister_feedback_target(RtspOutputImpl *impl) {
    int i = 0;

    pthread_mutex_lock(&g_rtsp_feedback_lock);
    for (i = 0; i < MAX_RTSP_FEEDBACK_OUTPUTS; ++i) {
        if (g_rtsp_feedback_outputs[i] == impl) {
            g_rtsp_feedback_outputs[i] = NULL;
        }
    }
    pthread_mutex_unlock(&g_rtsp_feedback_lock);
}

/*
 * RTSP 库级 RTCP RR 回调入口。
 * 本函数不做自适应决策，只把 RTCP 指标转换为 MediaOutputNetFeedbackInfo，
 * 再通过 output 创建时注入的回调交给 MediaGateway 统一控制器。
 */
static void rtsp_output_handle_rtcp_report(const RtspRtcpReceiverReport *report, void *userdata) {
    int i = 0;
    RtspOutputImpl *impl = NULL;
    MediaOutputNetFeedbackInfo feedback = {0};

    (void)userdata;
    if (!report) 
    {
        LOG_ERROR("rtsp_output_handle_rtcp_report failed: report is NULL");
        return;
    }
    pthread_mutex_lock(&g_rtsp_feedback_lock);
    for (i = 0; i < MAX_RTSP_FEEDBACK_OUTPUTS; ++i) {
        impl = g_rtsp_feedback_outputs[i];
        if (!impl || !impl->config.feedback_holder || !impl->config.feedback_holder->callback) {
            continue;
        }
        if (!rtsp_session_name_matches(report->session_name, impl->config.session_name)) {
            continue;
        }
        memset(&feedback, 0, sizeof(feedback));
        feedback.output_type = MEDIA_OUTPUT_TYPE_RTSP;
        feedback.output_name = impl->config.name;
        feedback.session_name = impl->config.session_name;
        feedback.client_ip = report->client_ip;
        feedback.is_audio = report->is_audio;
        feedback.fraction_lost = report->fraction_lost;
        feedback.cumulative_lost = report->cumulative_lost;
        feedback.jitter = report->jitter;
        feedback.rtt_ms = report->rtt_ms;
        feedback.rr_count = report->rr_count;
        impl->config.feedback_holder->callback(&feedback, impl->config.feedback_holder->userdata);
    }
    pthread_mutex_unlock(&g_rtsp_feedback_lock);
}

/* 发送一帧编码音频到 RTSP server，并保留媒体 PTS；返回 MEDIA_OK 或统一错误码。 */
static int rtsp_output_send_audio_packet(RtspOutputImpl *impl, const MediaPacket *packet) {
    RtspMediaFrame frame;
    if (packet->codec != MEDIA_CODEC_G711A && packet->codec != MEDIA_CODEC_AAC) {
        if (!impl->unsupported_audio_warned) {
            LOG_WARN("rtsp_output_send_audio_packet skip unsupported audio codec=%d, only G711A/PCMA and AAC are supported",
                     packet->codec);
            impl->unsupported_audio_warned = 1;
        }
        return MEDIA_OK;
    }

    frame.data = (uint8_t *)packet->buffer->data;
    frame.data_len = (int)packet->buffer->size;
    frame.pts_us = packet->pts_us; /* 这个pts_us是DBUF的时间，rtp_ts = pts_us * 90000ULL / 1000000ULL; */
    if (sessionSendAudioFrame(impl->session, &frame) < 0) {
        LOG_ERROR("rtsp_output_send_audio_packet failed: sessionSendAudioFrame size=%zu frame=%" PRIu64,
                  packet->buffer->size,
                  packet->frame_id);
        return MEDIA_ERR;
    }
    return MEDIA_OK;
}

/* 根据 RTSP output 配置添加音频 track；返回 MEDIA_OK 或统一错误码。 */
static int rtsp_output_add_audio_track(RtspOutputImpl *impl) {
    MediaCodecType codec;
    int sample_rate;
    int channels;
    int profile;
    enum AUDIO_e rtsp_audio_type;

    codec = impl->config.audio_codec;
    if (codec == MEDIA_CODEC_NONE) {
        return MEDIA_OK;
    }

    sample_rate = (impl->config.audio_sample_rate > 0) ? impl->config.audio_sample_rate : 8000;
    channels = (impl->config.audio_channels > 0) ? impl->config.audio_channels : 1;
    profile = (impl->config.aac_profile > 0) ? impl->config.aac_profile : 2;

    if (codec == MEDIA_CODEC_AAC) {
        rtsp_audio_type = AUDIO_AAC;
    } else if (codec == MEDIA_CODEC_G711A) {
        rtsp_audio_type = AUDIO_PCMA;
        profile = 0;
    } else {
        LOG_WARN("rtsp_output_start skip unsupported declared audio codec=%d session=%s",
                 codec,
                 impl->config.session_name ? impl->config.session_name : "unknown");
        return MEDIA_OK;
    }

    if (sessionAddAudio(impl->session, rtsp_audio_type, profile, sample_rate, channels) < 0) {
        LOG_ERROR("rtsp_output_start failed: sessionAddAudio session=%s codec=%d sample_rate=%d channels=%d",
                  impl->config.session_name ? impl->config.session_name : "unknown",
                  codec,
                  sample_rate,
                  channels);
        return MEDIA_ERR;
    }
    return MEDIA_OK;
}

static void rtsp_output_report_first_keyframe_after_idr(RtspOutputImpl *impl, const MediaPacket *packet) {
    uint64_t now;
    uint64_t detect_ts;
    uint64_t idr_req_ts;
    uint64_t detect_to_idr_req;
    uint64_t idr_req_to_send;
    uint64_t detect_to_send;

    if (!packet->is_key_frame || !atomic_exchange(&impl->awaiting_first_keyframe_after_external_idr, 0)) {
        return;
    }

    now = now_us();
    detect_ts = atomic_load(&impl->new_client_detect_ts_us);
    idr_req_ts = atomic_load(&impl->external_idr_request_ts_us);
    detect_to_idr_req = (detect_ts > 0 && now >= detect_ts) ? (now - detect_ts) : 0;
    idr_req_to_send = (idr_req_ts > 0 && now >= idr_req_ts) ? (now - idr_req_ts) : 0;
    detect_to_send = (detect_ts > 0 && now >= detect_ts) ? (now - detect_ts) : 0;
    printf("[E2E] event=first_keyframe_sent_after_new_client session=%s frame_id=%" PRIu64
           " detect_to_idr_req_us=%" PRIu64 " idr_req_to_send_us=%" PRIu64 " detect_to_send_us=%" PRIu64 "\n",
           impl->config.session_name ? impl->config.session_name : "unknown",
           packet->frame_id,
           detect_to_idr_req,
           idr_req_to_send,
           detect_to_send);
}

/* 发送一帧 H264 视频到 RTSP server；返回 MEDIA_OK 或统一错误码。 */
static int rtsp_output_send_video_packet(RtspOutputImpl *impl, const MediaPacket *packet) {
    RtspMediaFrame frame;
    int ret = MEDIA_OK;

    if (packet->codec != MEDIA_CODEC_H264) {
        return MEDIA_OK;
    }

    rtsp_output_probe_new_client(impl);
    rtsp_output_report_first_keyframe_after_idr(impl, packet);
    /*
     * 这里不要拆 Annex-B。rtsp-server 需要完整 access unit，
     * 这样同一帧里的 SPS/PPS/slice 才能共用一个 RTP timestamp 和一个 marker。
     */
    frame.data = (uint8_t *)packet->buffer->data;
    frame.data_len = (int)packet->buffer->size;
    frame.pts_us = packet->pts_us;
    ret = sessionSendVideoFrame(impl->session, &frame);
    if (ret < 0)
        return MEDIA_ERR;
    return MEDIA_OK;
}

/* 共享 RTSP 服务线程入口：阻塞运行 rtspStartServer。 */
static void *shared_rtsp_server_thread(void *arg) {
    RtspSharedServer *shared = (RtspSharedServer *)arg;
    util_set_thread_name("rtsp-server");
    rtspStartServer(shared->auth_enable,
                    shared->server_ip,
                    shared->server_port,
                    shared->user,
                    shared->password);
    return NULL;
}

/* 校验新 output 的 server 参数是否与共享 server 一致。 */
static int shared_rtsp_config_compatible(const MediaOutputRtspConfig *cfg) {
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
 * 并检查新 output 的 server 参数是否和已启动的一致（端口/鉴权等）
 * 确保 main/sub 共用同一个 8554 服务端
 * @param {MediaOutputRtspConfig} *cfg
 * @return {*}
 */

/* 获取共享 RTSP server 引用；返回 MEDIA_OK 或统一错误码。 */
static int shared_rtsp_server_acquire(const MediaOutputRtspConfig *cfg) {
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
            LOG_ERROR("shared_rtsp_server_acquire failed: rtspModuleInit ip=%s port=%d",
                      cfg->server_ip ? cfg->server_ip : "unknown",
                      cfg->server_port);
            pthread_mutex_unlock(&g_rtsp_shared_lock);
            return MEDIA_ERR;
        }
        rtspSetRtcpReportCallback(rtsp_output_handle_rtcp_report, NULL);
        g_rtsp_shared_server.module_ready = 1;
        g_rtsp_shared_server.in_use = 1;
        /* 启动rtsp服务线程 */
        ret = pthread_create(&g_rtsp_shared_server.server_thread, NULL, shared_rtsp_server_thread, &g_rtsp_shared_server);
        if (ret != 0) {
            LOG_ERROR("shared_rtsp_server_acquire failed: pthread_create ret=%d ip=%s port=%d",
                      ret,
                      cfg->server_ip ? cfg->server_ip : "unknown",
                      cfg->server_port);
            rtspModuleDel();
            memset(&g_rtsp_shared_server, 0, sizeof(g_rtsp_shared_server));
            pthread_mutex_unlock(&g_rtsp_shared_lock);
            return MEDIA_ERR;
        }
        g_rtsp_shared_server.server_started = 1;
    } else if (!shared_rtsp_config_compatible(cfg)) {
        LOG_ERROR("RTSP shared server config conflict: expect %s:%d auth=%d user=%s",
                  g_rtsp_shared_server.server_ip,
                  g_rtsp_shared_server.server_port,
                  g_rtsp_shared_server.auth_enable,
                  g_rtsp_shared_server.user);
        pthread_mutex_unlock(&g_rtsp_shared_lock);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    g_rtsp_shared_ref_count++;
    pthread_mutex_unlock(&g_rtsp_shared_lock);
    return MEDIA_OK;
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
            rtspSetRtcpReportCallback(NULL, NULL);
            rtspModuleDel();
        }
        memset(&g_rtsp_shared_server, 0, sizeof(g_rtsp_shared_server));
    }

    pthread_mutex_unlock(&g_rtsp_shared_lock);
}

/* output 启动：挂载共享 server，并创建当前 output 的 session；返回 MEDIA_OK 或统一错误码。 */
static int rtsp_output_start(MediaOutput *output) {
    RtspOutputImpl *impl = (RtspOutputImpl *)output->impl;
    if (!impl) {
        LOG_ERROR("rtsp_output_start failed: impl is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    if (shared_rtsp_server_acquire(&impl->config) != MEDIA_OK) {
        LOG_ERROR("rtsp_output_start failed: acquire shared server ip=%s port=%d session=%s",
                  impl->config.server_ip ? impl->config.server_ip : "unknown",
                  impl->config.server_port,
                  impl->config.session_name ? impl->config.session_name : "unknown");
        return MEDIA_ERR;
    }
    impl->shared_server_acquired = 1;

    /* 创建 RTSP 会话 */
    impl->session = rtspAddSession(impl->config.session_name);
    if (!impl->session) {
        LOG_ERROR("rtsp_output_start failed: rtspAddSession session=%s",
                  impl->config.session_name ? impl->config.session_name : "unknown");
        shared_rtsp_server_release();
        impl->shared_server_acquired = 0;
        return MEDIA_ERR;
    }
    if (rtsp_output_add_audio_track(impl) != MEDIA_OK) {
        rtspDelSession(impl->session);
        impl->session = NULL;
        shared_rtsp_server_release();
        impl->shared_server_acquired = 0;
        return MEDIA_ERR;
    }
    /* 当前session添加视频流 */
    if (sessionAddVideo(impl->session, VIDEO_H264) < 0) {
        LOG_ERROR("rtsp_output_start failed: sessionAddVideo session=%s codec=H264",
                  impl->config.session_name ? impl->config.session_name : "unknown");
        rtspDelSession(impl->session);
        impl->session = NULL;
        shared_rtsp_server_release();
        impl->shared_server_acquired = 0;
        return MEDIA_ERR;
    }
    impl->last_client_count = query_rtsp_session_client_count(impl->session);
    if (impl->last_client_count < 0) {
        impl->last_client_count = 0;
    }
    atomic_store(&impl->pending_external_idr, 0);
    atomic_store(&impl->awaiting_first_keyframe_after_external_idr, 0);
    atomic_store(&impl->new_client_detect_ts_us, 0);
    atomic_store(&impl->external_idr_request_ts_us, 0);
    rtsp_output_register_feedback_target(impl);

    printf("[INFO] RTSP output ready: rtsp://%s:%d/%s\n",
           impl->config.server_ip,
           impl->config.server_port,
           impl->config.session_name);
    return MEDIA_OK;
}

/* output 连接检查：session 已就绪即可视为可用；返回 MEDIA_OK 或统一错误码。 */
static int rtsp_output_connect(MediaOutput *output) {
    RtspOutputImpl *impl = (RtspOutputImpl *)output->impl;
    if (!impl || !impl->session) {
        LOG_ERROR("rtsp_output_connect failed: session is NULL");
        return MEDIA_ERR_NOT_READY;
    }
    return MEDIA_OK;
}

/* output 发送：视频发送 H264，音频发送 G711A/PCMA；返回 MEDIA_OK 或统一错误码。 */
static int rtsp_output_send_packet(MediaOutput *output, const MediaPacket *packet) {
    RtspOutputImpl *impl = (RtspOutputImpl *)output->impl;

    if (!impl || !impl->session || !packet || !packet->buffer) {
        LOG_ERROR("rtsp_output_send_packet failed: invalid args session_ready=%d packet=%p buffer=%p",
                  (impl && impl->session) ? 1 : 0,
                  (void *)packet,
                  (void *)(packet ? packet->buffer : NULL));
        return MEDIA_ERR_INVALID_PARAM;
    }

    if (packet->frame_type == MEDIA_FRAME_TYPE_AUDIO) {
        return rtsp_output_send_audio_packet(impl, packet);
    }
    if (packet->frame_type == MEDIA_FRAME_TYPE_VIDEO) {
        return rtsp_output_send_video_packet(impl, packet);
    }
    return MEDIA_OK;
}

/* 设置 RTSP session 的视频 RTP 包级 pacer；返回 MEDIA_OK 或统一错误码。 */
static int rtsp_output_set_video_pacer(MediaOutput *output, MediaOutputPacerMode mode, int pacing_rate_bps) {
    RtspOutputImpl *impl = NULL;
    RtspVideoPacerMode rtsp_mode = RTSP_VIDEO_PACER_DISABLED;
    int enabled = 0;

    if (!output) {
        LOG_ERROR("output is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    switch (mode) {
    case MEDIA_OUTPUT_PACER_DISABLED:
        enabled = 0;
        rtsp_mode = RTSP_VIDEO_PACER_DISABLED;
        break;
    case MEDIA_OUTPUT_PACER_ENABLED:
        enabled = 1;
        rtsp_mode = RTSP_VIDEO_PACER_ENABLED;
        break;
    default:
        LOG_ERROR("invalid mode=%d", mode);
        return MEDIA_ERR_INVALID_PARAM;
    }

    if (enabled && pacing_rate_bps <= 0) {
        LOG_ERROR("enabled=%d invalid rate=%d",
                  enabled,
                  pacing_rate_bps);
        return MEDIA_ERR_INVALID_PARAM;
    }

    impl = (RtspOutputImpl *)output->impl;
    if (!impl || !impl->session) {
        /*
         * session 未创建时不能缓存为已应用；返回失败让通用层保留旧值，
         * 后续 output start 后 gateway 主循环会继续下发当前 pacing 目标。
         */
        LOG_ERROR("session is NULL");
        return MEDIA_ERR_NOT_READY;
    }
    if (rtspSetSessionVideoPacer(impl->session, rtsp_mode, pacing_rate_bps) != 0) {
        LOG_ERROR("session=%s enabled=%d rate=%d",
                  impl->config.session_name ? impl->config.session_name : "unknown",
                  enabled,
                  pacing_rate_bps);
        return MEDIA_ERR;
    }
    LOG_WARN("[RTSP_PACER] session=%s enabled=%d video_pacing_rate_bps=%d",
             impl->config.session_name ? impl->config.session_name : "unknown",
             enabled,
             pacing_rate_bps);
    return MEDIA_OK;
}

/* 查询当前 RTSP session 的视频 RTP pacer 调试统计。 */
static int rtsp_output_get_video_pacer_stats(MediaOutput *output, MediaOutputPacerStats *stats) {
    RtspOutputImpl *impl = NULL;
    RtspVideoPacerStats rtsp_stats = {0};
    int i = 0;

    if (!output || !stats) {
        LOG_ERROR("rtsp_output_get_video_pacer_stats failed: output=%p stats=%p",
                  (void *)output,
                  (void *)stats);
        return MEDIA_ERR_INVALID_PARAM;
    }

    memset(stats, 0, sizeof(*stats));
    impl = (RtspOutputImpl *)output->impl;
    if (!impl || !impl->session) {
        LOG_ERROR("rtsp_output_get_video_pacer_stats failed: session is NULL");
        return MEDIA_ERR_NOT_READY;
    }

    if (rtspGetSessionVideoPacerStats(impl->session, &rtsp_stats) != 0) {
        LOG_ERROR("rtsp_output_get_video_pacer_stats failed: session=%s",
                  impl->config.session_name ? impl->config.session_name : "unknown");
        return MEDIA_ERR;
    }

    stats->mode = rtsp_stats.mode == RTSP_VIDEO_PACER_ENABLED ?
                  MEDIA_OUTPUT_PACER_ENABLED :
                  MEDIA_OUTPUT_PACER_DISABLED;
    stats->pacing_rate_bps = rtsp_stats.pacing_rate_bps;
    stats->total_client_count = rtsp_stats.total_client_count;
    stats->reported_client_count = rtsp_stats.reported_client_count;
    for (i = 0; i < rtsp_stats.reported_client_count &&
                i < MEDIA_OUTPUT_PACER_STATS_MAX_CLIENTS &&
                i < RTSP_VIDEO_PACER_STATS_MAX_CLIENTS; ++i) {
        stats->clients[i].in_use = rtsp_stats.clients[i].in_use;
        snprintf(stats->clients[i].client_ip,
                 sizeof(stats->clients[i].client_ip),
                 "%s",
                 rtsp_stats.clients[i].client_ip);
        stats->clients[i].client_rtp_port = rtsp_stats.clients[i].client_rtp_port;
        stats->clients[i].transport = rtsp_stats.clients[i].transport;
        stats->clients[i].rate_bps = rtsp_stats.clients[i].rate_bps;
        stats->clients[i].packet_count = rtsp_stats.clients[i].packet_count;
        stats->clients[i].byte_count = rtsp_stats.clients[i].byte_count;
        stats->clients[i].sleep_count = rtsp_stats.clients[i].sleep_count;
        stats->clients[i].total_sleep_us = rtsp_stats.clients[i].total_sleep_us;
        stats->clients[i].last_packet_bytes = rtsp_stats.clients[i].last_packet_bytes;
        stats->clients[i].last_sleep_us = rtsp_stats.clients[i].last_sleep_us;
        stats->clients[i].last_interval_us = rtsp_stats.clients[i].last_interval_us;
        stats->clients[i].last_window_bps = rtsp_stats.clients[i].last_window_bps;
        stats->clients[i].max_window_bps = rtsp_stats.clients[i].max_window_bps;
        stats->clients[i].current_window_bps = rtsp_stats.clients[i].current_window_bps;
        stats->clients[i].last_window_bytes = rtsp_stats.clients[i].last_window_bytes;
        stats->clients[i].last_window_packets = rtsp_stats.clients[i].last_window_packets;
        stats->clients[i].last_window_elapsed_us = rtsp_stats.clients[i].last_window_elapsed_us;
        stats->clients[i].max_window_bytes = rtsp_stats.clients[i].max_window_bytes;
        stats->clients[i].max_window_packets = rtsp_stats.clients[i].max_window_packets;
        stats->clients[i].max_window_elapsed_us = rtsp_stats.clients[i].max_window_elapsed_us;
        stats->clients[i].reset_count = rtsp_stats.clients[i].reset_count;
        stats->clients[i].last_reset_reason = rtsp_stats.clients[i].last_reset_reason;
        stats->clients[i].last_reset_lag_us = rtsp_stats.clients[i].last_reset_lag_us;
        stats->clients[i].last_reset_now_us = rtsp_stats.clients[i].last_reset_now_us;
        stats->clients[i].last_reset_next_send_ts_us = rtsp_stats.clients[i].last_reset_next_send_ts_us;
        stats->clients[i].last_reset_window_bytes = rtsp_stats.clients[i].last_reset_window_bytes;
        stats->clients[i].last_reset_window_packets = rtsp_stats.clients[i].last_reset_window_packets;
    }
    return MEDIA_OK;
}

/* 当前实现无需主动断链，保留该钩子用于接口一致性。 */
static void rtsp_output_disconnect(MediaOutput *output) {
    (void)output;
}

/* output 停止：先删 session，再释放共享 server 引用。 */
static void rtsp_output_stop(MediaOutput *output) {
    RtspOutputImpl *impl = (RtspOutputImpl *)output->impl;
    if (!impl) {
        return;
    }
    if (impl->session) {
        rtsp_output_unregister_feedback_target(impl);
        rtspDelSession(impl->session);
        impl->session = NULL;
    }
    if (impl->shared_server_acquired) {
        shared_rtsp_server_release();
        impl->shared_server_acquired = 0;
    }
}

/* 构建并初始化一个 RTSP output；返回 MEDIA_OK 或统一错误码。 */
int media_output_setup_rtsp(MediaOutput *output, const MediaOutputRtspConfig *config) {
    static const MediaOutputVTable vtable = {
        rtsp_output_start,
        rtsp_output_connect,
        rtsp_output_send_packet,
        rtsp_output_disconnect,
        rtsp_output_stop,
        rtsp_output_set_video_pacer,
        rtsp_output_get_video_pacer_stats
    };
    MediaOutputChannelConfig output_config;
    RtspOutputImpl *impl;

    if (!output) {
        LOG_ERROR("media_output_setup_rtsp failed: output is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    impl = (RtspOutputImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        LOG_ERROR("media_output_setup_rtsp failed: impl alloc");
        return MEDIA_ERR_NO_MEMORY;
    }

    if (config) {
        impl->config = *config;
    }
    if (!impl->config.name) {
        impl->config.name = DEFAULT_RTSP_OUTPUT_NAME;
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
    if (impl->config.immediate_sps_pps_on_new_client) {
        /*
         * 旧模式会把缓存的 SPS/PPS 当作独立 NALU 发送。
         * 切到整帧 RTP 输入后，解码初始化应通过新客户端触发的 IDR 完成。
         */
        LOG_WARN("RTSP immediate SPS/PPS is ignored after switching to frame-based RTSP input; request IDR on new client instead");
        impl->config.immediate_sps_pps_on_new_client = 0;
    }
    memset(&output_config, 0, sizeof(output_config));
    output_config.name = impl->config.name;
    output_config.queue_capacity = (impl->config.queue_capacity > 0) ? impl->config.queue_capacity : 32;
    output_config.reconnect_interval_ms = 1000;
    output_config.drop_until_keyframe_after_reconnect = 0;
    output_config.feedback_holder = impl->config.feedback_holder;

    if (media_output_init(output, &output_config, &vtable, impl) != MEDIA_OK) {
        LOG_ERROR("media_output_setup_rtsp failed: media_output_init name=%s session=%s",
                  impl->config.name ? impl->config.name : "unknown",
                  impl->config.session_name ? impl->config.session_name : "unknown");
        free(impl);
        return MEDIA_ERR;
    }
    output->type = MEDIA_OUTPUT_TYPE_RTSP;
    return MEDIA_OK;
}

int media_output_rtsp_consume_external_idr_request(MediaOutput *output) {
    RtspOutputImpl *impl;
    uint64_t now;
    uint64_t detect_ts;
    if (!output) {
        return 0;
    }
    impl = (RtspOutputImpl *)output->impl;
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
