#include "mediaGatewayStats.h"

#include "mediaGatewayClock.h"

#include "logger.h"

#include <inttypes.h>
#include <string.h>

/**
 * @description: 清空吞吐统计窗口。
 */
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

/**
 * @description: 清空 benchmark 统计窗口。
 */
void media_gateway_bench_reset_window(MediaGatewayCtx *ctx)
{
    if (!ctx)
        return;
    ctx->bench.sample_count = 0;
    ctx->bench.driver_to_dqbuf_sum_us = 0;
    ctx->bench.driver_to_dqbuf_max_us = 0;
    ctx->bench.dqbuf_ioctl_sum_us = 0;
    ctx->bench.dqbuf_ioctl_max_us = 0;
    ctx->bench.capture_call_sum_us = 0;
    ctx->bench.capture_call_max_us = 0;
    ctx->bench.capture_copy_sum_us = 0;
    ctx->bench.capture_copy_max_us = 0;
    ctx->bench.dqbuf_to_put_sum_us = 0;
    ctx->bench.dqbuf_to_put_max_us = 0;
    ctx->bench.put_to_get_sum_us = 0;
    ctx->bench.put_to_get_max_us = 0;
    ctx->bench.mpp_input_copy_sum_us = 0;
    ctx->bench.mpp_input_copy_max_us = 0;
    ctx->bench.mpp_put_frame_sum_us = 0;
    ctx->bench.mpp_put_frame_max_us = 0;
    ctx->bench.mpp_get_packet_sum_us = 0;
    ctx->bench.mpp_get_packet_max_us = 0;
    ctx->bench.mpp_packet_copy_sum_us = 0;
    ctx->bench.mpp_packet_copy_max_us = 0;
    ctx->bench.mpp_total_sum_us = 0;
    ctx->bench.mpp_total_max_us = 0;
    ctx->bench.dqbuf_to_get_sum_us = 0;
    ctx->bench.dqbuf_to_get_max_us = 0;
    ctx->bench.dqbuf_to_fanout_sum_us = 0;
    ctx->bench.dqbuf_to_fanout_max_us = 0;
}

/**
 * @description: 累计一帧 benchmark 样本。
 */
void media_gateway_bench_record_sample(MediaGatewayCtx *ctx,
                                       uint64_t driver_to_dqbuf_us,
                                       uint64_t dqbuf_ioctl_us,
                                       uint64_t capture_call_us,
                                       uint64_t capture_copy_us,
                                       uint64_t dqbuf_to_put_us,
                                       uint64_t put_to_get_us,
                                       const MppEncoderTiming *mpp_timing,
                                       uint64_t dqbuf_to_get_us,
                                       uint64_t dqbuf_to_fanout_us)
{
    if (!ctx)
        return;
    ctx->bench.sample_count++;
    ctx->bench.driver_to_dqbuf_sum_us += driver_to_dqbuf_us;
    ctx->bench.dqbuf_ioctl_sum_us += dqbuf_ioctl_us;
    ctx->bench.capture_call_sum_us += capture_call_us;
    ctx->bench.capture_copy_sum_us += capture_copy_us;
    ctx->bench.dqbuf_to_put_sum_us += dqbuf_to_put_us;
    ctx->bench.put_to_get_sum_us += put_to_get_us;
    if (mpp_timing)
    {
        ctx->bench.mpp_input_copy_sum_us += mpp_timing->input_copy_us;
        ctx->bench.mpp_put_frame_sum_us += mpp_timing->put_frame_us;
        ctx->bench.mpp_get_packet_sum_us += mpp_timing->get_packet_us;
        ctx->bench.mpp_packet_copy_sum_us += mpp_timing->packet_copy_us;
        ctx->bench.mpp_total_sum_us += mpp_timing->total_us;
    }
    ctx->bench.dqbuf_to_get_sum_us += dqbuf_to_get_us;
    ctx->bench.dqbuf_to_fanout_sum_us += dqbuf_to_fanout_us;
    if (driver_to_dqbuf_us > ctx->bench.driver_to_dqbuf_max_us)
        ctx->bench.driver_to_dqbuf_max_us = driver_to_dqbuf_us;
    if (dqbuf_ioctl_us > ctx->bench.dqbuf_ioctl_max_us)
        ctx->bench.dqbuf_ioctl_max_us = dqbuf_ioctl_us;
    if (capture_call_us > ctx->bench.capture_call_max_us)
        ctx->bench.capture_call_max_us = capture_call_us;
    if (capture_copy_us > ctx->bench.capture_copy_max_us)
        ctx->bench.capture_copy_max_us = capture_copy_us;
    if (dqbuf_to_put_us > ctx->bench.dqbuf_to_put_max_us)
        ctx->bench.dqbuf_to_put_max_us = dqbuf_to_put_us;
    if (put_to_get_us > ctx->bench.put_to_get_max_us)
        ctx->bench.put_to_get_max_us = put_to_get_us;
    if (mpp_timing)
    {
        if (mpp_timing->input_copy_us > ctx->bench.mpp_input_copy_max_us)
            ctx->bench.mpp_input_copy_max_us = mpp_timing->input_copy_us;
        if (mpp_timing->put_frame_us > ctx->bench.mpp_put_frame_max_us)
            ctx->bench.mpp_put_frame_max_us = mpp_timing->put_frame_us;
        if (mpp_timing->get_packet_us > ctx->bench.mpp_get_packet_max_us)
            ctx->bench.mpp_get_packet_max_us = mpp_timing->get_packet_us;
        if (mpp_timing->packet_copy_us > ctx->bench.mpp_packet_copy_max_us)
            ctx->bench.mpp_packet_copy_max_us = mpp_timing->packet_copy_us;
        if (mpp_timing->total_us > ctx->bench.mpp_total_max_us)
            ctx->bench.mpp_total_max_us = mpp_timing->total_us;
    }
    if (dqbuf_to_get_us > ctx->bench.dqbuf_to_get_max_us)
        ctx->bench.dqbuf_to_get_max_us = dqbuf_to_get_us;
    if (dqbuf_to_fanout_us > ctx->bench.dqbuf_to_fanout_max_us)
        ctx->bench.dqbuf_to_fanout_max_us = dqbuf_to_fanout_us;
}

