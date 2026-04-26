#include "mediaGateway.h"

#include "mediaGatewayCaptureWorker.h"

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

static const char *safe_str(const char *value, const char *fallback) {
    /* Return configured string when valid; otherwise use fallback. */
    return (value && value[0] != '\0') ? value : fallback;
}

static uint64_t get_now_us(void) {
    /* Monotonic timestamp in microseconds for latency/throughput stats. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int ensure_scaled_frame_cache(MediaGatewayCtx *ctx, int stream_idx, size_t need_size) {
    /* Grow per-stream scale cache once and reuse it for later frames. */
    uint8_t *new_buf;
    if (!ctx || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS) {
        return -1;
    }
    if (ctx->scaled_frame_cache_size[stream_idx] >= need_size) {
        return 0;
    }
    new_buf = (uint8_t *)realloc(ctx->scaled_frame_cache[stream_idx], need_size);
    if (!new_buf) {
        fprintf(stderr, "[ERROR] realloc scaled frame cache failed stream=%d need=%zu\n", stream_idx, need_size);
        return -1;
    }
    ctx->scaled_frame_cache[stream_idx] = new_buf;
    ctx->scaled_frame_cache_size[stream_idx] = need_size;
    return 0;
}

/*
 * CPU fallback scaler.
 * Policy in this file is:
 *   1) ISP direct output (no scale needed, same resolution as capture)
 *   2) RGA scale (if enabled and available)
 *   3) CPU nearest-neighbor fallback
 */
static int scale_nv12_nearest(const uint8_t *src,
                              int src_w,
                              int src_h,
                              uint8_t *dst,
                              int dst_w,
                              int dst_h) {
    const uint8_t *src_y;
    const uint8_t *src_uv;
    uint8_t *dst_y;
    uint8_t *dst_uv;
    int x;
    int y;

    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return -1;
    }
    if ((src_w & 1) || (src_h & 1) || (dst_w & 1) || (dst_h & 1)) {
        return -1;
    }

    src_y = src;
    src_uv = src + (size_t)src_w * src_h;
    dst_y = dst;
    dst_uv = dst + (size_t)dst_w * dst_h;

    for (y = 0; y < dst_h; ++y) {
        int sy = (y * src_h) / dst_h;
        const uint8_t *src_line = src_y + (size_t)sy * src_w;
        uint8_t *dst_line = dst_y + (size_t)y * dst_w;
        for (x = 0; x < dst_w; ++x) {
            int sx = (x * src_w) / dst_w;
            dst_line[x] = src_line[sx];
        }
    }

    for (y = 0; y < dst_h / 2; ++y) {
        int sy = (y * (src_h / 2)) / (dst_h / 2);
        const uint8_t *src_line = src_uv + (size_t)sy * src_w;
        uint8_t *dst_line = dst_uv + (size_t)y * dst_w;
        for (x = 0; x < dst_w; x += 2) {
            int sx = ((x / 2) * (src_w / 2)) / (dst_w / 2);
            dst_line[x] = src_line[sx * 2];
            dst_line[x + 1] = src_line[sx * 2 + 1];
        }
    }
    return 0;
}

typedef enum {
    SCALE_PATH_ISP_DIRECT = 0,
    SCALE_PATH_RGA = 1,
    SCALE_PATH_CPU_NEAREST = 2
} ScalePath;

#if defined(ENABLE_RGA_SCALER)
/*
 * Optional external RGA hook.
 * Integrator can provide this symbol from an RGA module.
 */
__attribute__((weak)) int media_gateway_rga_scale_nv12(const uint8_t *src,
                                                        int src_w,
                                                        int src_h,
                                                        uint8_t *dst,
                                                        int dst_w,
                                                        int dst_h);
#endif

static int scale_nv12_rga_if_available(const uint8_t *src,
                                       int src_w,
                                       int src_h,
                                       uint8_t *dst,
                                       int dst_w,
                                       int dst_h) {
#if defined(ENABLE_RGA_SCALER)
    if (media_gateway_rga_scale_nv12) {
        return media_gateway_rga_scale_nv12(src, src_w, src_h, dst, dst_w, dst_h);
    }
#endif
    (void)src;
    (void)src_w;
    (void)src_h;
    (void)dst;
    (void)dst_w;
    (void)dst_h;
    return -1;
}

static int prepare_stream_encode_input(MediaGatewayCtx *ctx,
                                       int stream_idx,
                                       const uint8_t *raw_frame,
                                       size_t raw_len,
                                       const uint8_t **encode_input,
                                       size_t *encode_input_len,
                                       ScalePath *path_used) {
    const MediaGatewayStreamConfig *stream_cfg;
    size_t scaled_len;

    if (!ctx || !raw_frame || !encode_input || !encode_input_len || !path_used) return -1;
    if (stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS) return -1;

    stream_cfg = &ctx->config.streams[stream_idx];

    /*
     * ISP direct path: capture output already matches target stream size.
     * Here "ISP direct" means no extra scaling in media gateway.
     */
    if (stream_cfg->width == CAPTURE_WIDTH && stream_cfg->height == CAPTURE_HEIGHT) {
        *encode_input = raw_frame;
        *encode_input_len = raw_len;
        *path_used = SCALE_PATH_ISP_DIRECT;
        return 0;
    }

    scaled_len = (size_t)stream_cfg->width * stream_cfg->height * 3 / 2;
    if (ensure_scaled_frame_cache(ctx, stream_idx, scaled_len) != 0) return -1;

    if (scale_nv12_rga_if_available(raw_frame,
                                    CAPTURE_WIDTH,
                                    CAPTURE_HEIGHT,
                                    ctx->scaled_frame_cache[stream_idx],
                                    stream_cfg->width,
                                    stream_cfg->height) == 0) {
        *encode_input = ctx->scaled_frame_cache[stream_idx];
        *encode_input_len = scaled_len;
        *path_used = SCALE_PATH_RGA;
        return 0;
    }

    if (scale_nv12_nearest(raw_frame,
                           CAPTURE_WIDTH,
                           CAPTURE_HEIGHT,
                           ctx->scaled_frame_cache[stream_idx],
                           stream_cfg->width,
                           stream_cfg->height) != 0) {
        return -1;
    }

    *encode_input = ctx->scaled_frame_cache[stream_idx];
    *encode_input_len = scaled_len;
    *path_used = SCALE_PATH_CPU_NEAREST;
    return 0;
}

