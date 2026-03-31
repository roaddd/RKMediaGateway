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
 * @description: 获取当前单调时钟时间，单位微秒
 * @return {static uint64_t}
 */
static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 填充默认配置项
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
 * @description: 将业务配置转换为编码器参数
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
}

/**
 * @description: 重新初始化编码器实例
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
 * @description: 设置不同的推流协议链路
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
        ctx->sink_count++;
    }
    return (ctx->sink_count > 0) ? 0 : -1;
}

/**
 * @description: 启动全部推流通道
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
 * @description: 停止全部推流通道
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
 * @description: 释放全部推流通道资源
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
 * @description: 输出各推流通道统计信息
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
 * @description: 初始化媒体网关上下文
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

    /* Capture comes first because width/height and frame availability drive the rest of the pipeline. */
    if (v4l2_capture_init(&ctx->capture) < 0) {
        goto fail;
    }
    ctx->capture_ready = 1; /* 设置v4l2开始捕获标志 */

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
    ctx->encoder_ready = 1; /* 设置MPP初始化完成标志 */

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
    if (ctx->config.config_file_path) {
        printf("[INFO] config file hook reserved: %s\n", ctx->config.config_file_path);
    }
    return 0;

fail:
    media_gateway_deinit(ctx);
    return -1;
}

/**
 * @description: 运行媒体网关主循环
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
        MediaPacket packet; /* 局部栈变量，每帧只重置内容，不单独 malloc/free。 */
        int i;
        uint64_t dqbuf_ts_us = 0;
        uint64_t driver_to_dqbuf_us = 0;

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

        /* MPP may transiently fail, so repeated errors trigger an encoder rebuild instead of full exit. */
        if (mpp_encoder_encode_frame(&ctx->encoder,
                                     raw_frame,
                                     (size_t)raw_len,
                                     frame_id,
                                     &h264_data,
                                     &h264_len,
                                     &is_key_frame,
                                     NULL,
                                     NULL) < 0) {
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
        /* 引用计数生命周期示例：create=1，RTSP 入队=2，RTMP 入队=3。 */
        /* 现在每一帧都会为 MediaBuffer 和 buffer->data 做一次申请，最后由引用计数归零时释放。 */
        /* 按当前默认 30fps / 2Mbps / 2 路 sink 来看，这部分开销通常还能接受，优点是实现简单、共享清晰。 */
        /* 真正需要考虑内存池的场景通常是更高码率、更高帧率、更多路输出，或者长时间运行下对抖动和碎片更敏感。 */
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
        /* 这里释放主线程手里的那一份引用；此后只剩各 sink 队列中的引用。 */
        /* 当所有 sink 线程都处理完成并各自 reset 后，引用计数最终回到 0。 */
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
                ctx->stat_frames = 0;
                ctx->stat_bytes = 0;
                ctx->stat_last_ts_us = now;
            }
        }
    }

    return 0;
}

/**
 * @description: 请求停止媒体网关主循环
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
 * @description: 释放媒体网关相关资源
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
 * @description: 获取当前网关吞吐统计信息
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






