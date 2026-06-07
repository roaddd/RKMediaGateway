#include "mediaGateway.h"
#include "mediaGatewayClock.h"
#include "mediaGatewayMetrics.h"
#include "mediaGatewayPipeline.h"
#include "mediaGatewayProcess.h"
#include "mediaGatewayStats.h"

#include "audioFrameSource.h"
#include "mediaFrameSource.h"

#include "logger.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ENABLE_RTSP 1
#define DEFAULT_ENCODE_FPS 30
#define DEFAULT_ENCODE_BITRATE (2 * 1024 * 1024)
#define DEFAULT_ENCODE_GOP 30
#define DEFAULT_RC_MODE MPP_ENC_RC_MODE_CBR
#define DEFAULT_H264_PROFILE 100
#define DEFAULT_H264_LEVEL 40
#define DEFAULT_H264_CABAC_EN 1
#define DEFAULT_LOW_LATENCY_MODE 1
#define DEFAULT_STATS_INTERVAL_SEC 1
#define DEFAULT_CAPTURE_RETRY_MS 5
#define DEFAULT_MAX_CONSECUTIVE_FAILURES 30
#define DEFAULT_RECORD_FLUSH_INTERVAL_FRAMES 30
#define DEFAULT_BENCH_ENABLE 0
#define DEFAULT_BENCH_SAMPLE_EVERY 1
#define DEFAULT_BENCH_PRINT_INTERVAL_SEC 1
#define DEFAULT_ENABLE_AUDIO 1
#define DEFAULT_AUDIO_BIND_STREAM_INDEX 0
#define DEFAULT_AUDIO_RETRY_MS 5
#define DEFAULT_AUDIO_MAX_CONSECUTIVE_FAILURES 30