static void fill_default_stream(MediaGatewayStreamConfig *dst,
                                const MediaGatewayStreamConfig *src,
                                int stream_idx) {
    /* Normalize one stream config: defaults, bounds and protocol sub-configs. */
    int default_width = (stream_idx == 0) ? CAPTURE_WIDTH : (CAPTURE_WIDTH / 2);
    int default_height = (stream_idx == 0) ? CAPTURE_HEIGHT : (CAPTURE_HEIGHT / 2);
    MediaGatewayStreamConfig src_copy;
    int has_src = 0;

    if (src) {
        src_copy = *src;
        has_src = 1;
    }

    memset(dst, 0, sizeof(*dst));
    if (has_src) {
        *dst = src_copy;
    }

    dst->enabled = dst->enabled ? 1 : 0;
    dst->name = safe_str(dst->name, (stream_idx == 0) ? "main" : "sub");
    if (dst->width <= 0) dst->width = default_width;
    if (dst->height <= 0) dst->height = default_height;
    if (dst->width & 1) dst->width -= 1;
    if (dst->height & 1) dst->height -= 1;
    if (dst->fps <= 0) dst->fps = DEFAULT_ENCODE_FPS;
    if (dst->bitrate <= 0) dst->bitrate = (stream_idx == 0) ? DEFAULT_ENCODE_BITRATE : (DEFAULT_ENCODE_BITRATE / 2);
    if (dst->gop <= 0) dst->gop = DEFAULT_ENCODE_GOP;
    if (dst->rc_mode <= 0) dst->rc_mode = DEFAULT_RC_MODE;
    if (dst->h264_profile <= 0) dst->h264_profile = DEFAULT_H264_PROFILE;
    if (dst->h264_level <= 0) dst->h264_level = DEFAULT_H264_LEVEL;
    if (dst->h264_cabac_en <= 0) dst->h264_cabac_en = DEFAULT_H264_CABAC_EN;

    dst->rtsp.name = safe_str(dst->rtsp.name, (stream_idx == 0) ? "rtsp-main" : "rtsp-sub");
    dst->rtsp.session_name = safe_str(dst->rtsp.session_name, (stream_idx == 0) ? "live_main" : "live_sub");
    dst->rtsp.server_ip = safe_str(dst->rtsp.server_ip, "0.0.0.0");
    if (dst->rtsp.server_port <= 0) dst->rtsp.server_port = 8554;
    dst->rtsp.user = safe_str(dst->rtsp.user, "admin");
    dst->rtsp.password = safe_str(dst->rtsp.password, "123456");
    if (dst->rtsp.queue_capacity <= 0) dst->rtsp.queue_capacity = 32;
    if (dst->rtsp.immediate_sps_pps_on_new_client != 0) dst->rtsp.immediate_sps_pps_on_new_client = 1;

    dst->rtmp.name = safe_str(dst->rtmp.name, (stream_idx == 0) ? "rtmp-main" : "rtmp-sub");
    dst->rtmp.video_codec_name = safe_str(dst->rtmp.video_codec_name, "H264");
    dst->rtmp.encoder_name = safe_str(dst->rtmp.encoder_name, "RKMediaGateway");
    if (dst->rtmp.queue_capacity <= 0) dst->rtmp.queue_capacity = 64;
    if (dst->rtmp.reconnect_interval_ms <= 0) dst->rtmp.reconnect_interval_ms = 1000;
    if (dst->rtmp.connect_timeout_ms <= 0) dst->rtmp.connect_timeout_ms = 3000;
    if (dst->rtmp.video_width <= 0) dst->rtmp.video_width = dst->width;
    if (dst->rtmp.video_height <= 0) dst->rtmp.video_height = dst->height;
    if (dst->rtmp.video_fps <= 0) dst->rtmp.video_fps = dst->fps;
    if (dst->rtmp.video_bitrate <= 0) dst->rtmp.video_bitrate = dst->bitrate;

    dst->gb28181.name = safe_str(dst->gb28181.name, (stream_idx == 0) ? "gb28181-main" : "gb28181-sub");
    dst->gb28181.server_ip = safe_str(dst->gb28181.server_ip, "192.168.1.1");
    if (dst->gb28181.server_port <= 0) dst->gb28181.server_port = 5060;
    dst->gb28181.server_domain = safe_str(dst->gb28181.server_domain, "3402000000");
    dst->gb28181.server_id = safe_str(dst->gb28181.server_id, "34020000002000000001");
    dst->gb28181.device_id = safe_str(dst->gb28181.device_id, "34020000001320000001");
    dst->gb28181.device_domain = safe_str(dst->gb28181.device_domain, dst->gb28181.server_domain);
    dst->gb28181.device_password = safe_str(dst->gb28181.device_password, "12345678");
    dst->gb28181.bind_ip = safe_str(dst->gb28181.bind_ip, "0.0.0.0");
    if (dst->gb28181.local_sip_port <= 0) dst->gb28181.local_sip_port = 5060;
    dst->gb28181.sip_contact_ip = safe_str(dst->gb28181.sip_contact_ip, "127.0.0.1");
    dst->gb28181.media_ip = safe_str(dst->gb28181.media_ip, dst->gb28181.sip_contact_ip);
    if (dst->gb28181.media_port <= 0) dst->gb28181.media_port = 30000;
    if (dst->gb28181.register_expires <= 0) dst->gb28181.register_expires = 3600;
    if (dst->gb28181.keepalive_interval_sec <= 0) dst->gb28181.keepalive_interval_sec = 60;
    if (dst->gb28181.register_retry_interval_sec <= 0) dst->gb28181.register_retry_interval_sec = 5;
    dst->gb28181.device_name = safe_str(dst->gb28181.device_name, "RK3568 Camera");
    dst->gb28181.manufacturer = safe_str(dst->gb28181.manufacturer, "Topeet");
    dst->gb28181.model = safe_str(dst->gb28181.model, "RKMediaGateway");
    dst->gb28181.firmware = safe_str(dst->gb28181.firmware, "1.0.0");
    dst->gb28181.channel_id = safe_str(dst->gb28181.channel_id, dst->gb28181.device_id);
    dst->gb28181.user_agent = safe_str(dst->gb28181.user_agent, "RKMediaGateway-GB28181/1.0");
    if (dst->gb28181.queue_capacity <= 0) dst->gb28181.queue_capacity = 64;
}

static void fill_default_config(MediaGatewayConfig *dst, const MediaGatewayConfig *src) {
    /* Normalize top-level config and make sure at least one stream is valid. */
    int i;
    MediaGatewayStreamConfig s0;
    memset(dst, 0, sizeof(*dst));
    if (src) {
        *dst = *src;
    }

    if (dst->low_latency_mode <= 0) dst->low_latency_mode = DEFAULT_LOW_LATENCY_MODE;
    if (dst->stats_interval_sec <= 0) dst->stats_interval_sec = DEFAULT_STATS_INTERVAL_SEC;
    if (dst->capture_retry_ms <= 0) dst->capture_retry_ms = DEFAULT_CAPTURE_RETRY_MS;
    if (dst->max_consecutive_failures <= 0) dst->max_consecutive_failures = DEFAULT_MAX_CONSECUTIVE_FAILURES;
    if (dst->record_flush_interval_frames <= 0) dst->record_flush_interval_frames = DEFAULT_RECORD_FLUSH_INTERVAL_FRAMES;
    dst->bench_enable = dst->bench_enable ? 1 : DEFAULT_BENCH_ENABLE;
    if (dst->bench_sample_every <= 0) dst->bench_sample_every = DEFAULT_BENCH_SAMPLE_EVERY;
    if (dst->bench_print_interval_sec <= 0) dst->bench_print_interval_sec = DEFAULT_BENCH_PRINT_INTERVAL_SEC;

    if (dst->stream_count <= 0) {
        memset(&s0, 0, sizeof(s0));
        s0.enabled = 1;
        s0.name = "main";
        s0.width = CAPTURE_WIDTH;
        s0.height = CAPTURE_HEIGHT;
        s0.fps = (dst->fps > 0) ? dst->fps : DEFAULT_ENCODE_FPS;
        s0.bitrate = (dst->bitrate > 0) ? dst->bitrate : DEFAULT_ENCODE_BITRATE;
        s0.gop = (dst->gop > 0) ? dst->gop : DEFAULT_ENCODE_GOP;
        s0.rc_mode = (dst->rc_mode > 0) ? dst->rc_mode : DEFAULT_RC_MODE;
        s0.h264_profile = (dst->h264_profile > 0) ? dst->h264_profile : DEFAULT_H264_PROFILE;
        s0.h264_level = (dst->h264_level > 0) ? dst->h264_level : DEFAULT_H264_LEVEL;
        s0.h264_cabac_en = (dst->h264_cabac_en > 0) ? dst->h264_cabac_en : DEFAULT_H264_CABAC_EN;
        s0.qp_init = dst->qp_init;
        s0.qp_min = dst->qp_min;
        s0.qp_max = dst->qp_max;
        s0.qp_min_i = dst->qp_min_i;
        s0.qp_max_i = dst->qp_max_i;
        s0.qp_max_step = dst->qp_max_step;
        s0.enable_rtsp = dst->enable_rtsp;
        s0.enable_rtmp = dst->enable_rtmp;
        s0.enable_gb28181 = dst->enable_gb28181;
        s0.rtsp = dst->rtsp;
        s0.rtmp = dst->rtmp;
        s0.gb28181 = dst->gb28181;
        if (s0.enable_rtsp == 0 && s0.enable_rtmp == 0 && s0.enable_gb28181 == 0) {
            s0.enable_rtsp = DEFAULT_ENABLE_RTSP;
        }
        fill_default_stream(&dst->streams[0], &s0, 0);
        dst->stream_count = 1;
    } else {
        if (dst->stream_count > MEDIA_GATEWAY_MAX_STREAMS) dst->stream_count = MEDIA_GATEWAY_MAX_STREAMS;
        for (i = 0; i < dst->stream_count; ++i) {
            fill_default_stream(&dst->streams[i], &dst->streams[i], i);
        }
    }

    for (i = dst->stream_count; i < MEDIA_GATEWAY_MAX_STREAMS; ++i) {
        memset(&dst->streams[i], 0, sizeof(dst->streams[i]));
        dst->streams[i].name = (i == 0) ? "main" : "sub";
    }
}

