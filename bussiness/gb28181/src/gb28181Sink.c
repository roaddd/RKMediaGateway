#include "gb28181Sink.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb28181Device.h"

/*
 * GB28181 sink 的职责：
 * 1. 作为 mediaGateway 的一个输出通道（MediaSink）接收编码后的视频包；
 * 2. 将 MediaPacket(H264 Annex-B) 转交给 gb28181Device 模块封装并发送；
 * 3. 管理 gb28181Device 的生命周期与 SIP 事件线程。
 */

typedef struct {
    Gb28181SinkConfig config;     /* sink 配置副本，避免外部临时配置对象失效。 */
    Gb28181DeviceCtx device_ctx;  /* 底层 GB28181 设备模块上下文。 */
    pthread_t sip_thread;         /* 运行 gb28181_device_run() 的线程句柄。 */
    int sip_thread_started;       /* SIP 线程是否已创建成功。 */
    int started;                  /* sink 是否已进入启动完成状态。 */
} Gb28181SinkImpl;

/* 字符串兜底：value 为空时返回 fallback。 */
static const char *safe_str(const char *value, const char *fallback) {
    return (value && value[0] != '\0') ? value : fallback;
}

/*
 * 补齐 sink 默认配置。
 * 这里的默认值与 gb28181Device 内部默认保持一致，避免两层默认值不一致导致行为偏差。
 */
static void fill_default_config(Gb28181SinkConfig *dst, const Gb28181SinkConfig *src) {
    memset(dst, 0, sizeof(*dst));
    if (src) {
        *dst = *src;
    }
    dst->name = safe_str(dst->name, "gb28181");
    dst->server_ip = safe_str(dst->server_ip, "192.168.1.1");
    if (dst->server_port <= 0) dst->server_port = 5060;
    dst->server_domain = safe_str(dst->server_domain, "3402000000");
    dst->server_id = safe_str(dst->server_id, "34020000002000000001");
    dst->device_id = safe_str(dst->device_id, "34020000001320000001");
    dst->device_domain = safe_str(dst->device_domain, dst->server_domain);
    dst->device_password = safe_str(dst->device_password, "12345678");
    dst->bind_ip = safe_str(dst->bind_ip, "0.0.0.0");
    if (dst->local_sip_port <= 0) dst->local_sip_port = 5060;
    dst->sip_contact_ip = safe_str(dst->sip_contact_ip, "127.0.0.1");
    dst->media_ip = safe_str(dst->media_ip, dst->sip_contact_ip);
    if (dst->media_port <= 0) dst->media_port = 30000;
    if (dst->register_expires <= 0) dst->register_expires = 3600;
    if (dst->keepalive_interval_sec <= 0) dst->keepalive_interval_sec = 60;
    if (dst->register_retry_interval_sec <= 0) dst->register_retry_interval_sec = 5;
    dst->device_name = safe_str(dst->device_name, "RK3568 Camera");
    dst->manufacturer = safe_str(dst->manufacturer, "Topeet");
    dst->model = safe_str(dst->model, "RKMediaGateway");
    dst->firmware = safe_str(dst->firmware, "1.0.0");
    dst->channel_id = safe_str(dst->channel_id, dst->device_id);
    dst->user_agent = safe_str(dst->user_agent, "RKMediaGateway-GB28181/1.0");
}

/*
 * 将 sink 配置转换为 gb28181Device 配置。
 * 强制 external_media_input=1：表示 gb28181Device 不再自己初始化 V4L2/MPP，
 * 而是复用 mediaGateway 共享编码链路，由本 sink 注入 H264。
 */
static void build_device_config(const Gb28181SinkConfig *src, Gb28181DeviceConfig *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->server_ip = src->server_ip;
    dst->server_port = src->server_port;
    dst->server_id = src->server_id;
    dst->server_domain = src->server_domain;
    dst->device_id = src->device_id;
    dst->device_domain = src->device_domain;
    dst->device_password = src->device_password;
    dst->bind_ip = src->bind_ip;
    dst->local_sip_port = src->local_sip_port;
    dst->sip_contact_ip = src->sip_contact_ip;
    dst->media_ip = src->media_ip;
    dst->media_port = src->media_port;
    dst->register_expires = src->register_expires;
    dst->keepalive_interval_sec = src->keepalive_interval_sec;
    dst->register_retry_interval_sec = src->register_retry_interval_sec;
    dst->device_name = src->device_name;
    dst->manufacturer = src->manufacturer;
    dst->model = src->model;
    dst->firmware = src->firmware;
    dst->channel_id = src->channel_id;
    dst->user_agent = src->user_agent;
    dst->external_media_input = 1;
}