typedef struct {
    MediaGatewayPipeline pipeline;                               /* 运行期音视频编码流水线。 */
    MediaFrameSource frame_sources[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 视频帧源线程对象。 */
    int frame_source_inited[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES];  /* 对应视频帧源是否已初始化。 */
    int frame_source_started[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 对应视频帧源线程是否已启动。 */
    AudioFrameSource audio_source;                               /* 音频帧源线程对象。 */
    int audio_source_inited;                                     /* 音频帧源是否已初始化。 */
    int audio_source_started;                                    /* 音频帧源线程是否已启动。 */
} MediaGatewayRunResources;

/**
 * @description: 返回非空字符串；输入为空时返回 fallback。
 */
static const char *safe_str(const char *value, const char *fallback)
{
    return (value && value[0] != '\0') ? value : fallback;
}

/**
 * @description: 获取单调时钟时间戳，单位微秒。
 */
uint64_t media_gateway_get_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 归一化一个视频采集源配置。
 */
static void fill_default_capture_source(CaptureSourceConfig *dst,
                                        const CaptureSourceConfig *src,
                                        int source_idx)
{
    CaptureSourceConfig src_copy;
    int has_src = 0;

    if (src)
    {
        src_copy = *src;
        has_src = 1;
    }

    memset(dst, 0, sizeof(*dst));
    if (has_src)
        *dst = src_copy;

    dst->enabled = dst->enabled ? 1 : 0;
    dst->name = safe_str(dst->name, (source_idx == 0) ? "main_path" : "self_path");
    dst->device_path = safe_str(dst->device_path, (source_idx == 0) ? "/dev/video0" : "/dev/video1");
    if (dst->width <= 0)
        dst->width = (source_idx == 0) ? CAPTURE_WIDTH : 1280;
    if (dst->height <= 0)
        dst->height = (source_idx == 0) ? CAPTURE_HEIGHT : 720;
    if (dst->width & 1)
        dst->width -= 1;
    if (dst->height & 1)
        dst->height -= 1;
    if (dst->pixelformat == 0)
        dst->pixelformat = CAPTURE_FORMAT;
    if (dst->buffer_count <= 0)
        dst->buffer_count = V4L2_CAPTURE_BUFFER_COUNT;
    if (dst->buffer_count > V4L2_CAPTURE_BUFFER_COUNT)
        dst->buffer_count = V4L2_CAPTURE_BUFFER_COUNT;
}

/**
 * @description: 归一化一个码流配置及其协议输出子配置。
 */
static void fill_default_stream(MediaGatewayStreamConfig *dst,
                                const MediaGatewayStreamConfig *src,
                                int stream_idx)
{
    int default_width = (stream_idx == 0) ? CAPTURE_WIDTH : (CAPTURE_WIDTH / 2);
    int default_height = (stream_idx == 0) ? CAPTURE_HEIGHT : (CAPTURE_HEIGHT / 2);
    MediaGatewayStreamConfig src_copy;
    int has_src = 0;

    if (src)
    {
        src_copy = *src;
        has_src = 1;
    }

    memset(dst, 0, sizeof(*dst));
    if (has_src)
        *dst = src_copy;

    dst->enabled = dst->enabled ? 1 : 0;
    dst->name = safe_str(dst->name, (stream_idx == 0) ? "main" : "sub");
    if (!has_src || dst->source_index < 0 || dst->source_index >= MEDIA_GATEWAY_MAX_CAPTURE_SOURCES)
        dst->source_index = stream_idx;
    if (dst->width <= 0)
        dst->width = default_width;
    if (dst->height <= 0)
        dst->height = default_height;
    if (dst->width & 1)
        dst->width -= 1;
    if (dst->height & 1)
        dst->height -= 1;
    if (dst->fps <= 0)
        dst->fps = DEFAULT_ENCODE_FPS;
    if (dst->bitrate <= 0)
        dst->bitrate = (stream_idx == 0) ? DEFAULT_ENCODE_BITRATE : (DEFAULT_ENCODE_BITRATE / 2);
    if (dst->gop <= 0)
        dst->gop = DEFAULT_ENCODE_GOP;
    if (dst->rc_mode <= 0)
        dst->rc_mode = DEFAULT_RC_MODE;
    if (dst->h264_profile <= 0)
        dst->h264_profile = DEFAULT_H264_PROFILE;
    if (dst->h264_level <= 0)
        dst->h264_level = DEFAULT_H264_LEVEL;
    if (dst->h264_cabac_en <= 0)
        dst->h264_cabac_en = DEFAULT_H264_CABAC_EN;

    dst->rtsp.name = safe_str(dst->rtsp.name, (stream_idx == 0) ? "rtsp-main" : "rtsp-sub");
    dst->rtsp.session_name = safe_str(dst->rtsp.session_name, (stream_idx == 0) ? "live_main" : "live_sub");
    dst->rtsp.server_ip = safe_str(dst->rtsp.server_ip, "0.0.0.0");
    if (dst->rtsp.server_port <= 0)
        dst->rtsp.server_port = 8554;
    dst->rtsp.user = safe_str(dst->rtsp.user, "admin");
    dst->rtsp.password = safe_str(dst->rtsp.password, "123456");
    if (dst->rtsp.queue_capacity <= 0)
        dst->rtsp.queue_capacity = 32;
    if (dst->rtsp.immediate_sps_pps_on_new_client != 0)
        dst->rtsp.immediate_sps_pps_on_new_client = 1;

    dst->rtmp.name = safe_str(dst->rtmp.name, (stream_idx == 0) ? "rtmp-main" : "rtmp-sub");
    dst->rtmp.video_codec_name = safe_str(dst->rtmp.video_codec_name, "H264");
    dst->rtmp.encoder_name = safe_str(dst->rtmp.encoder_name, "RKMediaGateway");
    if (dst->rtmp.queue_capacity <= 0)
        dst->rtmp.queue_capacity = 64;
    if (dst->rtmp.reconnect_interval_ms <= 0)
        dst->rtmp.reconnect_interval_ms = 1000;
    if (dst->rtmp.connect_timeout_ms <= 0)
        dst->rtmp.connect_timeout_ms = 3000;
    if (dst->rtmp.video_width <= 0)
        dst->rtmp.video_width = dst->width;
    if (dst->rtmp.video_height <= 0)
        dst->rtmp.video_height = dst->height;
    if (dst->rtmp.video_fps <= 0)
        dst->rtmp.video_fps = dst->fps;
    if (dst->rtmp.video_bitrate <= 0)
        dst->rtmp.video_bitrate = dst->bitrate;

    dst->gb28181.name = safe_str(dst->gb28181.name, (stream_idx == 0) ? "gb28181-main" : "gb28181-sub");
    dst->gb28181.server_ip = safe_str(dst->gb28181.server_ip, "192.168.1.1");
    if (dst->gb28181.server_port <= 0)
        dst->gb28181.server_port = 5060;
    dst->gb28181.server_domain = safe_str(dst->gb28181.server_domain, "3402000000");
    dst->gb28181.server_id = safe_str(dst->gb28181.server_id, "34020000002000000001");
    dst->gb28181.device_id = safe_str(dst->gb28181.device_id, "34020000001320000001");
    dst->gb28181.device_domain = safe_str(dst->gb28181.device_domain, dst->gb28181.server_domain);
    dst->gb28181.device_password = safe_str(dst->gb28181.device_password, "12345678");
    dst->gb28181.bind_ip = safe_str(dst->gb28181.bind_ip, "0.0.0.0");
    if (dst->gb28181.local_sip_port <= 0)
        dst->gb28181.local_sip_port = 5060;
    dst->gb28181.sip_contact_ip = safe_str(dst->gb28181.sip_contact_ip, "127.0.0.1");
    dst->gb28181.media_ip = safe_str(dst->gb28181.media_ip, dst->gb28181.sip_contact_ip);
    if (dst->gb28181.media_port <= 0)
        dst->gb28181.media_port = 30000;
    if (dst->gb28181.register_expires <= 0)
        dst->gb28181.register_expires = 3600;
    if (dst->gb28181.keepalive_interval_sec <= 0)
        dst->gb28181.keepalive_interval_sec = 60;
    if (dst->gb28181.register_retry_interval_sec <= 0)
        dst->gb28181.register_retry_interval_sec = 5;
    dst->gb28181.device_name = safe_str(dst->gb28181.device_name, "RK3568 Camera");
    dst->gb28181.manufacturer = safe_str(dst->gb28181.manufacturer, "Topeet");
    dst->gb28181.model = safe_str(dst->gb28181.model, "RKMediaGateway");
    dst->gb28181.firmware = safe_str(dst->gb28181.firmware, "1.0.0");
    dst->gb28181.channel_id = safe_str(dst->gb28181.channel_id, dst->gb28181.device_id);
    dst->gb28181.user_agent = safe_str(dst->gb28181.user_agent, "RKMediaGateway-GB28181/1.0");
    if (dst->gb28181.queue_capacity <= 0)
        dst->gb28181.queue_capacity = 64;
}

/**
 * @description: 归一化 gateway 顶层配置，补齐兼容模式默认值。
 */
static void fill_default_config(MediaGatewayConfig *dst, const MediaGatewayConfig *src)
{
    int i;
    MediaGatewayConfig src_copy;
    int has_src = 0;

    if (src)
    {
        src_copy = *src;
        has_src = 1;
    }
    memset(dst, 0, sizeof(*dst));
    if (has_src)
        *dst = src_copy;

    dst->enable_rtsp = dst->enable_rtsp ? 1 : DEFAULT_ENABLE_RTSP;
    dst->enable_rtmp = dst->enable_rtmp ? 1 : 0;
    dst->enable_gb28181 = dst->enable_gb28181 ? 1 : 0;
    if (dst->fps <= 0)
        dst->fps = DEFAULT_ENCODE_FPS;
    if (dst->bitrate <= 0)
        dst->bitrate = DEFAULT_ENCODE_BITRATE;
    if (dst->gop <= 0)
        dst->gop = DEFAULT_ENCODE_GOP;
    if (dst->rc_mode <= 0)
        dst->rc_mode = DEFAULT_RC_MODE;
    if (dst->h264_profile <= 0)
        dst->h264_profile = DEFAULT_H264_PROFILE;
    if (dst->h264_level <= 0)
        dst->h264_level = DEFAULT_H264_LEVEL;
    if (dst->h264_cabac_en <= 0)
        dst->h264_cabac_en = DEFAULT_H264_CABAC_EN;
    dst->low_latency_mode = dst->low_latency_mode ? 1 : DEFAULT_LOW_LATENCY_MODE;
    if (dst->stats_interval_sec <= 0)
        dst->stats_interval_sec = DEFAULT_STATS_INTERVAL_SEC;
    if (dst->capture_retry_ms <= 0)
        dst->capture_retry_ms = DEFAULT_CAPTURE_RETRY_MS;
    if (dst->max_consecutive_failures <= 0)
        dst->max_consecutive_failures = DEFAULT_MAX_CONSECUTIVE_FAILURES;
    if (dst->record_flush_interval_frames <= 0)
        dst->record_flush_interval_frames = DEFAULT_RECORD_FLUSH_INTERVAL_FRAMES;
    dst->bench_enable = dst->bench_enable ? 1 : DEFAULT_BENCH_ENABLE;
    if (dst->bench_sample_every <= 0)
        dst->bench_sample_every = DEFAULT_BENCH_SAMPLE_EVERY;
    if (dst->bench_print_interval_sec <= 0)
        dst->bench_print_interval_sec = DEFAULT_BENCH_PRINT_INTERVAL_SEC;

    if (dst->capture_source_count <= 0)
        dst->capture_source_count = 1;
    if (dst->capture_source_count > MEDIA_GATEWAY_MAX_CAPTURE_SOURCES)
        dst->capture_source_count = MEDIA_GATEWAY_MAX_CAPTURE_SOURCES;
    for (i = 0; i < dst->capture_source_count; ++i)
        fill_default_capture_source(&dst->capture_sources[i], has_src ? &src_copy.capture_sources[i] : NULL, i);
    for (i = dst->capture_source_count; i < MEDIA_GATEWAY_MAX_CAPTURE_SOURCES; ++i)
        memset(&dst->capture_sources[i], 0, sizeof(dst->capture_sources[i]));

    dst->audio.enabled = dst->audio.enabled ? 1 : DEFAULT_ENABLE_AUDIO;
    dst->audio.device_name = safe_str(dst->audio.device_name, AUDIO_CAPTURE_DEFAULT_DEVICE);
    if (dst->audio.sample_rate <= 0)
        dst->audio.sample_rate = AUDIO_CAPTURE_DEFAULT_SAMPLE_RATE;
    if (dst->audio.channels <= 0)
        dst->audio.channels = AUDIO_CAPTURE_DEFAULT_CHANNELS;
    if (dst->audio.format == 0)
        dst->audio.format = AUDIO_SAMPLE_FORMAT_S16LE;
    if (dst->audio.period_frames <= 0)
        dst->audio.period_frames = AUDIO_CAPTURE_DEFAULT_PERIOD_FRAMES;
    if (dst->audio.buffer_periods <= 0)
        dst->audio.buffer_periods = AUDIO_CAPTURE_DEFAULT_BUFFER_PERIODS;
    if (dst->audio.source_slots <= 0)
        dst->audio.source_slots = AUDIO_FRAME_SOURCE_DEFAULT_SLOTS;
    if (dst->audio.retry_ms <= 0)
        dst->audio.retry_ms = DEFAULT_AUDIO_RETRY_MS;
    if (dst->audio.max_consecutive_failures <= 0)
        dst->audio.max_consecutive_failures = DEFAULT_AUDIO_MAX_CONSECUTIVE_FAILURES;
    if (dst->audio.codec == MEDIA_CODEC_NONE)
    {
        dst->audio.codec = (dst->audio.g711_mode == G711_ENCODER_MODE_ULAW) ? MEDIA_CODEC_G711U : MEDIA_CODEC_G711A;
    }
    if (dst->audio.codec != MEDIA_CODEC_G711A &&
        dst->audio.codec != MEDIA_CODEC_G711U &&
        dst->audio.codec != MEDIA_CODEC_AAC)
    {
        dst->audio.codec = MEDIA_CODEC_G711A;
    }
    if (dst->audio.codec == MEDIA_CODEC_G711U)
        dst->audio.g711_mode = G711_ENCODER_MODE_ULAW;
    else if (dst->audio.codec == MEDIA_CODEC_G711A)
        dst->audio.g711_mode = G711_ENCODER_MODE_ALAW;
    if (dst->audio.aac_bitrate <= 0)
        dst->audio.aac_bitrate = 32000;
    if (dst->audio.aac_profile <= 0)
        dst->audio.aac_profile = 2;
    if (dst->audio.bind_stream_index < 0 || dst->audio.bind_stream_index >= MEDIA_GATEWAY_MAX_STREAMS)
        dst->audio.bind_stream_index = DEFAULT_AUDIO_BIND_STREAM_INDEX;

    if (dst->stream_count <= 0)
    {
        MediaGatewayStreamConfig s0;
        memset(&s0, 0, sizeof(s0));
        s0.enabled = 1;
        s0.name = "main";
        s0.source_index = 0;
        s0.width = CAPTURE_WIDTH;
        s0.height = CAPTURE_HEIGHT;
        s0.fps = dst->fps;
        s0.bitrate = dst->bitrate;
        s0.gop = dst->gop;
        s0.rc_mode = dst->rc_mode;
        s0.h264_profile = dst->h264_profile;
        s0.h264_level = dst->h264_level;
        s0.h264_cabac_en = dst->h264_cabac_en;
        s0.enable_rtsp = dst->enable_rtsp;
        s0.enable_rtmp = dst->enable_rtmp;
        s0.enable_gb28181 = dst->enable_gb28181;
        s0.rtsp = dst->rtsp;
        s0.rtmp = dst->rtmp;
        s0.gb28181 = dst->gb28181;
        fill_default_stream(&dst->streams[0], &s0, 0);
        dst->stream_count = 1;
    }
    else
    {
        if (dst->stream_count > MEDIA_GATEWAY_MAX_STREAMS)
            dst->stream_count = MEDIA_GATEWAY_MAX_STREAMS;
        for (i = 0; i < dst->stream_count; ++i)
            fill_default_stream(&dst->streams[i], has_src ? &src_copy.streams[i] : &dst->streams[i], i);
    }
    for (i = dst->stream_count; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
        memset(&dst->streams[i], 0, sizeof(dst->streams[i]));

    for (i = 0; i < dst->stream_count; ++i)
    {
        if (dst->streams[i].enabled)
        {
            int source_idx = dst->streams[i].source_index;
            if (source_idx < 0 || source_idx >= dst->capture_source_count)
            {
                source_idx = 0;
                dst->streams[i].source_index = 0;
            }
            dst->capture_sources[source_idx].enabled = 1;
        }
    }
}

/**
 * @description: 为指定 stream 创建其启用的协议输出通道。
 */
static int setup_outputs_for_stream(MediaGatewayCtx *ctx, int stream_idx)
{
    const MediaGatewayStreamConfig *s = &ctx->config.streams[stream_idx];
    MediaOutputConfig output_config;
    if (!s->enabled)
        return 0;

    if (s->enable_rtsp)
    {
        if (ctx->output_count >= MEDIA_GATEWAY_MAX_OUTPUTS)
        {
            LOG_ERROR("setup_outputs_for_stream failed: RTSP output limit stream=%d count=%d max=%d",
                      stream_idx,
                      ctx->output_count,
                      MEDIA_GATEWAY_MAX_OUTPUTS);
            return -1;
        }
        memset(&output_config, 0, sizeof(output_config));
        output_config.type = MEDIA_OUTPUT_TYPE_RTSP;
        output_config.protocol.rtsp = s->rtsp;
        if (ctx->config.audio.enabled && ctx->config.audio.bind_stream_index == stream_idx)
        {
            output_config.protocol.rtsp.audio_codec = ctx->config.audio.codec;
            output_config.protocol.rtsp.audio_sample_rate = ctx->config.audio.sample_rate;
            output_config.protocol.rtsp.audio_channels = ctx->config.audio.channels;
            output_config.protocol.rtsp.aac_profile = ctx->config.audio.aac_profile;
        }
        else
        {
            output_config.protocol.rtsp.audio_codec = MEDIA_CODEC_NONE;
        }
        if (media_output_setup(&ctx->outputs[ctx->output_count], &output_config) != 0)
        {
            LOG_ERROR("setup_outputs_for_stream failed: setup RTSP stream=%d name=%s",
                      stream_idx,
                      s->name ? s->name : "unknown");
            return -1;
        }
        ctx->output_stream_index[ctx->output_count] = stream_idx;
        ctx->rtsp_output_index[stream_idx] = ctx->output_count;
        ctx->output_count++;
    }
    if (s->enable_rtmp)
    {
#if defined(ENABLE_RTMP_OUTPUT)
        if (ctx->output_count >= MEDIA_GATEWAY_MAX_OUTPUTS)
        {
            LOG_ERROR("setup_outputs_for_stream failed: RTMP output limit stream=%d count=%d max=%d",
                      stream_idx,
                      ctx->output_count,
                      MEDIA_GATEWAY_MAX_OUTPUTS);
            return -1;
        }
        memset(&output_config, 0, sizeof(output_config));
        output_config.type = MEDIA_OUTPUT_TYPE_RTMP;
        output_config.protocol.rtmp = s->rtmp;
        if (media_output_setup(&ctx->outputs[ctx->output_count], &output_config) != 0)
        {
            LOG_ERROR("setup_outputs_for_stream failed: setup RTMP stream=%d name=%s",
                      stream_idx,
                      s->name ? s->name : "unknown");
            return -1;
        }
        ctx->output_stream_index[ctx->output_count] = stream_idx;
        ctx->output_count++;
#endif
    }
    if (s->enable_gb28181)
    {
        if (ctx->output_count >= MEDIA_GATEWAY_MAX_OUTPUTS)
        {
            LOG_ERROR("setup_outputs_for_stream failed: GB28181 output limit stream=%d count=%d max=%d",
                      stream_idx,
                      ctx->output_count,
                      MEDIA_GATEWAY_MAX_OUTPUTS);
            return -1;
        }
        memset(&output_config, 0, sizeof(output_config));
        output_config.type = MEDIA_OUTPUT_TYPE_GB28181;
        output_config.protocol.gb28181 = s->gb28181;
        if (media_output_setup(&ctx->outputs[ctx->output_count], &output_config) != 0)
        {
            LOG_ERROR("setup_outputs_for_stream failed: setup GB28181 stream=%d name=%s",
                      stream_idx,
                      s->name ? s->name : "unknown");
            return -1;
        }
        ctx->output_stream_index[ctx->output_count] = stream_idx;
        ctx->gb28181_output_index[stream_idx] = ctx->output_count;
        ctx->output_count++;
    }
    return 0;
}

/**
 * @description: 为全部启用 stream 创建输出通道。
 */
static int setup_outputs(MediaGatewayCtx *ctx)
{
    int i;
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        if (!ctx->stream_enabled[i])
            continue;
        /* 为stream创建每个协议对应的输出通道 */
        if (setup_outputs_for_stream(ctx, i) != 0)
        {
            LOG_ERROR("setup_outputs failed: stream=%d name=%s",
                      i,
                      ctx->config.streams[i].name ? ctx->config.streams[i].name : "unknown");
            return -1;
        }
    }
    if (ctx->output_count <= 0)
    {
        LOG_ERROR("setup_outputs failed: no enabled output configured");
        return -1;
    }
    return 0;
}

/**
 * @description: 启动所有已创建的输出线程。
 */
static int start_outputs(MediaGatewayCtx *ctx)
{
    int i;
    for (i = 0; i < ctx->output_count; ++i)
    {
        if (media_output_start(&ctx->outputs[i]) != 0)
        {
            LOG_ERROR("start_outputs failed: idx=%d name=%s stream=%d",
                      i,
                      ctx->outputs[i].config.name ? ctx->outputs[i].config.name : "unknown",
                      ctx->output_stream_index[i]);
            return -1;
        }
    }
    return 0;
}

/**
 * @description: 停止所有输出线程。
 */
static void stop_outputs(MediaGatewayCtx *ctx)
{
    int i;
    for (i = 0; i < ctx->output_count; ++i)
        media_output_stop(&ctx->outputs[i]);
}

/**
 * @description: 释放所有输出对象及其协议实现对象。
 */
static void deinit_outputs(MediaGatewayCtx *ctx)
{
    int i;
    for (i = 0; i < ctx->output_count; ++i)
    {
        void *impl = ctx->outputs[i].impl;
        media_output_deinit(&ctx->outputs[i]);
        free(impl);
    }
    ctx->output_count = 0;
}

/**
 * @description: 打印归一化后的关键配置。
 */
static void log_effective_config(const MediaGatewayConfig *cfg)
{
    if (!cfg)
        return;
    LOG_INFO("[CFG] capture_source_count=%d stream_count=%d low_latency=%d stats_interval_sec=%d",
             cfg->capture_source_count,
             cfg->stream_count,
             cfg->low_latency_mode,
             cfg->stats_interval_sec);
}

/**
 * @description: 初始化基础上下文、统计锁和归一化配置。
 */
static int init_gateway_base(MediaGatewayCtx *ctx, const MediaGatewayConfig *config)
{
    int i;

    memset(ctx, 0, sizeof(*ctx));
    if (pthread_mutex_init(&ctx->stats_lock, NULL) != 0)
    {
        LOG_ERROR("media_gateway_init failed: pthread_mutex_init stats_lock");
        return -1;
    }
    ctx->stats_lock_ready = 1;
    fill_default_config(&ctx->config, config);
    log_set_level((LogLevel)ctx->config.log_level);
    log_effective_config(&ctx->config);
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        ctx->rtsp_output_index[i] = -1;
        ctx->gb28181_output_index[i] = -1;
    }
    return 0;
}

/**
 * @description: 初始化所有启用的视频采集设备。
 */
static int init_gateway_captures(MediaGatewayCtx *ctx)
{
    int i;

    for (i = 0; i < ctx->config.capture_source_count; ++i)
    {
        V4L2CaptureConfig capture_config;
        const CaptureSourceConfig *source = &ctx->config.capture_sources[i];
        if (!source->enabled)
            continue;

        memset(&capture_config, 0, sizeof(capture_config));
        capture_config.device_path = source->device_path;
        capture_config.width = source->width;
        capture_config.height = source->height;
        capture_config.pixelformat = source->pixelformat;
        capture_config.buffer_count = source->buffer_count;
        if (v4l2_capture_init_with_config(&ctx->captures[i], &capture_config) < 0)
        {
            LOG_ERROR("media_gateway_init failed: capture source=%d name=%s device=%s",
                      i,
                      source->name ? source->name : "unknown",
                      source->device_path ? source->device_path : "unknown");
            return -1;
        }
        ctx->capture_ready[i] = 1;
        ctx->config.capture_sources[i].width = ctx->captures[i].format.width;
        ctx->config.capture_sources[i].height = ctx->captures[i].format.height;
        ctx->config.capture_sources[i].pixelformat = ctx->captures[i].format.pixelformat;
    }
    return 0;
}

/**
 * @description: 初始化所有启用 stream 的视频编码器。
 */
static int init_gateway_video_encoders(MediaGatewayCtx *ctx)
{
    int i;

    for (i = 0; i < ctx->config.stream_count; ++i)
    {
        int source_idx;
        if (!ctx->config.streams[i].enabled)
            continue;
        source_idx = ctx->config.streams[i].source_index;
        if (source_idx < 0 || source_idx >= ctx->config.capture_source_count || !ctx->capture_ready[source_idx])
        {
            LOG_ERROR("media_gateway_init failed: stream=%d name=%s capture source not ready index=%d",
                      i,
                      ctx->config.streams[i].name ? ctx->config.streams[i].name : "unknown",
                      source_idx);
            return -1;
        }
        if (media_gateway_reset_encoder(ctx, i) != 0)
        {
            LOG_ERROR("media_gateway_init failed: reset_encoder stream=%d name=%s",
                      i,
                      ctx->config.streams[i].name ? ctx->config.streams[i].name : "unknown");
            return -1;
        }
        ctx->stream_enabled[i] = 1;
    }
    return 0;
}

/**
 * @description: 初始化音频采集设备和配置选择的音频编码器。
 */
static int init_gateway_audio(MediaGatewayCtx *ctx)
{
    if (ctx->config.audio.enabled)
    {
        AudioCaptureConfig audio_capture_config;
        G711EncoderConfig g711_config;
        AacEncoderConfig aac_config;

        memset(&audio_capture_config, 0, sizeof(audio_capture_config));
        audio_capture_config.device_name = ctx->config.audio.device_name;
        audio_capture_config.sample_rate = ctx->config.audio.sample_rate;
        audio_capture_config.channels = ctx->config.audio.channels;
        audio_capture_config.format = ctx->config.audio.format;
        audio_capture_config.period_frames = ctx->config.audio.period_frames;
        audio_capture_config.buffer_periods = ctx->config.audio.buffer_periods;
        if (audio_capture_init(&ctx->audio_capture, &audio_capture_config) != 0)
        {
            LOG_ERROR("media_gateway_init failed: audio_capture device=%s rate=%d channels=%d",
                      ctx->config.audio.device_name ? ctx->config.audio.device_name : "unknown",
                      ctx->config.audio.sample_rate,
                      ctx->config.audio.channels);
            return -1;
        }
        ctx->audio_capture_ready = 1;
        ctx->config.audio.sample_rate = ctx->audio_capture.config.sample_rate;
        ctx->config.audio.period_frames = ctx->audio_capture.config.period_frames;

        if (ctx->config.audio.codec == MEDIA_CODEC_AAC)
        {
            memset(&aac_config, 0, sizeof(aac_config));
            aac_config.sample_rate = ctx->audio_capture.config.sample_rate;
            aac_config.channels = ctx->audio_capture.config.channels;
            aac_config.bitrate = ctx->config.audio.aac_bitrate;
            aac_config.profile = ctx->config.audio.aac_profile;
            aac_config.max_samples_per_frame = ctx->audio_capture.config.period_frames;
            if (aac_encoder_init(&ctx->aac_encoder, &aac_config) != 0)
            {
                LOG_ERROR("media_gateway_init failed: aac_encoder");
                return -1;
            }
        }
        else
        {
            memset(&g711_config, 0, sizeof(g711_config));
            g711_config.mode = ctx->config.audio.g711_mode;
            g711_config.sample_rate = ctx->audio_capture.config.sample_rate;
            g711_config.channels = ctx->audio_capture.config.channels;
            g711_config.max_samples_per_frame = ctx->audio_capture.config.period_frames;
            if (g711_encoder_init(&ctx->audio_encoder, &g711_config) != 0)
            {
                LOG_ERROR("media_gateway_init failed: g711_encoder");
                return -1;
            }
        }
        ctx->audio_encoder_ready = 1;
    }
    return 0;
}

/**
 * @description: 创建并启动全部输出通道。
 */
static int init_gateway_outputs(MediaGatewayCtx *ctx)
{
    if (setup_outputs(ctx) != 0)
    {
        LOG_ERROR("init_gateway_outputs failed: setup outputs");
        return -1;
    }
    if (start_outputs(ctx) != 0)
    {
        LOG_ERROR("init_gateway_outputs failed: start outputs");
        return -1;
    }
    return 0;
}

/**
 * @description: 按配置打开本地录像文件。
 */
static int init_gateway_record_file(MediaGatewayCtx *ctx)
{
    if (ctx->config.record_file_path && ctx->config.record_file_path[0] != '\0')
    {
        ctx->record_fp = fopen(ctx->config.record_file_path, "ab");
        if (!ctx->record_fp)
        {
            LOG_ERROR("media_gateway_init failed: open record file path=%s errno=%d(%s)",
                      ctx->config.record_file_path,
                      errno,
                      strerror(errno));
            return -1;
        }
    }
    return 0;
}

/**
 * @description: 初始化运行期统计和 benchmark 窗口。
 */
static void init_gateway_runtime_stats(MediaGatewayCtx *ctx)
{
    ctx->running = 1;
    ctx->stats.last_ts_us = media_gateway_get_now_us();
    ctx->bench.enable = ctx->config.bench_enable ? 1 : 0;
    ctx->bench.sample_every = ctx->config.bench_sample_every;
    ctx->bench.print_interval_sec = ctx->config.bench_print_interval_sec;
    if (ctx->bench.sample_every <= 0)
        ctx->bench.sample_every = DEFAULT_BENCH_SAMPLE_EVERY;
    if (ctx->bench.print_interval_sec <= 0)
        ctx->bench.print_interval_sec = DEFAULT_BENCH_PRINT_INTERVAL_SEC;
    ctx->bench.last_ts_us = ctx->stats.last_ts_us;
    LOG_INFO("[CFG] bench enable=%d sample_every=%d print_interval_sec=%d",
             ctx->bench.enable,
             ctx->bench.sample_every,
             ctx->bench.print_interval_sec);
    media_gateway_bench_reset_window(ctx);
}

/**
 * @description: 初始化 media gateway 上下文和所有长期持有的资源。
 */
int media_gateway_init(MediaGatewayCtx *ctx, const MediaGatewayConfig *config)
{
    if (!ctx)
    {
        LOG_ERROR("media_gateway_init failed: ctx is NULL");
        return -1;
    }
    if (init_gateway_base(ctx, config) != 0)
    {
        LOG_ERROR("media_gateway_init failed: init gateway base");
        return -1;
    }
    if (init_gateway_captures(ctx) != 0)
    {
        LOG_ERROR("media_gateway_init failed: init gateway captures");
        goto fail;
    }
    if (init_gateway_video_encoders(ctx) != 0)
    {
        LOG_ERROR("media_gateway_init failed: init gateway video encoders");
        goto fail;
    }
    if (init_gateway_audio(ctx) != 0)
    {
        LOG_ERROR("media_gateway_init failed: init gateway audio");
        goto fail;
    }
    if (init_gateway_outputs(ctx) != 0)
    {
        LOG_ERROR("media_gateway_init failed: init gateway outputs");
        goto fail;
    }
    if (init_gateway_record_file(ctx) != 0)
    {
        LOG_ERROR("media_gateway_init failed: init gateway record file");
        goto fail;
    }

    init_gateway_runtime_stats(ctx);
    return 0;

fail:
    LOG_ERROR("media_gateway_init rollback: deinit partially initialized resources");
    media_gateway_deinit(ctx);
    return -1;
}

/**
 * @description: 启动 run 生命周期内的视频采集帧源。
 */
static int start_run_frame_sources(MediaGatewayCtx *ctx, MediaGatewayRunResources *res)
{
    int source_idx;

    for (source_idx = 0; source_idx < ctx->config.capture_source_count; ++source_idx)
    {
        if (!ctx->capture_ready[source_idx])
            continue;
        if (media_frame_source_init(&res->frame_sources[source_idx],
                                    &ctx->captures[source_idx],
                                    ctx->config.capture_sources[source_idx].name,
                                    ctx->config.capture_retry_ms,
                                    ctx->config.max_consecutive_failures) != 0)
        {
            LOG_ERROR("media_gateway_run failed: init frame source source=%d", source_idx);
            return -1;
        }
        res->frame_source_inited[source_idx] = 1;
        if (media_frame_source_start(&res->frame_sources[source_idx]) != 0)
        {
            LOG_ERROR("media_gateway_run failed: start frame source source=%d", source_idx);
            return -1;
        }
        res->frame_source_started[source_idx] = 1;
    }
    return 0;
}

/**
 * @description: 启动 run 生命周期内的音频采集帧源。
 */
static int start_run_audio_source(MediaGatewayCtx *ctx, MediaGatewayRunResources *res)
{
    if (ctx->config.audio.enabled && ctx->audio_capture_ready)
    {
        if (audio_frame_source_init(&res->audio_source,
                                    &ctx->audio_capture,
                                    ctx->config.audio.source_slots,
                                    ctx->config.audio.retry_ms,
                                    ctx->config.audio.max_consecutive_failures) != 0)
        {
            LOG_ERROR("media_gateway_run failed: init audio frame source");
            return -1;
        }
        res->audio_source_inited = 1;
        if (audio_frame_source_start(&res->audio_source) != 0)
        {
            LOG_ERROR("media_gateway_run failed: start audio frame source");
            return -1;
        }
        res->audio_source_started = 1;
    }
    return 0;
}

/**
 * @description: 从一个视频采集源取最新帧并发布到绑定的 stream 输入槽。
 */
static int dispatch_video_source_once(MediaGatewayCtx *ctx,
                                      MediaGatewayRunResources *res,
                                      int source_idx,
                                      int *got_frame)
{
    MediaFrame frame;
    int stream_idx;
    int slot_index = -1;
    int acquire_ret;
    int ret = 0;

    if (!res->frame_source_started[source_idx])
        return 0;
    /* 从视频采集线程队列里获取最新帧 */
    acquire_ret = media_frame_source_acquire_latest(&res->frame_sources[source_idx], &frame, &slot_index, 10);
    if (acquire_ret < 0)
    {
        LOG_ERROR("media_gateway_run failed: frame source stopped by fatal error source=%d", source_idx);
        return -1;
    }
    if (acquire_ret == 0)
        return 0;

    *got_frame = 1;
    for (stream_idx = 0; stream_idx < ctx->config.stream_count; ++stream_idx)
    {
        if (!ctx->stream_enabled[stream_idx])
            continue;
        if (ctx->config.streams[stream_idx].source_index != source_idx)
            continue;
        /* 将最新采集帧引用发布到对应的视频编码线程输入槽。 */
        if (media_gateway_video_input_publish(&res->pipeline.video_inputs[stream_idx], &frame) != 0)
        {
            LOG_ERROR("media_gateway_run failed: publish video frame source=%d stream=%d",
                      source_idx,
                      stream_idx);
            ret = -1;
            ctx->running = 0;
            break;
        }
    }

    media_frame_source_release(&res->frame_sources[source_idx], slot_index);
    media_frame_reset(&frame);
    return ret;
}

/**
 * @description: 非阻塞 drain 音频帧并发布到音频编码 FIFO。
 */
static int drain_audio_source_once(MediaGatewayCtx *ctx,
                                   MediaGatewayRunResources *res,
                                   int *got_frame)
{
    AudioFrame audio_frame;
    int audio_slot_index = -1;
    int audio_drain_count = 0;
    int acquire_ret;

    if (!res->audio_source_started)
        return 0;

    while (audio_drain_count < ctx->config.audio.source_slots)
    {
        audio_slot_index = -1;
        /* 从音频采集线程队列里面获取最旧帧 */
        acquire_ret = audio_frame_source_acquire(&res->audio_source, &audio_frame, &audio_slot_index, 0);
        if (acquire_ret < 0)
        {
            LOG_ERROR("media_gateway_run failed: audio frame source fatal error");
            return -1;
        }
        if (acquire_ret == 0)
            break;

        *got_frame = 1;
        /* 将音频帧发布到音频编码队列 */
        if (media_gateway_audio_queue_publish(&res->pipeline.audio_queue, &audio_frame) != 0)
        {
            LOG_ERROR("media_gateway_run failed: publish audio frame=%" PRIu64, audio_frame.frame_id);
            ctx->running = 0;
            audio_frame_source_release(&res->audio_source, audio_slot_index);
            return -1;
        }
        audio_frame_source_release(&res->audio_source, audio_slot_index);
        audio_drain_count++;
    }
    return 0;
}

/**
 * @description: 执行一次 run 主循环调度。
 * 这个函数是 run 循环的核心，负责从视频采集线程队列里获取最新帧并发布到对应的视频编码线程输入队列，
 * 没有让编码线程直接从采集线程队列获取帧的原因是多个编码线程可能对应一个采集源
 */
static int run_gateway_once(MediaGatewayCtx *ctx, MediaGatewayRunResources *res, int *got_frame)
{
    int source_idx;

    *got_frame = 0;
    for (source_idx = 0; source_idx < ctx->config.capture_source_count; ++source_idx)
    {
        /* 从视频采集线程队列里面获取最新帧并放入编码线程得到输入队列 */
        if (dispatch_video_source_once(ctx, res, source_idx, got_frame) != 0)
        {
            LOG_ERROR("run_gateway_once failed: dispatch video source=%d", source_idx);
            return -1;
        }
    }

    /* 从音频采集线程队列里面获取最新帧并放入音频编码线程输入队列 */
    if (drain_audio_source_once(ctx, res, got_frame) != 0)
    {
        LOG_ERROR("run_gateway_once failed: drain audio source");
        return -1;
    }
    if (media_gateway_pipeline_get_ret(&res->pipeline) != 0)
    {
        LOG_ERROR("run_gateway_once failed: pipeline ret");
        return -1;
    }

    pthread_mutex_lock(&ctx->stats_lock);
    media_gateway_log_throughput_if_due(ctx);
    pthread_mutex_unlock(&ctx->stats_lock);
    if (!*got_frame)
        usleep(1000);
    return 0;
}

/**
 * @description: 清理 run 生命周期内创建的短期资源。
 */
static void cleanup_run_resources(MediaGatewayCtx *ctx, MediaGatewayRunResources *res)
{
    int source_idx;

    ctx->running = 0;
    if (res->audio_source_inited)
        audio_frame_source_deinit(&res->audio_source);
    for (source_idx = 0; source_idx < MEDIA_GATEWAY_MAX_CAPTURE_SOURCES; ++source_idx)
    {
        if (res->frame_source_inited[source_idx])
            media_frame_source_deinit(&res->frame_sources[source_idx]);
    }
    media_gateway_pipeline_join_workers(&res->pipeline);
    media_gateway_pipeline_deinit(&res->pipeline);
}

/**
 * @description: 启动 gateway 运行循环，负责采集帧分发和 worker 生命周期管理。
 */
int media_gateway_run(MediaGatewayCtx *ctx)
{
    MediaGatewayRunResources res;
    int ret = 0;

    if (!ctx || !ctx->running)
    {
        LOG_ERROR("media_gateway_run failed: invalid ctx or not running");
        return -1;
    }

    memset(&res, 0, sizeof(res));
    if (media_gateway_pipeline_init(&res.pipeline, ctx) != 0)
    {
        LOG_ERROR("media_gateway_run failed: init pipeline");
        ret = -1;
        goto out;
    }
    /* 启动所有的视频编码线程和音频编码线程 */
    if (media_gateway_pipeline_start_workers(&res.pipeline) != 0)
    {
        LOG_ERROR("media_gateway_run failed: start pipeline workers");
        ret = -1;
        goto out;
    }
    /* 启动视频采集线程 */
    if (start_run_frame_sources(ctx, &res) != 0)
    {
        ret = -1;
        goto out;
    }
    /* 启动音频采集线程 */
    if (start_run_audio_source(ctx, &res) != 0)
    {
        ret = -1;
        goto out;
    }

    while (ctx->running)
    {
        int got_frame = 0;
        if (run_gateway_once(ctx, &res, &got_frame) != 0)
        {
            ret = -1;
            break;
        }
    }

out:
    cleanup_run_resources(ctx, &res);
    return ret;
}

/**
 * @description: 请求 gateway 运行循环退出。
 */
void media_gateway_stop(MediaGatewayCtx *ctx)
{
    if (!ctx)
        return;
    ctx->running = 0;
}

/**
 * @description: 释放 media gateway 初始化阶段持有的全部资源。
 */
void media_gateway_deinit(MediaGatewayCtx *ctx)
{
    int i;
    if (!ctx)
        return;

    stop_outputs(ctx);
    deinit_outputs(ctx);

    if (ctx->record_fp)
    {
        fflush(ctx->record_fp);
        fclose(ctx->record_fp);
        ctx->record_fp = NULL;
    }
    if (ctx->audio_encoder_ready)
    {
        if (ctx->config.audio.codec == MEDIA_CODEC_AAC)
            aac_encoder_deinit(&ctx->aac_encoder);
        else
            g711_encoder_deinit(&ctx->audio_encoder);
        ctx->audio_encoder_ready = 0;
    }
    if (ctx->audio_capture_ready)
    {
        audio_capture_deinit(&ctx->audio_capture);
        ctx->audio_capture_ready = 0;
    }
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        if (ctx->encoder_ready[i])
        {
            mpp_encoder_deinit(&ctx->encoders[i]);
            ctx->encoder_ready[i] = 0;
        }
        free(ctx->scaled_frame_cache[i]);
        ctx->scaled_frame_cache[i] = NULL;
        ctx->scaled_frame_cache_size[i] = 0;
    }
    for (i = 0; i < MEDIA_GATEWAY_MAX_CAPTURE_SOURCES; ++i)
    {
        if (ctx->capture_ready[i])
        {
            v4l2_capture_deinit(&ctx->captures[i]);
            ctx->capture_ready[i] = 0;
        }
    }
    if (ctx->stats_lock_ready)
    {
        pthread_mutex_destroy(&ctx->stats_lock);
        ctx->stats_lock_ready = 0;
    }
    memset(&ctx->config, 0, sizeof(ctx->config));
    ctx->running = 0;
}