/**
 * @description: 到达周期时打印 benchmark 摘要并重置窗口。
 */
static void bench_log_and_reset_if_due(MediaGatewayCtx *ctx)
{
    uint64_t now;
    uint64_t span_us;
    double sample_count;
    if (!ctx || !ctx->bench.enable)
        return;
    now = media_gateway_get_now_us();
    span_us = now - ctx->bench.last_ts_us;
    if (span_us < (uint64_t)ctx->bench.print_interval_sec * 1000000ULL)
        return;

    if (ctx->bench.sample_count > 0)
    {
        sample_count = (double)ctx->bench.sample_count;
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
                 ctx->bench.sample_count,
                 (double)ctx->bench.driver_to_dqbuf_sum_us / sample_count, ctx->bench.driver_to_dqbuf_max_us,
                 (double)ctx->bench.dqbuf_ioctl_sum_us / sample_count, ctx->bench.dqbuf_ioctl_max_us,
                 (double)ctx->bench.capture_call_sum_us / sample_count, ctx->bench.capture_call_max_us,
                 (double)ctx->bench.capture_copy_sum_us / sample_count, ctx->bench.capture_copy_max_us,
                 (double)ctx->bench.dqbuf_to_put_sum_us / sample_count, ctx->bench.dqbuf_to_put_max_us,
                 (double)ctx->bench.put_to_get_sum_us / sample_count, ctx->bench.put_to_get_max_us,
                 (double)ctx->bench.mpp_input_copy_sum_us / sample_count, ctx->bench.mpp_input_copy_max_us,
                 (double)ctx->bench.mpp_put_frame_sum_us / sample_count, ctx->bench.mpp_put_frame_max_us,
                 (double)ctx->bench.mpp_get_packet_sum_us / sample_count, ctx->bench.mpp_get_packet_max_us,
                 (double)ctx->bench.mpp_packet_copy_sum_us / sample_count, ctx->bench.mpp_packet_copy_max_us,
                 (double)ctx->bench.mpp_total_sum_us / sample_count, ctx->bench.mpp_total_max_us,
                 (double)ctx->bench.dqbuf_to_get_sum_us / sample_count, ctx->bench.dqbuf_to_get_max_us,
                 (double)ctx->bench.dqbuf_to_fanout_sum_us / sample_count, ctx->bench.dqbuf_to_fanout_max_us);
    }
    else
    {
        LOG_INFO("[BENCH_SUMMARY] samples=0 no sampled frames in this interval");
    }
    ctx->bench.last_ts_us = now;
    media_gateway_bench_reset_window(ctx);
}

/**
 * @description: 打印所有输出通道的运行状态。
 */
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

/**
 * @description: 到达周期时打印吞吐、输出和 benchmark 状态。
 */
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
        LOG_INFO("[STAT] stream=%d name=%s fps=%.2f bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64,
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
        LOG_INFO("[STAT] audio fps=%.2f bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64,
                 afps,
                 akbps,
                 ctx->stats.audio_frames,
                 ctx->stats.audio_bytes);
    }

    log_output_stats(ctx);
    bench_log_and_reset_if_due(ctx);
    reset_throughput_window(ctx);
    ctx->stats.last_ts_us = now;
}

/**
 * @description: 获取当前吞吐统计窗口的实时估算值。
 */
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