static void build_encoder_options(const MediaGatewayStreamConfig *cfg, MppEncoderOptions *opt) {
    /* Translate stream config fields into encoder option struct. */
    memset(opt, 0, sizeof(*opt));
    opt->rc_mode = cfg->rc_mode;
    opt->h264_profile = cfg->h264_profile;
    opt->h264_level = cfg->h264_level;
    opt->h264_cabac_en = cfg->h264_cabac_en;
    opt->qp_init = cfg->qp_init;
    opt->qp_min = cfg->qp_min;
    opt->qp_max = cfg->qp_max;
    opt->qp_min_i = cfg->qp_min_i;
    opt->qp_max_i = cfg->qp_max_i;
    opt->qp_max_step = cfg->qp_max_step;
}

static void bench_reset_window(MediaGatewayCtx *ctx) {
    /* Reset benchmark accumulators for the next report window. */
    if (!ctx) return;
    ctx->bench_sample_count = 0;
    ctx->bench_driver_to_dqbuf_sum_us = 0;
    ctx->bench_driver_to_dqbuf_max_us = 0;
    ctx->bench_dqbuf_ioctl_sum_us = 0;
    ctx->bench_dqbuf_ioctl_max_us = 0;
    ctx->bench_capture_call_sum_us = 0;
    ctx->bench_capture_call_max_us = 0;
    ctx->bench_capture_copy_sum_us = 0;
    ctx->bench_capture_copy_max_us = 0;
    ctx->bench_dqbuf_to_put_sum_us = 0;
    ctx->bench_dqbuf_to_put_max_us = 0;
    ctx->bench_put_to_get_sum_us = 0;
    ctx->bench_put_to_get_max_us = 0;
    ctx->bench_mpp_input_copy_sum_us = 0;
    ctx->bench_mpp_input_copy_max_us = 0;
    ctx->bench_mpp_put_frame_sum_us = 0;
    ctx->bench_mpp_put_frame_max_us = 0;
    ctx->bench_mpp_get_packet_sum_us = 0;
    ctx->bench_mpp_get_packet_max_us = 0;
    ctx->bench_mpp_packet_copy_sum_us = 0;
    ctx->bench_mpp_packet_copy_max_us = 0;
    ctx->bench_mpp_total_sum_us = 0;
    ctx->bench_mpp_total_max_us = 0;
    ctx->bench_dqbuf_to_get_sum_us = 0;
    ctx->bench_dqbuf_to_get_max_us = 0;
    ctx->bench_dqbuf_to_fanout_sum_us = 0;
    ctx->bench_dqbuf_to_fanout_max_us = 0;
}

static void bench_record_sample(MediaGatewayCtx *ctx,
                                uint64_t driver_to_dqbuf_us,
                                uint64_t dqbuf_ioctl_us,
                                uint64_t capture_call_us,
                                uint64_t capture_copy_us,
                                uint64_t dqbuf_to_put_us,
                                uint64_t put_to_get_us,
                                const MppEncoderTiming *mpp_timing,
                                uint64_t dqbuf_to_get_us,
                                uint64_t dqbuf_to_fanout_us) {
    /* Accumulate one sampled frame's stage latencies. */
    if (!ctx) return;
    ctx->bench_sample_count++;
    ctx->bench_driver_to_dqbuf_sum_us += driver_to_dqbuf_us;
    ctx->bench_dqbuf_ioctl_sum_us += dqbuf_ioctl_us;
    ctx->bench_capture_call_sum_us += capture_call_us;
    ctx->bench_capture_copy_sum_us += capture_copy_us;
    ctx->bench_dqbuf_to_put_sum_us += dqbuf_to_put_us;
    ctx->bench_put_to_get_sum_us += put_to_get_us;
    if (mpp_timing) {
        ctx->bench_mpp_input_copy_sum_us += mpp_timing->input_copy_us;
        ctx->bench_mpp_put_frame_sum_us += mpp_timing->put_frame_us;
        ctx->bench_mpp_get_packet_sum_us += mpp_timing->get_packet_us;
        ctx->bench_mpp_packet_copy_sum_us += mpp_timing->packet_copy_us;
        ctx->bench_mpp_total_sum_us += mpp_timing->total_us;
    }
    ctx->bench_dqbuf_to_get_sum_us += dqbuf_to_get_us;
    ctx->bench_dqbuf_to_fanout_sum_us += dqbuf_to_fanout_us;
    if (driver_to_dqbuf_us > ctx->bench_driver_to_dqbuf_max_us) ctx->bench_driver_to_dqbuf_max_us = driver_to_dqbuf_us;
    if (dqbuf_ioctl_us > ctx->bench_dqbuf_ioctl_max_us) ctx->bench_dqbuf_ioctl_max_us = dqbuf_ioctl_us;
    if (capture_call_us > ctx->bench_capture_call_max_us) ctx->bench_capture_call_max_us = capture_call_us;
    if (capture_copy_us > ctx->bench_capture_copy_max_us) ctx->bench_capture_copy_max_us = capture_copy_us;
    if (dqbuf_to_put_us > ctx->bench_dqbuf_to_put_max_us) ctx->bench_dqbuf_to_put_max_us = dqbuf_to_put_us;
    if (put_to_get_us > ctx->bench_put_to_get_max_us) ctx->bench_put_to_get_max_us = put_to_get_us;
    if (mpp_timing) {
        if (mpp_timing->input_copy_us > ctx->bench_mpp_input_copy_max_us) ctx->bench_mpp_input_copy_max_us = mpp_timing->input_copy_us;
        if (mpp_timing->put_frame_us > ctx->bench_mpp_put_frame_max_us) ctx->bench_mpp_put_frame_max_us = mpp_timing->put_frame_us;
        if (mpp_timing->get_packet_us > ctx->bench_mpp_get_packet_max_us) ctx->bench_mpp_get_packet_max_us = mpp_timing->get_packet_us;
        if (mpp_timing->packet_copy_us > ctx->bench_mpp_packet_copy_max_us) ctx->bench_mpp_packet_copy_max_us = mpp_timing->packet_copy_us;
        if (mpp_timing->total_us > ctx->bench_mpp_total_max_us) ctx->bench_mpp_total_max_us = mpp_timing->total_us;
    }
    if (dqbuf_to_get_us > ctx->bench_dqbuf_to_get_max_us) ctx->bench_dqbuf_to_get_max_us = dqbuf_to_get_us;
    if (dqbuf_to_fanout_us > ctx->bench_dqbuf_to_fanout_max_us) ctx->bench_dqbuf_to_fanout_max_us = dqbuf_to_fanout_us;
}

