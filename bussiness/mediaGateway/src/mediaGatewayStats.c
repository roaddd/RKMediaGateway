#include "mediaGatewayStats.h"

#include "mediaGatewayClock.h"
#include "mediaGatewayMetrics.h"

#include "logger.h"

#include <inttypes.h>
#include <string.h>

static void reset_throughput_window(MediaGatewayCtx *ctx)
{
    int i;

    ctx->stats.frames = 0;
    ctx->stats.bytes = 0;
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        ctx->stats.stream_frames[i] = 0;
        ctx->stats.stream_bytes[i] = 0;
    }
    ctx->stats.audio_frames = 0;
    ctx->stats.audio_bytes = 0;
}

static void log_output_stats(MediaGatewayCtx *ctx)
{
    int i;

    for (i = 0; i < ctx->output_count; ++i)
    {
        MediaOutputStats stats;
        media_output_get_stats(&ctx->outputs[i], &stats);
        LOG_INFO("[OUTPUT] idx=%d stream=%d name=%s type=%d connected=%d queue=%d sent=%" PRIu64
                 " bytes=%" PRIu64 " dropped=%" PRIu64 " send_failures=%" PRIu64
                 " reconnects=%" PRIu64 " wait_key=%d",
                 i,
                 ctx->output_stream_index[i],
                 ctx->outputs[i].config.name ? ctx->outputs[i].config.name : "unknown",
                 ctx->outputs[i].type,
                 stats.connected,
                 stats.queue_depth,
                 stats.sent_frames,
                 stats.sent_bytes,
                 stats.dropped_frames,
                 stats.send_failures,
                 stats.reconnect_count,
                 stats.waiting_for_keyframe);
    }
}

void media_gateway_log_throughput_if_due(MediaGatewayCtx *ctx)
{
    uint64_t now = media_gateway_get_now_us();
    uint64_t span_us = now - ctx->stats.last_ts_us;
    double span_sec;
    double fps;
    double kbps;
    int i;

    if (span_us < (uint64_t)ctx->config.stats_interval_sec * 1000000ULL)
        return;

    span_sec = (double)span_us / 1000000.0;
    fps = (span_sec > 0.0) ? ((double)ctx->stats.frames / span_sec) : 0.0;
    kbps = (span_sec > 0.0) ? ((double)ctx->stats.bytes * 8.0 / 1000.0 / span_sec) : 0.0;
    LOG_INFO("[STAT] total_fps=%.2f total_bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64,
             fps, kbps, ctx->stats.frames, ctx->stats.bytes);

    for (i = 0; i < ctx->config.stream_count; ++i)
    {
        double sfps;
        double skbps;
        if (!ctx->stream_enabled[i])
            continue;
        sfps = (span_sec > 0.0) ? ((double)ctx->stats.stream_frames[i] / span_sec) : 0.0;
        skbps = (span_sec > 0.0) ? ((double)ctx->stats.stream_bytes[i] * 8.0 / 1000.0 / span_sec) : 0.0;
        LOG_WARN("[STAT] stream=%d name=%s fps=%.2f bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64,
                 i,
                 ctx->config.streams[i].name ? ctx->config.streams[i].name : "unknown",
                 sfps,
                 skbps,
                 ctx->stats.stream_frames[i],
                 ctx->stats.stream_bytes[i]);
    }
    if (ctx->config.audio.enabled)
    {
        double afps = (span_sec > 0.0) ? ((double)ctx->stats.audio_frames / span_sec) : 0.0;
        double akbps = (span_sec > 0.0) ? ((double)ctx->stats.audio_bytes * 8.0 / 1000.0 / span_sec) : 0.0;
        LOG_WARN("[STAT] audio fps=%.2f bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64,
                 afps,
                 akbps,
                 ctx->stats.audio_frames,
                 ctx->stats.audio_bytes);
    }

    log_output_stats(ctx);
    media_gateway_bench_log_and_reset_if_due(ctx);
    reset_throughput_window(ctx);
    ctx->stats.last_ts_us = now;
}

void media_gateway_get_throughput(MediaGatewayCtx *ctx, MediaGatewayThroughput *throughput)
{
    uint64_t now;
    uint64_t span_us;

    if (!ctx || !throughput)
        return;

    memset(throughput, 0, sizeof(*throughput));
    if (ctx->stats_lock_ready)
        pthread_mutex_lock(&ctx->stats_lock);
    now = media_gateway_get_now_us();
    span_us = now - ctx->stats.last_ts_us;
    throughput->frames = ctx->stats.frames;
    throughput->bytes = ctx->stats.bytes;
    if (span_us > 0)
    {
        double span_sec = (double)span_us / 1000000.0;
        throughput->fps = (double)ctx->stats.frames / span_sec;
        throughput->bitrate_kbps = (double)ctx->stats.bytes * 8.0 / 1000.0 / span_sec;
    }
    if (ctx->stats_lock_ready)
        pthread_mutex_unlock(&ctx->stats_lock);
}