/* SIP 线程入口：阻塞运行 gb28181_device_run() 直到 stop。 */
static void *gb28181_sink_sip_loop(void *arg) {
    Gb28181SinkImpl *impl = (Gb28181SinkImpl *)arg;
    if (!impl) {
        return NULL;
    }
    gb28181_device_run(&impl->device_ctx);
    return NULL;
}

/*
 * sink start：
 * 1. 初始化 gb28181Device；
 * 2. 拉起 SIP 事件线程；
 * 3. 进入 started 状态。
 */
static int gb28181_sink_start(MediaSink *sink) {
    Gb28181SinkImpl *impl = (Gb28181SinkImpl *)sink->impl;
    Gb28181DeviceConfig device_config;
    if (!impl) {
        return -1;
    }
    if (impl->started) {
        return 0;
    }

    build_device_config(&impl->config, &device_config);
    if (gb28181_device_init(&impl->device_ctx, &device_config) != 0) {
        return -1;
    }
    if (pthread_create(&impl->sip_thread, NULL, gb28181_sink_sip_loop, impl) != 0) {
        gb28181_device_deinit(&impl->device_ctx);
        return -1;
    }

    impl->sip_thread_started = 1;
    impl->started = 1;
    return 0;
}

/* MediaSink connect 钩子：此处仅确认 sink 已启动。 */
static int gb28181_sink_connect(MediaSink *sink) {
    Gb28181SinkImpl *impl = (Gb28181SinkImpl *)sink->impl;
    return (impl && impl->started) ? 0 : -1;
}

/*
 * 发送路径：
 * - 仅处理 H264 包；
 * - 将 Annex-B 数据转交 gb28181_device_send_h264()；
 * - 实际的 PS 封装与 RTP 分包在 gb28181Device 内部完成。
 */
static int gb28181_sink_send_packet(MediaSink *sink, const MediaPacket *packet) {
    Gb28181SinkImpl *impl = (Gb28181SinkImpl *)sink->impl;
    if (!impl || !impl->started || !packet || !packet->buffer) {
        return -1;
    }

    /* 非 H264 直接忽略，保持通道宽容，不影响主循环。 */
    if (packet->codec != MEDIA_CODEC_H264) {
        return 0;
    }

    if (gb28181_device_send_h264(&impl->device_ctx,
                                 packet->buffer->data,
                                 packet->buffer->size,
                                 packet->is_key_frame,
                                 packet->pts_us) != 0) {
        return -1;
    }
    return 0;
}

/* disconnect 钩子：当前无额外动作，真正收尾在 stop。 */
static void gb28181_sink_disconnect(MediaSink *sink) {
    (void)sink;
}

/*
 * sink stop：
 * 1. 请求 gb28181Device 停止；
 * 2. 等待 SIP 线程退出；
 * 3. 释放 gb28181Device 资源。
 */
static void gb28181_sink_stop(MediaSink *sink) {
    Gb28181SinkImpl *impl = (Gb28181SinkImpl *)sink->impl;
    if (!impl) {
        return;
    }

    gb28181_device_stop(&impl->device_ctx);
    if (impl->sip_thread_started) {
        pthread_join(impl->sip_thread, NULL);
        impl->sip_thread_started = 0;
    }
    gb28181_device_deinit(&impl->device_ctx);
    impl->started = 0;
}

/*
 * 创建并初始化 GB28181 sink。
 * 该函数只做对象构建与通用队列初始化，不会真正建立 SIP/RTP 连接。
 */
int gb28181_sink_setup(MediaSink *sink, const Gb28181SinkConfig *config) {
    static const MediaSinkVTable vtable = {
        gb28181_sink_start,
        gb28181_sink_connect,
        gb28181_sink_send_packet,
        gb28181_sink_disconnect,
        gb28181_sink_stop
    };
    MediaSinkConfig sink_config;
    Gb28181SinkImpl *impl = NULL;

    if (!sink) {
        return -1;
    }

    impl = (Gb28181SinkImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        return -1;
    }
    fill_default_config(&impl->config, config);

    memset(&sink_config, 0, sizeof(sink_config));
    sink_config.name = impl->config.name;
    sink_config.queue_capacity = (impl->config.queue_capacity > 0) ? impl->config.queue_capacity : 64;
    sink_config.reconnect_interval_ms = 1000;
    /* 点播建立后先等关键帧，确保首批对外发送就是可解码起点。 */
    sink_config.drop_until_keyframe_after_reconnect = 1;

    if (media_sink_init(sink, &sink_config, &vtable, impl) != 0) {
        free(impl);
        return -1;
    }
    return 0;
}

int gb28181_sink_consume_external_idr_request(MediaSink *sink) {
    Gb28181SinkImpl *impl = NULL;
    if (!sink) {
        return 0;
    }
    impl = (Gb28181SinkImpl *)sink->impl;
    if (!impl || !impl->started) {
        return 0;
    }
    return gb28181_device_consume_external_idr_request(&impl->device_ctx);
}