static void bench_log_and_reset_if_due(MediaGatewayCtx *ctx) {
    /* Print benchmark summary on interval and then clear the window. */
    uint64_t now;
    uint64_t span_us;
    double sample_count;
    if (!ctx || !ctx->bench_enable) return;
    now = get_now_us();
    span_us = now - ctx->bench_last_ts_us;
    if (span_us < (uint64_t)ctx->bench_print_interval_sec * 1000000ULL) return;

    if (ctx->bench_sample_count > 0) {
        sample_count = (double)ctx->bench_sample_count;
        LOG_INFO("[BENCH_SUMMARY] samples=%" PRIu64
                 " avg_driver_to_dqbuf=%.2fus max_driver_to_dqbuf=%" PRIu64 "us"
                 " avg_dqbuf_ioctl=%.2fus max_dqbuf_ioctl=%" PRIu64 "us"
                 " avg_capture_call=%.2fus max_capture_call=%" PRIu64 "us"
                 " avg_capture_copy=%.2fus max_capture_copy=%" PRIu64 "us"
                 " avg_dqbuf_to_put=%.2fus max_dqbuf_to_put=%" PRIu64 "us"
                 " avg_put_to_get=%.2fus max_put_to_get=%" PRIu64 "us"
                 " avg_mpp_input_copy=%.2fus max_mpp_input_copy=%" PRIu64 "us"
                 " avg_mpp_put_frame=%.2fus max_mpp_put_frame=%" PRIu64 "us"
                 " avg_mpp_get_packet=%.2fus max_mpp_get_packet=%" PRIu64 "us"
                 " avg_mpp_packet_copy=%.2fus max_mpp_packet_copy=%" PRIu64 "us"
                 " avg_mpp_total=%.2fus max_mpp_total=%" PRIu64 "us"
                 " avg_dqbuf_to_get=%.2fus max_dqbuf_to_get=%" PRIu64 "us"
                 " avg_dqbuf_to_fanout=%.2fus max_dqbuf_to_fanout=%" PRIu64 "us",
                 ctx->bench_sample_count,
                 (double)ctx->bench_driver_to_dqbuf_sum_us / sample_count, ctx->bench_driver_to_dqbuf_max_us,
                 (double)ctx->bench_dqbuf_ioctl_sum_us / sample_count, ctx->bench_dqbuf_ioctl_max_us,
                 (double)ctx->bench_capture_call_sum_us / sample_count, ctx->bench_capture_call_max_us,
                 (double)ctx->bench_capture_copy_sum_us / sample_count, ctx->bench_capture_copy_max_us,
                 (double)ctx->bench_dqbuf_to_put_sum_us / sample_count, ctx->bench_dqbuf_to_put_max_us,
                 (double)ctx->bench_put_to_get_sum_us / sample_count, ctx->bench_put_to_get_max_us,
                 (double)ctx->bench_mpp_input_copy_sum_us / sample_count, ctx->bench_mpp_input_copy_max_us,
                 (double)ctx->bench_mpp_put_frame_sum_us / sample_count, ctx->bench_mpp_put_frame_max_us,
                 (double)ctx->bench_mpp_get_packet_sum_us / sample_count, ctx->bench_mpp_get_packet_max_us,
                 (double)ctx->bench_mpp_packet_copy_sum_us / sample_count, ctx->bench_mpp_packet_copy_max_us,
                 (double)ctx->bench_mpp_total_sum_us / sample_count, ctx->bench_mpp_total_max_us,
                 (double)ctx->bench_dqbuf_to_get_sum_us / sample_count, ctx->bench_dqbuf_to_get_max_us,
                 (double)ctx->bench_dqbuf_to_fanout_sum_us / sample_count, ctx->bench_dqbuf_to_fanout_max_us);
    } else {
        LOG_INFO("[BENCH_SUMMARY] samples=0 no sampled frames in this interval");
    }
    ctx->bench_last_ts_us = now;
    bench_reset_window(ctx);
}

static void trigger_external_idr_if_needed(MediaGatewayCtx *ctx, int stream_idx) {
    /* Bridge external sink key-frame request to encoder IDR request. */
    int sink_idx;
    int need_idr = 0;
    if (!ctx || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS) return;

    /* GB28181: 点播建立后可请求上游尽快补关键帧。 */
    sink_idx = ctx->gb28181_sink_index[stream_idx];
    if (sink_idx >= 0 && sink_idx < ctx->sink_count) {
        if (gb28181_sink_consume_external_idr_request(&ctx->sinks[sink_idx])) {
            need_idr = 1;
        }
    }

    /*
     * RTSP: 检测到“新客户端连入”后，也触发一次 IDR。
     * 这样新观看端不必长时间等待下一个自然 GOP 关键帧。
     */
    sink_idx = ctx->rtsp_sink_index[stream_idx];
    if (sink_idx >= 0 && sink_idx < ctx->sink_count) {
        if (rtsp_sink_consume_external_idr_request(&ctx->sinks[sink_idx])) {
            need_idr = 1;
        }
    }

    if (need_idr) {
        if (mpp_encoder_request_idr(&ctx->encoders[stream_idx]) != 0) {
            fprintf(stderr, "[WARN] stream=%d failed to request IDR from external sink event\n", stream_idx);
        }
    }
}

static int reset_encoder(MediaGatewayCtx *ctx, int stream_idx) {
    /* Recreate one encoder instance using current stream settings. */
    MppEncoderOptions options;
    const MediaGatewayStreamConfig *stream_cfg;
    if (!ctx || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS) return -1;
    stream_cfg = &ctx->config.streams[stream_idx];
    build_encoder_options(stream_cfg, &options);
    if (ctx->encoder_ready[stream_idx]) {
        mpp_encoder_deinit(&ctx->encoders[stream_idx]);
        ctx->encoder_ready[stream_idx] = 0;
    }
    if (mpp_encoder_init(&ctx->encoders[stream_idx],
                         stream_cfg->width,
                         stream_cfg->height,
                         stream_cfg->fps,
                         stream_cfg->bitrate,
                         stream_cfg->gop,
                         &options) < 0) {
        return -1;
    }
    ctx->encoder_ready[stream_idx] = 1;
    return 0;
}

/**
 * @description: sinks[0] = rtspSink     sink_stream_index[0] = 0         
 *               sinks[1] = rtmpSink     sink_stream_index[1] = 0
 *               sinks[2] = gb28181Sink  sink_stream_index[2] = 0
 *               sinks[3] = rtspSink     sink_stream_index[3] = 1
 *               sinks[4] = rtmpSink     sink_stream_index[4] = 1
 *               sinks[5] = gb28181Sink  sink_stream_index[5] = 1
 * @param {MediaGatewayCtx} *ctx
 * @param {int} stream_idx
 * @return {*}
 */
static int setup_sinks_for_stream(MediaGatewayCtx *ctx, int stream_idx) {
    /* Create and map protocol sinks for one enabled stream. */
    const MediaGatewayStreamConfig *s = &ctx->config.streams[stream_idx];
    if (!s->enabled) return 0;

    if (s->enable_rtsp) {
        if (ctx->sink_count >= MEDIA_GATEWAY_MAX_SINKS) {
            fprintf(stderr,
                    "[ERROR] setup_sinks_for_stream failed: too many sinks stream=%d name=%s type=rtsp max=%d\n",
                    stream_idx,
                    s->name ? s->name : "unknown",
                    MEDIA_GATEWAY_MAX_SINKS);
            return -1;
        }
        if (rtsp_sink_setup(&ctx->sinks[ctx->sink_count], &s->rtsp) != 0) {
            fprintf(stderr,
                    "[ERROR] setup_sinks_for_stream failed: rtsp_sink_setup stream=%d name=%s session=%s port=%d\n",
                    stream_idx,
                    s->name ? s->name : "unknown",
                    s->rtsp.session_name ? s->rtsp.session_name : "unknown",
                    s->rtsp.server_port);
            return -1;
        }
        ctx->sink_stream_index[ctx->sink_count] = stream_idx;
        ctx->rtsp_sink_index[stream_idx] = ctx->sink_count;
        ctx->sink_count++;
    }
    if (s->enable_rtmp) {
#if defined(ENABLE_RTMP_SINK)
        if (ctx->sink_count >= MEDIA_GATEWAY_MAX_SINKS) {
            fprintf(stderr,
                    "[ERROR] setup_sinks_for_stream failed: too many sinks stream=%d name=%s type=rtmp max=%d\n",
                    stream_idx,
                    s->name ? s->name : "unknown",
                    MEDIA_GATEWAY_MAX_SINKS);
            return -1;
        }
        if (rtmp_sink_setup(&ctx->sinks[ctx->sink_count], &s->rtmp) != 0) {
            fprintf(stderr,
                    "[ERROR] setup_sinks_for_stream failed: rtmp_sink_setup stream=%d name=%s url=%s\n",
                    stream_idx,
                    s->name ? s->name : "unknown",
                    s->rtmp.publish_url ? s->rtmp.publish_url : "");
            return -1;
        }
        ctx->sink_stream_index[ctx->sink_count] = stream_idx;
        ctx->sink_count++;
#endif
    }
    if (s->enable_gb28181) {
        if (ctx->sink_count >= MEDIA_GATEWAY_MAX_SINKS) {
            fprintf(stderr,
                    "[ERROR] setup_sinks_for_stream failed: too many sinks stream=%d name=%s type=gb28181 max=%d\n",
                    stream_idx,
                    s->name ? s->name : "unknown",
                    MEDIA_GATEWAY_MAX_SINKS);
            return -1;
        }
        if (gb28181_sink_setup(&ctx->sinks[ctx->sink_count], &s->gb28181) != 0) {
            fprintf(stderr,
                    "[ERROR] setup_sinks_for_stream failed: gb28181_sink_setup stream=%d name=%s server=%s:%d device=%s\n",
                    stream_idx,
                    s->name ? s->name : "unknown",
                    s->gb28181.server_ip ? s->gb28181.server_ip : "unknown",
                    s->gb28181.server_port,
                    s->gb28181.device_id ? s->gb28181.device_id : "unknown");
            return -1;
        }
        ctx->sink_stream_index[ctx->sink_count] = stream_idx;
        ctx->gb28181_sink_index[stream_idx] = ctx->sink_count;
        ctx->sink_count++;
    }
    return 0;
}

