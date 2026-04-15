#include "mediaGateway.h"

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

/**
 * @description: Get current monotonic timestamp in microseconds
 * @return {static uint64_t}
 */
static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: Fill default config fields
 * @param {MediaGatewayConfig *} dst
 * @param {const MediaGatewayConfig *} src
 * @return {static void}
 */
static void fill_default_config(MediaGatewayConfig *dst, const MediaGatewayConfig *src) {
    /* Start from zero so partially-filled configs can inherit predictable defaults. */
    memset(dst, 0, sizeof(*dst));
    if (src) {
        *dst = *src;
    }

    /* If the caller leaves both protocol toggles unset, keep RTSP enabled as the legacy default. */
    if (dst->enable_rtsp == 0 && dst->enable_rtmp == 0 && dst->enable_gb28181 == 0) {
        dst->enable_rtsp = DEFAULT_ENABLE_RTSP;
    }
    if (dst->fps <= 0) {
        dst->fps = DEFAULT_ENCODE_FPS;
    }
    if (dst->bitrate <= 0) {
        dst->bitrate = DEFAULT_ENCODE_BITRATE;
    }
    if (dst->gop <= 0) {
        dst->gop = DEFAULT_ENCODE_GOP;
    }
    if (dst->rc_mode <= 0) {
        dst->rc_mode = DEFAULT_RC_MODE;
    }
    if (dst->h264_profile <= 0) {
        dst->h264_profile = DEFAULT_H264_PROFILE;
    }
    if (dst->h264_level <= 0) {
        dst->h264_level = DEFAULT_H264_LEVEL;
    }
    if (dst->h264_cabac_en <= 0) {
        dst->h264_cabac_en = DEFAULT_H264_CABAC_EN;
    }
    if (dst->low_latency_mode <= 0) {
        dst->low_latency_mode = DEFAULT_LOW_LATENCY_MODE;
    }
    if (dst->stats_interval_sec <= 0) {
        dst->stats_interval_sec = DEFAULT_STATS_INTERVAL_SEC;
    }
    if (dst->capture_retry_ms <= 0) {
        dst->capture_retry_ms = DEFAULT_CAPTURE_RETRY_MS;
    }
    if (dst->max_consecutive_failures <= 0) {
        dst->max_consecutive_failures = DEFAULT_MAX_CONSECUTIVE_FAILURES;
    }
    if (dst->record_flush_interval_frames <= 0) {
        dst->record_flush_interval_frames = DEFAULT_RECORD_FLUSH_INTERVAL_FRAMES;
    }

    /* RTMP metadata should describe the same elementary stream that the encoder produces. */
    if (dst->rtmp.video_width <= 0) {
        dst->rtmp.video_width = CAPTURE_WIDTH;
    }
    if (dst->rtmp.video_height <= 0) {
        dst->rtmp.video_height = CAPTURE_HEIGHT;
    }
    if (dst->rtmp.video_fps <= 0) {
        dst->rtmp.video_fps = dst->fps;
    }
    if (dst->rtmp.video_bitrate <= 0) {
        dst->rtmp.video_bitrate = dst->bitrate;
    }
    if (!dst->rtmp.video_codec_name) {
        dst->rtmp.video_codec_name = "H264";
    }
    if (!dst->rtmp.encoder_name) {
        dst->rtmp.encoder_name = "RKMediaGateway";
    }
}

/**
 * @description: Convert gateway config to encoder options
 * @param {const MediaGatewayConfig *} cfg
 * @param {MppEncoderOptions *} opt
 * @return {static void}
 */