static int setup_sinks(MediaGatewayCtx *ctx) {
    /* Setup sinks for all enabled streams. */
    int i;
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i) {
        if (!ctx->stream_enabled[i]) continue;
        if (setup_sinks_for_stream(ctx, i) != 0) {
            fprintf(stderr, "[ERROR] setup_sinks failed: stream=%d name=%s\n",
                    i,
                    ctx->config.streams[i].name ? ctx->config.streams[i].name : "unknown");
            return -1;
        }
    }
    if (ctx->sink_count <= 0) {
        fprintf(stderr, "[ERROR] setup_sinks failed: no enabled sink configured\n");
        return -1;
    }
    return 0;
}

static int start_sinks(MediaGatewayCtx *ctx) {
    /* Start all sinks so enqueue can be consumed immediately. */
    int i;
    for (i = 0; i < ctx->sink_count; ++i) {
        if (media_sink_start(&ctx->sinks[i]) != 0) {
            fprintf(stderr, "[ERROR] start_sinks failed: idx=%d name=%s stream=%d\n",
                    i,
                    ctx->sinks[i].config.name ? ctx->sinks[i].config.name : "unknown",
                    ctx->sink_stream_index[i]);
            return -1;
        }
    }
    return 0;
}

static void stop_sinks(MediaGatewayCtx *ctx) {
    /* Stop sink workers before deinit to avoid concurrent accesses. */
    int i;
    for (i = 0; i < ctx->sink_count; ++i) media_sink_stop(&ctx->sinks[i]);
}

static void deinit_sinks(MediaGatewayCtx *ctx) {
    /* Release sink objects and their implementation payloads. */
    int i;
    for (i = 0; i < ctx->sink_count; ++i) {
        void *impl = ctx->sinks[i].impl;
        media_sink_deinit(&ctx->sinks[i]);
        free(impl);
    }
    ctx->sink_count = 0;
}

static void log_sink_stats(MediaGatewayCtx *ctx) {
    /* Periodic sink-level health and queue diagnostics. */
    int i;
    for (i = 0; i < ctx->sink_count; ++i) {
        MediaSinkStats stats;
        media_sink_get_stats(&ctx->sinks[i], &stats);
        printf("[SINK] stream=%d name=%s connected=%d queue=%d dropped=%" PRIu64 " sent=%" PRIu64
               " bytes=%" PRIu64 " reconnects=%" PRIu64 " wait_key=%d\n",
               ctx->sink_stream_index[i],
               ctx->sinks[i].config.name ? ctx->sinks[i].config.name : "unknown",
               stats.connected,
               stats.queue_depth,
               stats.dropped_frames,
               stats.sent_frames,
               stats.sent_bytes,
               stats.reconnect_count,
               stats.waiting_for_keyframe);
    }
}

/**
 * @description: 打印传入的所有配置参数
 * @param {MediaGatewayConfig} *cfg
 * @return {*}
 */
static void log_effective_config(const MediaGatewayConfig *cfg) {
    int i;
    if (!cfg) return;

    printf("[CFG] stream_count=%d low_latency=%d stats_interval_sec=%d capture_retry_ms=%d max_failures=%d bench(enable=%d sample_every=%d print_interval_sec=%d)\n",
           cfg->stream_count,
           cfg->low_latency_mode,
           cfg->stats_interval_sec,
           cfg->capture_retry_ms,
           cfg->max_consecutive_failures,
           cfg->bench_enable,
           cfg->bench_sample_every,
           cfg->bench_print_interval_sec);
    printf("[CFG] record_file=%s record_flush_interval_frames=%d\n",
           (cfg->record_file_path && cfg->record_file_path[0] != '\0') ? cfg->record_file_path : "(disabled)",
           cfg->record_flush_interval_frames);

    for (i = 0; i < cfg->stream_count && i < MEDIA_GATEWAY_MAX_STREAMS; ++i) {
        const MediaGatewayStreamConfig *s = &cfg->streams[i];
        printf("[CFG] stream=%d name=%s enabled=%d size=%dx%d fps=%d bitrate=%d gop=%d rc=%d\n",
               i,
               s->name ? s->name : "unknown",
               s->enabled,
               s->width,
               s->height,
               s->fps,
               s->bitrate,
               s->gop,
               s->rc_mode);
        printf("[CFG] stream=%d outputs rtsp=%d rtmp=%d gb28181=%d\n",
               i,
               s->enable_rtsp,
               s->enable_rtmp,
               s->enable_gb28181);
        if (s->enable_rtsp) {
            printf("[CFG] stream=%d rtsp url=rtsp://%s:%d/%s auth=%d immediate_sps_pps=%d\n",
                   i,
                   s->rtsp.server_ip ? s->rtsp.server_ip : "0.0.0.0",
                   s->rtsp.server_port,
                   s->rtsp.session_name ? s->rtsp.session_name : "live",
                   s->rtsp.auth_enable,
                   s->rtsp.immediate_sps_pps_on_new_client);
        }
        if (s->enable_rtmp) {
            printf("[CFG] stream=%d rtmp publish_url=%s\n",
                   i,
                   (s->rtmp.publish_url && s->rtmp.publish_url[0] != '\0') ? s->rtmp.publish_url : "(empty)");
        }
        if (s->enable_gb28181) {
            printf("[CFG] stream=%d gb28181 server=%s:%d device=%s local_sip=%d media=%s:%d\n",
                   i,
                   s->gb28181.server_ip ? s->gb28181.server_ip : "unknown",
                   s->gb28181.server_port,
                   s->gb28181.device_id ? s->gb28181.device_id : "unknown",
                   s->gb28181.local_sip_port,
                   s->gb28181.media_ip ? s->gb28181.media_ip : "unknown",
                   s->gb28181.media_port);
        }
    }
}

int media_gateway_init(MediaGatewayCtx *ctx, const MediaGatewayConfig *config) {
    /* Full startup: config normalize, capture/encoders/sinks, optional file record. */
    int i;
    if (!ctx) {
        fprintf(stderr, "[ERROR] media_gateway_init failed: ctx is NULL\n");
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    fill_default_config(&ctx->config, config);
    log_effective_config(&ctx->config);
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i) {
        ctx->rtsp_sink_index[i] = -1;
        ctx->gb28181_sink_index[i] = -1;
    }

    if (v4l2_capture_init(&ctx->capture) < 0) {
        fprintf(stderr, "[ERROR] media_gateway_init failed: v4l2_capture_init\n");
        goto fail;
    }
    ctx->capture_ready = 1;

    for (i = 0; i < ctx->config.stream_count; ++i) {
        if (!ctx->config.streams[i].enabled) continue;
        if (reset_encoder(ctx, i) != 0) {
            fprintf(stderr, "[ERROR] media_gateway_init failed: reset_encoder stream=%d name=%s\n",
                    i,
                    ctx->config.streams[i].name ? ctx->config.streams[i].name : "unknown");
            goto fail;
        }
        ctx->stream_enabled[i] = 1;
    }

    if (setup_sinks(ctx) != 0) {
        fprintf(stderr, "[ERROR] media_gateway_init failed: setup_sinks\n");
        goto fail;
    }
    if (start_sinks(ctx) != 0) {
        fprintf(stderr, "[ERROR] media_gateway_init failed: start_sinks\n");
        goto fail;
    }

    if (ctx->config.record_file_path && ctx->config.record_file_path[0] != '\0') {
        ctx->record_fp = fopen(ctx->config.record_file_path, "ab");
        if (!ctx->record_fp) {
            fprintf(stderr,
                    "[ERROR] media_gateway_init failed: open record file path=%s errno=%d(%s)\n",
                    ctx->config.record_file_path,
                    errno,
                    strerror(errno));
            goto fail;
        }
    }

    ctx->running = 1;
    ctx->stat_last_ts_us = get_now_us();
    ctx->bench_enable = ctx->config.bench_enable ? 1 : 0;
    ctx->bench_sample_every = ctx->config.bench_sample_every;
    ctx->bench_print_interval_sec = ctx->config.bench_print_interval_sec;
    if (ctx->bench_sample_every <= 0) ctx->bench_sample_every = DEFAULT_BENCH_SAMPLE_EVERY;
    if (ctx->bench_print_interval_sec <= 0) ctx->bench_print_interval_sec = DEFAULT_BENCH_PRINT_INTERVAL_SEC;
    ctx->bench_last_ts_us = ctx->stat_last_ts_us;
    /* 打印调试信息的配置 */
    fprintf(stderr, "[CFG] bench enable=%d sample_every=%d print_interval_sec=%d\n",
            ctx->bench_enable,
            ctx->bench_sample_every,
            ctx->bench_print_interval_sec);
    bench_reset_window(ctx);
    return 0;

fail:
    fprintf(stderr, "[ERROR] media_gateway_init rollback: deinit partially initialized resources\n");
    media_gateway_deinit(ctx);
    return -1;
}

/**
 * @description: 为指定码流准备编码输入，必要时执行缩放。
 * @param {MediaGatewayCtx *} ctx 网关上下文。
 * @param {MediaGatewayRunState *} state 运行期状态，用于记录 fallback 告警。
 * @param {int} stream_idx 码流下标。
 * @param {MediaGatewayCapturedFrame *} frame 当前采集帧。
 * @param {const uint8_t **} encode_input 输出编码输入数据指针。
 * @param {size_t *} encode_input_len 输出编码输入数据长度。
 * @return {int} 0 成功，-1 失败。
 */
static int ensure_stream_input(MediaGatewayCtx *ctx,
                               MediaGatewayRunState *state,
                               int stream_idx,
                               const MediaGatewayCapturedFrame *frame,
                               const uint8_t **encode_input,
                               size_t *encode_input_len) {
    ScalePath scale_path = SCALE_PATH_ISP_DIRECT;
    const MediaGatewayStreamConfig *stream_cfg = &ctx->config.streams[stream_idx];

    if (prepare_stream_encode_input(ctx,
                                    stream_idx,
                                    frame->raw_frame,
                                    (size_t)frame->raw_len,
                                    encode_input,
                                    encode_input_len,
                                    &scale_path) != 0) {
        fprintf(stderr, "[ERROR] media_gateway_run failed: prepare_stream_encode_input stream=%d name=%s\n",
                stream_idx,
                stream_cfg->name ? stream_cfg->name : "unknown");
        return -1;
    }

    if (scale_path == SCALE_PATH_CPU_NEAREST && !state->rga_fallback_warned[stream_idx]) {
        fprintf(stderr,
                "[WARN] stream=%d fallback to CPU nearest scaler (RGA unavailable or failed)\n",
                stream_idx);
        state->rga_fallback_warned[stream_idx] = 1;
    }
    return 0;
}

/**
 * @description: 编码指定码流的一帧数据，并在连续失败时重建对应编码器。
 * @param {MediaGatewayCtx *} ctx 网关上下文。
 * @param {MediaGatewayRunState *} state 运行期状态，用于记录连续编码失败次数。
 * @param {int} stream_idx 码流下标。
 * @param {MediaGatewayCapturedFrame *} frame 当前采集帧。
 * @param {uint8_t *} encode_input 编码输入数据。
 * @param {size_t} encode_input_len 编码输入数据长度。
 * @param {uint8_t **} h264_data 输出 H264 数据指针。
 * @param {size_t *} h264_len 输出 H264 数据长度。
 * @param {int *} is_key_frame 输出是否关键帧。
 * @param {uint64_t *} encode_put_ts_us 输出 encode_put_frame 前时间戳。
 * @param {uint64_t *} encode_get_ts_us 输出 encode_get_packet 后时间戳。
 * @param {MppEncoderTiming *} mpp_timing 输出 MPP 内部分段耗时。
 * @return {int} 0 编码成功；1 本帧编码失败但可继续；-1 编码器重建失败。
 */
static int encode_stream_frame(MediaGatewayCtx *ctx,
                               MediaGatewayRunState *state,
                               int stream_idx,
                               const MediaGatewayCapturedFrame *frame,
                               const uint8_t *encode_input,
                               size_t encode_input_len,
                               uint8_t **h264_data,
                               size_t *h264_len,
                               int *is_key_frame,
                               uint64_t *encode_put_ts_us,
                               uint64_t *encode_get_ts_us,
                               MppEncoderTiming *mpp_timing) {
    const MediaGatewayStreamConfig *stream_cfg = &ctx->config.streams[stream_idx];

    trigger_external_idr_if_needed(ctx, stream_idx);
    if (mpp_encoder_encode_frame(&ctx->encoders[stream_idx],
                                 encode_input,
                                 encode_input_len,
                                 frame->frame_id,
                                 h264_data,
                                 h264_len,
                                 is_key_frame,
                                 encode_put_ts_us,
                                 encode_get_ts_us,
                                 mpp_timing) == 0) {
        state->consecutive_encode_fail[stream_idx] = 0;
        return 0;
    }

    state->consecutive_encode_fail[stream_idx]++;
    if (state->consecutive_encode_fail[stream_idx] >= 3) {
        if (reset_encoder(ctx, stream_idx) != 0) {
            fprintf(stderr, "[ERROR] media_gateway_run failed: reset_encoder stream=%d name=%s\n",
                    stream_idx,
                    stream_cfg->name ? stream_cfg->name : "unknown");
            return -1;
        }
        state->consecutive_encode_fail[stream_idx] = 0;
    }
    return 1;
}

/**
 * @description: 将编码后的 H264 数据封装为 MediaPacket，并分发到该码流绑定的所有 sink。
 * @param {MediaGatewayCtx *} ctx 网关上下文。
 * @param {int} stream_idx 码流下标。
 * @param {MediaGatewayCapturedFrame *} frame 当前采集帧。
 * @param {uint8_t *} h264_data H264 数据指针。
 * @param {size_t} h264_len H264 数据长度。
 * @param {int} is_key_frame 是否关键帧。
 * @return {int} 0 成功，-1 失败。
 */