static void build_encoder_options(const MediaGatewayConfig *cfg, MppEncoderOptions *opt) {
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

/**
 * @description: Reset benchmark accumulators for the current stats window
 * @param {MediaGatewayCtx *} ctx
 * @return {static void}
 */
static void bench_reset_window(MediaGatewayCtx *ctx) {
    if (!ctx) {
        return;
    }
    ctx->bench_sample_count = 0;
    ctx->bench_driver_to_dqbuf_sum_us = 0;
    ctx->bench_driver_to_dqbuf_max_us = 0;
    ctx->bench_dqbuf_to_put_sum_us = 0;
    ctx->bench_dqbuf_to_put_max_us = 0;
    ctx->bench_put_to_get_sum_us = 0;
    ctx->bench_put_to_get_max_us = 0;
    ctx->bench_dqbuf_to_get_sum_us = 0;
    ctx->bench_dqbuf_to_get_max_us = 0;
    ctx->bench_dqbuf_to_fanout_sum_us = 0;
    ctx->bench_dqbuf_to_fanout_max_us = 0;
}

/**
 * @description: Record one sampled frame latency data into benchmark accumulators
 * @param {MediaGatewayCtx *} ctx
 * @param {uint64_t} driver_to_dqbuf_us
 * @param {uint64_t} dqbuf_to_put_us
 * @param {uint64_t} put_to_get_us
 * @param {uint64_t} dqbuf_to_get_us
 * @param {uint64_t} dqbuf_to_fanout_us
 * @return {static void}
 */
static void bench_record_sample(MediaGatewayCtx *ctx,
                                uint64_t driver_to_dqbuf_us,
                                uint64_t dqbuf_to_put_us,
                                uint64_t put_to_get_us,
                                uint64_t dqbuf_to_get_us,
                                uint64_t dqbuf_to_fanout_us) {
    if (!ctx) {
        return;
    }

    ctx->bench_sample_count++;
    ctx->bench_driver_to_dqbuf_sum_us += driver_to_dqbuf_us;
    ctx->bench_dqbuf_to_put_sum_us += dqbuf_to_put_us;
    ctx->bench_put_to_get_sum_us += put_to_get_us;
    ctx->bench_dqbuf_to_get_sum_us += dqbuf_to_get_us;
    ctx->bench_dqbuf_to_fanout_sum_us += dqbuf_to_fanout_us;

    if (driver_to_dqbuf_us > ctx->bench_driver_to_dqbuf_max_us) {
        ctx->bench_driver_to_dqbuf_max_us = driver_to_dqbuf_us;
    }
    if (dqbuf_to_put_us > ctx->bench_dqbuf_to_put_max_us) {
        ctx->bench_dqbuf_to_put_max_us = dqbuf_to_put_us;
    }
    if (put_to_get_us > ctx->bench_put_to_get_max_us) {
        ctx->bench_put_to_get_max_us = put_to_get_us;
    }
    if (dqbuf_to_get_us > ctx->bench_dqbuf_to_get_max_us) {
        ctx->bench_dqbuf_to_get_max_us = dqbuf_to_get_us;
    }
    if (dqbuf_to_fanout_us > ctx->bench_dqbuf_to_fanout_max_us) {
        ctx->bench_dqbuf_to_fanout_max_us = dqbuf_to_fanout_us;
    }
}

/**
 * @description: Print benchmark summary for the current window and reset accumulators
 * @param {MediaGatewayCtx *} ctx
 * @return {static void}
 */
static void bench_log_and_reset_if_due(MediaGatewayCtx *ctx) {
    uint64_t now = 0;
    uint64_t span_us = 0;
    double sample_count = 0.0;

    if (!ctx || !ctx->bench_enable) {
        return;
    }

    now = get_now_us();
    span_us = now - ctx->bench_last_ts_us;
    if (span_us < (uint64_t)ctx->bench_print_interval_sec * 1000000ULL) {
        return;
    }

    if (ctx->bench_sample_count > 0) {
        sample_count = (double)ctx->bench_sample_count;
        printf("[BENCH] samples=%" PRIu64
               " avg_driver_to_dqbuf=%.2fus max_driver_to_dqbuf=%" PRIu64 "us"
               " avg_dqbuf_to_put=%.2fus max_dqbuf_to_put=%" PRIu64 "us"
               " avg_put_to_get=%.2fus max_put_to_get=%" PRIu64 "us"
               " avg_dqbuf_to_get=%.2fus max_dqbuf_to_get=%" PRIu64 "us"
               " avg_dqbuf_to_fanout=%.2fus max_dqbuf_to_fanout=%" PRIu64 "us\n",
               ctx->bench_sample_count,
               (double)ctx->bench_driver_to_dqbuf_sum_us / sample_count,
               ctx->bench_driver_to_dqbuf_max_us,
               (double)ctx->bench_dqbuf_to_put_sum_us / sample_count,
               ctx->bench_dqbuf_to_put_max_us,
               (double)ctx->bench_put_to_get_sum_us / sample_count,
               ctx->bench_put_to_get_max_us,
               (double)ctx->bench_dqbuf_to_get_sum_us / sample_count,
               ctx->bench_dqbuf_to_get_max_us,
               (double)ctx->bench_dqbuf_to_fanout_sum_us / sample_count,
               ctx->bench_dqbuf_to_fanout_max_us);
    } else {
        printf("[BENCH] samples=0 (no sampled frames in this interval)\n");
    }

    ctx->bench_last_ts_us = now;
    bench_reset_window(ctx);
}

static void trigger_external_idr_if_needed(MediaGatewayCtx *ctx) {
    int idx = -1;
    if (!ctx) {
        return;
    }
    idx = ctx->gb28181_sink_index;
    if (idx < 0 || idx >= ctx->sink_count) {
        return;
    }
    if (gb28181_sink_consume_external_idr_request(&ctx->sinks[idx])) {
        if (mpp_encoder_request_idr(&ctx->encoder) == 0) {
            printf("[GB28181] requested IDR from shared encoder (external mode)\n");
        } else {
            fprintf(stderr, "[GB28181] failed to request IDR from shared encoder\n");
        }
    }
}
/**
 * @description: Reinitialize encoder instance
 * @param {MediaGatewayCtx *} ctx
 * @return {static int}
 */
static int reset_encoder(MediaGatewayCtx *ctx) {
    MppEncoderOptions options;

    /* Rebuilding the encoder is the fastest recovery path after repeated MPP failures. */
    build_encoder_options(&ctx->config, &options);
    if (ctx->encoder_ready) {
        mpp_encoder_deinit(&ctx->encoder);
        ctx->encoder_ready = 0;
    }
    if (mpp_encoder_init(&ctx->encoder,
                         CAPTURE_WIDTH,
                         CAPTURE_HEIGHT,
                         ctx->config.fps,
                         ctx->config.bitrate,
                         ctx->config.gop,
                         &options) < 0) {
        return -1;
    }
    ctx->encoder_ready = 1;
    return 0;
}

/**
 * @description: Setup output protocol sinks
 * @param {MediaGatewayCtx} *ctx
 * @return {*}
 */
static int setup_sinks(MediaGatewayCtx *ctx) {
    /* Each protocol sink owns its own queue and sender thread, so setup is just registration here. */
    if (ctx->config.enable_rtsp) {
        if (ctx->sink_count >= MEDIA_GATEWAY_MAX_SINKS) {
            return -1;
        }
        if (rtsp_sink_setup(&ctx->sinks[ctx->sink_count], &ctx->config.rtsp) != 0) {
            return -1;
        }
        ctx->sink_count++;
    }
    if (ctx->config.enable_rtmp) {
#if defined(ENABLE_RTMP_SINK)
        if (ctx->sink_count >= MEDIA_GATEWAY_MAX_SINKS) {
            return -1;
        }
        if (rtmp_sink_setup(&ctx->sinks[ctx->sink_count], &ctx->config.rtmp) != 0) {
            return -1;
        }
        ctx->sink_count++;
#else
        fprintf(stderr, "[WARN] RTMP config ignored because ENABLE_RTMP is OFF at build time\n");
#endif
    }
    if (ctx->config.enable_gb28181) {
        if (ctx->sink_count >= MEDIA_GATEWAY_MAX_SINKS) {
            return -1;
        }
        if (gb28181_sink_setup(&ctx->sinks[ctx->sink_count], &ctx->config.gb28181) != 0) {
            return -1;
        }
        ctx->gb28181_sink_index = ctx->sink_count;
        ctx->sink_count++;
    }
    return (ctx->sink_count > 0) ? 0 : -1;
}

/**
 * @description: Start all sink channels
 * @param {MediaGatewayCtx *} ctx
 * @return {static int}
 */
static int start_sinks(MediaGatewayCtx *ctx) {
    int i;

    /* Start sinks after all modules are created so partial init failures can unwind cleanly. */
    for (i = 0; i < ctx->sink_count; ++i) {
        if (media_sink_start(&ctx->sinks[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

/**
 * @description: Stop all sink channels
 * @param {MediaGatewayCtx *} ctx
 * @return {static void}
 */
static void stop_sinks(MediaGatewayCtx *ctx) {
    int i;

    /* Stop all sender threads before tearing down shared encoder/capture resources. */
    for (i = 0; i < ctx->sink_count; ++i) {
        media_sink_stop(&ctx->sinks[i]);
    }
}

/**
 * @description: Release all sink resources
 * @param {MediaGatewayCtx *} ctx
 * @return {static void}
 */
static void deinit_sinks(MediaGatewayCtx *ctx) {
    int i;

    /* The generic sink layer does not own protocol-specific impl allocations, so free them here. */
    for (i = 0; i < ctx->sink_count; ++i) {
        void *impl = ctx->sinks[i].impl;
        media_sink_deinit(&ctx->sinks[i]);
        free(impl);
    }
    ctx->sink_count = 0;
}

/**
 * @description: Print per-sink statistics
 * @param {MediaGatewayCtx *} ctx
 * @return {static void}
 */
static void log_sink_stats(MediaGatewayCtx *ctx) {
    int i;

    /* Sink metrics expose protocol-specific backpressure without coupling the main loop to sink internals. */
    for (i = 0; i < ctx->sink_count; ++i) {
        MediaSinkStats stats;
        media_sink_get_stats(&ctx->sinks[i], &stats);
        printf("[SINK] name=%s connected=%d queue=%d dropped=%" PRIu64 " sent=%" PRIu64
               " bytes=%" PRIu64 " reconnects=%" PRIu64 " wait_key=%d\n",
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
 * @description: Initialize media gateway context
 * @param {MediaGatewayCtx *} ctx
 * @param {const MediaGatewayConfig *} config
 * @return {int}
 */
int media_gateway_init(MediaGatewayCtx *ctx, const MediaGatewayConfig *config) {
    MppEncoderOptions options;

    if (!ctx) {
        return -1;
    }

    /* One-time init for the whole gateway. Every module below uses the same normalized config snapshot. */
    memset(ctx, 0, sizeof(*ctx));
    fill_default_config(&ctx->config, config);
    ctx->gb28181_sink_index = -1;

    /* Capture comes first because width/height and frame availability drive the rest of the pipeline. */
    if (v4l2_capture_init(&ctx->capture) < 0) {
        goto fail;
    }
    ctx->capture_ready = 1; /* Mark v4l2 capture as initialized */

    build_encoder_options(&ctx->config, &options);
    /* The encoder is a shared producer. All output protocols fan out from the encoded H.264 stream. */
    if (mpp_encoder_init(&ctx->encoder,
                         CAPTURE_WIDTH,
                         CAPTURE_HEIGHT,
                         ctx->config.fps,
                         ctx->config.bitrate,
                         ctx->config.gop,
                         &options) < 0) {
        goto fail;
    }
    ctx->encoder_ready = 1; /* Mark MPP encoder as initialized */

    /* Create protocol sinks before entering the run loop so runtime hot path stays simple. */
    if (setup_sinks(ctx) != 0) {
        goto fail;
    }
    if (start_sinks(ctx) != 0) {
        goto fail;
    }

    /* Local recording stays optional and taps the same Annex-B stream used by network sinks. */
    if (ctx->config.record_file_path && ctx->config.record_file_path[0] != '\0') {
        ctx->record_fp = fopen(ctx->config.record_file_path, "ab");
        if (!ctx->record_fp) {
            fprintf(stderr, "[ERROR] open record file failed: %s (errno=%d)\n",
                    ctx->config.record_file_path, errno);
            goto fail;
        }
    }

    ctx->running = 1;
    ctx->stat_last_ts_us = get_now_us();
    ctx->bench_enable = MEDIA_GATEWAY_BENCH_ENABLE_DEFAULT ? 1 : 0;
    ctx->bench_sample_every = MEDIA_GATEWAY_BENCH_SAMPLE_EVERY_DEFAULT;
    ctx->bench_print_interval_sec = MEDIA_GATEWAY_BENCH_PRINT_INTERVAL_SEC_DEFAULT;
    if (ctx->bench_sample_every <= 0) {
        ctx->bench_sample_every = MEDIA_GATEWAY_BENCH_SAMPLE_EVERY_DEFAULT;
    }
    if (ctx->bench_print_interval_sec <= 0) {
        ctx->bench_print_interval_sec = MEDIA_GATEWAY_BENCH_PRINT_INTERVAL_SEC_DEFAULT;
    }
    ctx->bench_last_ts_us = ctx->stat_last_ts_us;
    bench_reset_window(ctx);
    if (ctx->bench_enable) {
        printf("[INFO] benchmark enabled sample_every=%d print_interval_sec=%d\n",
               ctx->bench_sample_every,
               ctx->bench_print_interval_sec);
    }
    if (ctx->config.config_file_path) {
        printf("[INFO] config file hook reserved: %s\n", ctx->config.config_file_path);
    }
    return 0;

fail:
    media_gateway_deinit(ctx);
    return -1;
}

/**
 * @description: Run media gateway main loop
 * @param {MediaGatewayCtx *} ctx
 * @return {int}
 */
int media_gateway_run(MediaGatewayCtx *ctx) {
    uint8_t *raw_frame = NULL;
    int raw_len = 0;
    uint64_t frame_id = 0;
    int consecutive_capture_fail = 0;
    int consecutive_encode_fail = 0;

    if (!ctx || !ctx->running) {
        return -1;
    }

    /* Main loop:
     * 1. Capture one raw NV12 frame.
     * 2. Encode it into Annex-B H.264.
     * 3. Wrap the encoded bytes in a ref-counted MediaBuffer.
     * 4. Fan the packet out to all protocol sinks without deep-copying per sink.
     */
    while (ctx->running) {
        uint8_t *h264_data = NULL;
        size_t h264_len = 0;
        int is_key_frame = 0;
        MediaBuffer *buffer = NULL;
        MediaPacket packet; /* Stack-local packet descriptor; reset content per frame without extra heap alloc/free. */
        int i;
        uint64_t dqbuf_ts_us = 0;
        uint64_t driver_to_dqbuf_us = 0;
        uint64_t encode_put_ts_us = 0;
        uint64_t encode_get_ts_us = 0;
        uint64_t bench_fanout_done_ts_us = 0;
        int bench_sample_this_frame = 0;

        /* Initialize the temporary packet descriptor used only for this dispatch iteration. */
        media_packet_init(&packet);
        /* The capture layer already returns stable copied frame memory, safe for downstream encoding. */
        if (v4l2_capture_frame(&ctx->capture,
                               &raw_frame,
                               &raw_len,
                               &frame_id,
                               &dqbuf_ts_us,
                               &driver_to_dqbuf_us) < 0) {
            consecutive_capture_fail++;
            if (consecutive_capture_fail >= ctx->config.max_consecutive_failures) {
                return -1;
            }
            usleep((useconds_t)ctx->config.capture_retry_ms * 1000U);
            continue;
        }
        consecutive_capture_fail = 0;

        /* external gb28181 会话建立后，由主线程触发一次共享编码器 IDR。 */
        trigger_external_idr_if_needed(ctx);

        /* MPP may transiently fail, so repeated errors trigger an encoder rebuild instead of full exit. */
        if (mpp_encoder_encode_frame(&ctx->encoder,
                                     raw_frame,
                                     (size_t)raw_len,
                                     frame_id,
                                     &h264_data,
                                     &h264_len,
                                     &is_key_frame,
                                     &encode_put_ts_us,
                                     &encode_get_ts_us) < 0) {
            consecutive_encode_fail++;
            if (consecutive_encode_fail >= 3) {
                if (reset_encoder(ctx) != 0) {
                    return -1;
                }
                consecutive_encode_fail = 0;
            }
            continue;
        }
        consecutive_encode_fail = 0;

        if (!h264_data || h264_len == 0) {
            continue;
        }
        /* The shared MediaBuffer lets every sink keep its own queue entry with only metadata copies. */
        /* Refcount lifecycle example: create=1, RTSP enqueue=2, RTMP enqueue=3. */
        /* Each frame allocates MediaBuffer and payload; both are freed when refcount returns to zero. */
        /* With default 30fps / 2Mbps / 2 sinks, this overhead is usually acceptable and keeps ownership simple. */
        /* Consider a memory pool for higher bitrate/fps, more sinks, or long-running jitter/fragmentation sensitivity. */
        if (media_buffer_create_copy(h264_data, h264_len, &buffer) != 0) {
            return -1;
        }

        packet.frame_type = MEDIA_FRAME_TYPE_VIDEO;
        packet.codec = MEDIA_CODEC_H264;
        packet.buffer = buffer;
        packet.frame_id = frame_id;
        packet.pts_us = dqbuf_ts_us;
        packet.dts_us = dqbuf_ts_us;
        packet.is_key_frame = is_key_frame;

        /* Each sink applies its own queueing, dropping and reconnect policy independently. */
        for (i = 0; i < ctx->sink_count; ++i) {
            media_sink_enqueue(&ctx->sinks[i], &packet);
        }
        bench_fanout_done_ts_us = get_now_us();

        /*
         * benchmark 采样策略：
         * - 仅在启用时工作；
         * - 每 N 帧采样一次，避免高频统计对实时链路造成明显扰动。
         */
        bench_sample_this_frame = (ctx->bench_enable && (frame_id % (uint64_t)ctx->bench_sample_every == 0)) ? 1 : 0;
        if (bench_sample_this_frame) {
            uint64_t dqbuf_to_put_us = 0;
            uint64_t put_to_get_us = 0;
            uint64_t dqbuf_to_get_us = 0;
            uint64_t dqbuf_to_fanout_us = 0;

            if (encode_put_ts_us >= dqbuf_ts_us) {
                dqbuf_to_put_us = encode_put_ts_us - dqbuf_ts_us;
            }
            if (encode_get_ts_us >= encode_put_ts_us) {
                put_to_get_us = encode_get_ts_us - encode_put_ts_us;
            }
            if (encode_get_ts_us >= dqbuf_ts_us) {
                dqbuf_to_get_us = encode_get_ts_us - dqbuf_ts_us;
            }
            if (bench_fanout_done_ts_us >= dqbuf_ts_us) {
                dqbuf_to_fanout_us = bench_fanout_done_ts_us - dqbuf_ts_us;
            }

            bench_record_sample(ctx,
                                driver_to_dqbuf_us,
                                dqbuf_to_put_us,
                                put_to_get_us,
                                dqbuf_to_get_us,
                                dqbuf_to_fanout_us);
        }

        /* Optional file recording is deliberately placed after dispatch so network sinks stay first-class. */
        if (ctx->record_fp) {
            size_t written = fwrite(h264_data, 1, h264_len, ctx->record_fp);
            if (written != h264_len) {
                fprintf(stderr, "[WARN] local record write short: %zu/%zu\n", written, h264_len);
            }
            if ((frame_id % (uint64_t)ctx->config.record_flush_interval_frames) == 0) {
                fflush(ctx->record_fp);
            }
        }
        ctx->stat_frames++;
        ctx->stat_bytes += h264_len;
        /* Release the main-thread reference; only sink-queue references remain afterwards. */
        /* Refcount returns to zero after all sink threads finish and reset their packets. */
        media_packet_reset(&packet);

        {
            uint64_t now = get_now_us();
            uint64_t span_us = now - ctx->stat_last_ts_us;
            /* Gateway statistics summarize total encoded output, while sink logs expose per-protocol health. */
            if (span_us >= (uint64_t)ctx->config.stats_interval_sec * 1000000ULL) {
                double span_sec = (double)span_us / 1000000.0;
                double fps = (span_sec > 0.0) ? ((double)ctx->stat_frames / span_sec) : 0.0;
                double kbps = (span_sec > 0.0) ? ((double)ctx->stat_bytes * 8.0 / 1000.0 / span_sec) : 0.0;
                printf("[STAT] fps=%.2f bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64 "\n",
                       fps, kbps, ctx->stat_frames, ctx->stat_bytes);
                log_sink_stats(ctx);
                bench_log_and_reset_if_due(ctx);
                ctx->stat_frames = 0;
                ctx->stat_bytes = 0;
                ctx->stat_last_ts_us = now;
            }
        }
    }

    return 0;
}

/**
 * @description: Request to stop media gateway main loop
 * @param {MediaGatewayCtx *} ctx
 * @return {void}
 */
void media_gateway_stop(MediaGatewayCtx *ctx) {
    if (!ctx) {
        return;
    }
    ctx->running = 0;
}

/**
 * @description: Release media gateway resources
 * @param {MediaGatewayCtx *} ctx
 * @return {void}
 */
void media_gateway_deinit(MediaGatewayCtx *ctx) {
    if (!ctx) {
        return;
    }

    /* Shutdown order matters: stop protocol workers first, then release shared producer resources. */
    stop_sinks(ctx);
    deinit_sinks(ctx);

    if (ctx->record_fp) {
        fflush(ctx->record_fp);
        fclose(ctx->record_fp);
        ctx->record_fp = NULL;
    }
    if (ctx->encoder_ready) {
        mpp_encoder_deinit(&ctx->encoder);
        ctx->encoder_ready = 0;
    }
    if (ctx->capture_ready) {
        v4l2_capture_deinit(&ctx->capture);
        ctx->capture_ready = 0;
    }
    memset(&ctx->config, 0, sizeof(ctx->config));
    ctx->running = 0;
}

/**
 * @description: Get current gateway throughput stats
 * @param {MediaGatewayCtx *} ctx
 * @param {MediaGatewayThroughput *} throughput
 * @return {void}
 */
void media_gateway_get_throughput(MediaGatewayCtx *ctx, MediaGatewayThroughput *throughput) {
    uint64_t now;
    uint64_t span_us;

    if (!ctx || !throughput) {
        return;
    }

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