static int enqueue_stream_packet(MediaGatewayCtx *ctx,
                                 int stream_idx,
                                 const MediaGatewayCapturedFrame *frame,
                                 uint8_t *h264_data,
                                 size_t h264_len,
                                 int is_key_frame) {
    MediaBuffer *buffer = NULL;
    MediaPacket packet;
    int i;
    int sink_hit = 0;

    if (media_buffer_create_copy(h264_data, h264_len, &buffer) != 0) {
        fprintf(stderr, "[ERROR] media_gateway_run failed: media_buffer_create_copy stream=%d size=%zu\n",
                stream_idx,
                h264_len);
        return -1;
    }

    media_packet_init(&packet);
    packet.frame_type = MEDIA_FRAME_TYPE_VIDEO;
    packet.codec = MEDIA_CODEC_H264;
    packet.buffer = buffer;
    packet.frame_id = frame->frame_id;
    packet.pts_us = frame->dqbuf_ts_us;
    packet.dts_us = frame->dqbuf_ts_us;
    packet.is_key_frame = is_key_frame;

    for (i = 0; i < ctx->sink_count; ++i) {
        if (ctx->sink_stream_index[i] != stream_idx) continue;
        sink_hit = 1;
        media_sink_enqueue(&ctx->sinks[i], &packet);
    }

    if (sink_hit) {
        ctx->stat_frames++;
        ctx->stat_bytes += h264_len;
        ctx->stream_stat_frames[stream_idx]++;
        ctx->stream_stat_bytes[stream_idx] += h264_len;
    }
    media_packet_reset(&packet);
    return 0;
}

/**
 * @description: 按配置采样并累计指定码流的 BENCH 性能埋点。
 * @param {MediaGatewayCtx *} ctx 网关上下文。
 * @param {int} stream_idx 码流下标，目前只统计 stream 0。
 * @param {MediaGatewayCapturedFrame *} frame 当前采集帧。
 * @param {uint64_t} encode_put_ts_us encode_put_frame 前时间戳。
 * @param {uint64_t} encode_get_ts_us encode_get_packet 后时间戳。
 * @param {MppEncoderTiming *} mpp_timing MPP 内部分段耗时。
 * @return {void}
 */
static void record_stream_benchmark(MediaGatewayCtx *ctx,
                                    int stream_idx,
                                    const MediaGatewayCapturedFrame *frame,
                                    uint64_t encode_put_ts_us,
                                    uint64_t encode_get_ts_us,
                                    const MppEncoderTiming *mpp_timing) {
    uint64_t now;
    uint64_t dqbuf_to_put_us;
    uint64_t put_to_get_us;
    uint64_t dqbuf_to_get_us;
    uint64_t dqbuf_to_fanout_us;

    if (!ctx->bench_enable || stream_idx != 0) return;
    if ((frame->frame_id % (uint64_t)ctx->bench_sample_every) != 0) return;

    now = get_now_us();
    dqbuf_to_put_us = (encode_put_ts_us >= frame->dqbuf_ts_us) ? (encode_put_ts_us - frame->dqbuf_ts_us) : 0;
    put_to_get_us = (encode_get_ts_us >= encode_put_ts_us) ? (encode_get_ts_us - encode_put_ts_us) : 0;
    dqbuf_to_get_us = (encode_get_ts_us >= frame->dqbuf_ts_us) ? (encode_get_ts_us - frame->dqbuf_ts_us) : 0;
    dqbuf_to_fanout_us = (now >= frame->dqbuf_ts_us) ? (now - frame->dqbuf_ts_us) : 0;
#if 0
    /*
     * 逐帧 BENCH 日志：用于观察单帧时间线。
     * 配置 GATEWAY_BENCH_SAMPLE_EVERY=1 时每一帧都会打印。
     */
    LOG_INFO("[BENCH_FRAME] stream=%d frame=%" PRIu64
             " driver_to_dqbuf=%" PRIu64 "us"
             " dqbuf_ioctl=%" PRIu64 "us"
             " capture_call=%" PRIu64 "us"
             " capture_copy=%" PRIu64 "us"
             " dqbuf_to_put=%" PRIu64 "us"
             " put_to_get=%" PRIu64 "us"
             " mpp_input_copy=%" PRIu64 "us"
             " mpp_put_frame=%" PRIu64 "us"
             " mpp_get_packet=%" PRIu64 "us"
             " mpp_packet_copy=%" PRIu64 "us"
             " mpp_total=%" PRIu64 "us"
             " dqbuf_to_get=%" PRIu64 "us"
             " dqbuf_to_fanout=%" PRIu64 "us",
             stream_idx,
             frame->frame_id,
             frame->driver_to_dqbuf_us,
             frame->dqbuf_ioctl_us,
             frame->capture_call_us,
             frame->frame_copy_us,
             dqbuf_to_put_us,
             put_to_get_us,
             mpp_timing ? mpp_timing->input_copy_us : 0,
             mpp_timing ? mpp_timing->put_frame_us : 0,
             mpp_timing ? mpp_timing->get_packet_us : 0,
             mpp_timing ? mpp_timing->packet_copy_us : 0,
             mpp_timing ? mpp_timing->total_us : 0,
             dqbuf_to_get_us,
             dqbuf_to_fanout_us);
#endif
    bench_record_sample(ctx,
                        frame->driver_to_dqbuf_us,
                        frame->dqbuf_ioctl_us,
                        frame->capture_call_us,
                        frame->frame_copy_us,
                        dqbuf_to_put_us,
                        put_to_get_us,
                        mpp_timing,
                        dqbuf_to_get_us,
                        dqbuf_to_fanout_us);
}

/**
 * @description: 可选地把 stream 0 的 H264 码流写入本地文件，便于离线分析。
 * @param {MediaGatewayCtx *} ctx 网关上下文。
 * @param {int} stream_idx 码流下标。
 * @param {uint64_t} frame_id 当前帧号。
 * @param {uint8_t *} h264_data H264 数据指针。
 * @param {size_t} h264_len H264 数据长度。
 * @return {void}
 */
static void maybe_record_stream_file(MediaGatewayCtx *ctx,
                                     int stream_idx,
                                     uint64_t frame_id,
                                     const uint8_t *h264_data,
                                     size_t h264_len) {
    size_t written;
    if (!ctx->record_fp || stream_idx != 0) return;

    written = fwrite(h264_data, 1, h264_len, ctx->record_fp);
    if (written != h264_len) fprintf(stderr, "[WARN] local record write short: %zu/%zu\n", written, h264_len);
    if ((frame_id % (uint64_t)ctx->config.record_flush_interval_frames) == 0) fflush(ctx->record_fp);
}

/**
 * @description: 完成单个码流的一帧处理，包括准备输入、编码、分发、统计和可选录像。
 * @param {MediaGatewayCtx *} ctx 网关上下文。
 * @param {MediaGatewayRunState *} state 运行期状态。
 * @param {MediaGatewayCapturedFrame *} frame 当前采集帧。
 * @param {int} stream_idx 码流下标。
 * @return {int} 0 成功或本帧可跳过，-1 发生不可恢复错误。
 */
static int process_gateway_stream(MediaGatewayCtx *ctx,
                                  MediaGatewayRunState *state,
                                  const MediaGatewayCapturedFrame *frame,
                                  int stream_idx) {
    const uint8_t *encode_input = NULL;
    size_t encode_input_len = 0;
    uint8_t *h264_data = NULL;
    size_t h264_len = 0;
    int is_key_frame = 0;
    uint64_t encode_put_ts_us = 0;
    uint64_t encode_get_ts_us = 0;
    MppEncoderTiming mpp_timing;
    int encode_ret;

    if (!ctx->stream_enabled[stream_idx]) {
        return 0;
    }

    if (ensure_stream_input(ctx, state, stream_idx, frame, &encode_input, &encode_input_len) != 0) return -1;

    encode_ret = encode_stream_frame(ctx,
                                     state,
                                     stream_idx,
                                     frame,
                                     encode_input,
                                     encode_input_len,
                                     &h264_data,
                                     &h264_len,
                                     &is_key_frame,
                                     &encode_put_ts_us,
                                     &encode_get_ts_us,
                                     &mpp_timing);
    if (encode_ret != 0) return (encode_ret < 0) ? -1 : 0;
    if (!h264_data || h264_len == 0) return 0;

    if (enqueue_stream_packet(ctx, stream_idx, frame, h264_data, h264_len, is_key_frame) != 0) {
        return -1;
    }

    record_stream_benchmark(ctx, stream_idx, frame, encode_put_ts_us, encode_get_ts_us, &mpp_timing);
    maybe_record_stream_file(ctx, stream_idx, frame->frame_id, h264_data, h264_len);
    return 0;
}

/**
 * @description: 清空当前吞吐统计窗口。
 * @param {MediaGatewayCtx *} ctx 网关上下文。
 * @return {void}
 */
static void reset_throughput_window(MediaGatewayCtx *ctx) {
    int i;
    ctx->stat_frames = 0;
    ctx->stat_bytes = 0;
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i) {
        ctx->stream_stat_frames[i] = 0;
        ctx->stream_stat_bytes[i] = 0;
    }
}

/**
 * @description: 到达配置周期后打印吞吐、sink 状态和 BENCH 信息，并重置统计窗口。
 * @param {MediaGatewayCtx *} ctx 网关上下文。
 * @return {void}
 */
static void log_throughput_if_due(MediaGatewayCtx *ctx) {
    uint64_t now = get_now_us();
    uint64_t span_us = now - ctx->stat_last_ts_us;
    double span_sec;
    double fps;
    double kbps;
    int i;

    if (span_us < (uint64_t)ctx->config.stats_interval_sec * 1000000ULL) return;

    span_sec = (double)span_us / 1000000.0;
    fps = (span_sec > 0.0) ? ((double)ctx->stat_frames / span_sec) : 0.0;
    kbps = (span_sec > 0.0) ? ((double)ctx->stat_bytes * 8.0 / 1000.0 / span_sec) : 0.0;
    fprintf(stderr, "[STAT] total_fps=%.2f total_bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64 "\n",
           fps, kbps, ctx->stat_frames, ctx->stat_bytes);

    for (i = 0; i < ctx->config.stream_count; ++i) {
        double sfps;
        double skbps;
        if (!ctx->stream_enabled[i]) continue;
        sfps = (span_sec > 0.0) ? ((double)ctx->stream_stat_frames[i] / span_sec) : 0.0;
        skbps = (span_sec > 0.0) ? ((double)ctx->stream_stat_bytes[i] * 8.0 / 1000.0 / span_sec) : 0.0;
        fprintf(stderr, "[STAT] stream=%d name=%s fps=%.2f bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64 "\n",
                i,
               ctx->config.streams[i].name ? ctx->config.streams[i].name : "unknown",
               sfps,
               skbps,
               ctx->stream_stat_frames[i],
               ctx->stream_stat_bytes[i]);
    }

    log_sink_stats(ctx);
    bench_log_and_reset_if_due(ctx);
    reset_throughput_window(ctx);
    ctx->stat_last_ts_us = now;
}

/**
 * @description: 网关主循环。采集工作由独立 worker 线程完成，本线程只取最新帧并执行编码/分发。
 * @param {MediaGatewayCtx *} ctx 网关上下文。
 * @return {int} 0 正常退出，-1 发生不可恢复错误。
 */
int media_gateway_run(MediaGatewayCtx *ctx) {
    MediaGatewayRunState state;
    MediaGatewayCaptureWorker capture_worker;
    int worker_inited = 0;
    int ret = 0;
    MediaGatewayCapturedFrame frame;
    int stream_idx = -1;
    int slot_index = -1;
    int acquire_ret = 0;

    if (!ctx || !ctx->running) {
        fprintf(stderr, "[ERROR] media_gateway_run failed: invalid ctx or not running\n");
        return -1;
    }

    memset(&state, 0, sizeof(state));
    memset(&capture_worker, 0, sizeof(capture_worker));

    if (media_gateway_capture_worker_init(&capture_worker,
                                          &ctx->capture,
                                          ctx->config.capture_retry_ms,
                                          ctx->config.max_consecutive_failures) != 0) {
        fprintf(stderr, "[ERROR] media_gateway_run failed: init capture worker\n");
        return -1;
    }
    worker_inited = 1;
    // 启动 capture worker 后才能进入主循环，否则可能错过早期错误导致无法正确退出。
    if (media_gateway_capture_worker_start(&capture_worker) != 0) {
        fprintf(stderr, "[ERROR] media_gateway_run failed: start capture worker\n");
        ret = -1;
        goto out;
    }

    while (ctx->running)
    {
        /*
         * 最多等待 100ms，避免没有新帧时主循环长时间卡住。
         * 超时不算错误，只用于继续刷新统计窗口并检查退出标志。
         */
        acquire_ret = media_gateway_capture_worker_acquire_latest(&capture_worker, &frame, &slot_index, 100);
        if (acquire_ret < 0) {
            fprintf(stderr, "[ERROR] media_gateway_run failed: capture worker stopped by fatal error\n");
            ret = -1;
            break;
        }
        if (acquire_ret == 0) {
            log_throughput_if_due(ctx);
            continue;
        }

        /*
         * frame.raw_frame 指向 worker 槽位缓存，必须在 release 之前完成所有码流处理。
         * 当前策略是只处理最新帧，编码来不及时允许丢旧帧，以保证实时性优先。
         */
        for (stream_idx = 0; stream_idx < ctx->config.stream_count; ++stream_idx) {
            if (process_gateway_stream(ctx, &state, &frame, stream_idx) != 0) {
                fprintf(stderr, "[ERROR] media_gateway_run failed: process_gateway_stream error\n");
                ret = -1;
                break;
            }
        }

        media_gateway_capture_worker_release(&capture_worker, slot_index);
        log_throughput_if_due(ctx);
        if (ret != 0) break;
    }

out:
    if (worker_inited) media_gateway_capture_worker_deinit(&capture_worker);
    return ret;
}

void media_gateway_stop(MediaGatewayCtx *ctx) {
    /* Signal run loop to exit gracefully. */
    if (!ctx) return;
    ctx->running = 0;
}

void media_gateway_deinit(MediaGatewayCtx *ctx) {
    /* Full teardown in safe order: sinks -> record file -> encoders -> capture. */
    int i;
    if (!ctx) return;

    stop_sinks(ctx);
    deinit_sinks(ctx);

    if (ctx->record_fp) {
        fflush(ctx->record_fp);
        fclose(ctx->record_fp);
        ctx->record_fp = NULL;
    }
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i) {
        if (ctx->encoder_ready[i]) {
            mpp_encoder_deinit(&ctx->encoders[i]);
            ctx->encoder_ready[i] = 0;
        }
        if (ctx->scaled_frame_cache[i]) {
            free(ctx->scaled_frame_cache[i]);
            ctx->scaled_frame_cache[i] = NULL;
        }
        ctx->scaled_frame_cache_size[i] = 0;
    }
    if (ctx->capture_ready) {
        v4l2_capture_deinit(&ctx->capture);
        ctx->capture_ready = 0;
    }
    memset(&ctx->config, 0, sizeof(ctx->config));
    ctx->running = 0;
}

void media_gateway_get_throughput(MediaGatewayCtx *ctx, MediaGatewayThroughput *throughput) {
    /* Export current window throughput estimate without mutating counters. */
    uint64_t now;
    uint64_t span_us;
    if (!ctx || !throughput) return;

    memset(throughput, 0, sizeof(*throughput));
    now = get_now_us();
    span_us = now - ctx->stat_last_ts_us;
    throughput->frames = ctx->stat_frames;
    throughput->bytes = ctx->stat_bytes;
    if (span_us > 0) {
        double span_sec = (double)span_us / 1000000.0;
        throughput->fps = (double)ctx->stat_frames / span_sec;
        throughput->bitrate_kbps = (double)ctx->stat_bytes * 8.0 / 1000.0 / span_sec;
    }
}
