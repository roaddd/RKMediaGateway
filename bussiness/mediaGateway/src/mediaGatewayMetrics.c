#include "mediaGatewayMetrics.h"

#include "mediaGatewayClock.h"

#include "logger.h"

#include <inttypes.h>
#include <string.h>

#define FRAME_TRACE_SAMPLE_INTERVAL 30ULL

static double bench_metric_avg(const MediaGatewayBenchmarkMetric *metric, double sample_count)
{
    return (double)metric->sum_us / sample_count;
}

static void bench_metric_record(MediaGatewayBenchmarkMetric *metric, uint64_t value_us)
{
    metric->sum_us += value_us;
    if (value_us > metric->max_us)
        metric->max_us = value_us;
}

void media_gateway_metrics_log_frame_trace(MediaGatewayCtx *ctx,
                                           int stream_idx,
                                           const MediaFrame *frame,
                                           const MediaPacket *packet,
                                           size_t h264_len)
{
    uint64_t dqbuf_to_output_queued_us;
    uint64_t camera_to_output_queued_us;

    if (!ctx || !frame || !packet)
        return;
    if (stream_idx < 0 || stream_idx >= ctx->config.stream_count ||
        (frame->frame_id % FRAME_TRACE_SAMPLE_INTERVAL) != 0)
    {
        return;
    }

    dqbuf_to_output_queued_us =
        (packet->path_metrics.enqueue_ts_us >= frame->dqbuf_ts_us)
            ? (packet->path_metrics.enqueue_ts_us - frame->dqbuf_ts_us)
            : 0;
    camera_to_output_queued_us = frame->metrics.camera_buffer_wait_us + dqbuf_to_output_queued_us;

    LOG_WARN("[FRAME_TRACE] stream=%s frame_id=%" PRIu64
             " camera_to_output_queued_us=%" PRIu64
             " camera_buffer_wait_us=%" PRIu64
             " dqbuf_to_output_queued_us=%" PRIu64
             " dqbuf_to_encode_start_us=%" PRIu64
             " encode_us=%" PRIu64
             " key=%d h264_len=%zu",
             ctx->config.streams[stream_idx].name ? ctx->config.streams[stream_idx].name : "unknown",
             frame->frame_id,
             camera_to_output_queued_us,
             frame->metrics.camera_buffer_wait_us,
             dqbuf_to_output_queued_us,
             packet->path_metrics.dqbuf_to_encode_start_us,
             packet->path_metrics.encode_us,
             packet->is_key_frame,
             h264_len);
}

void media_gateway_bench_reset_window(MediaGatewayCtx *ctx)
{
    if (!ctx)
        return;
    memset(ctx->bench.streams, 0, sizeof(ctx->bench.streams));
}

void media_gateway_bench_record_sample(MediaGatewayCtx *ctx,
                                       int stream_idx,
                                       uint64_t camera_buffer_wait_us,
                                       uint64_t dqbuf_ioctl_duration_us,
                                       uint64_t capture_call_duration_us,
                                       uint64_t mmap_to_frame_cache_copy_us,
                                       uint64_t frame_source_publish_us,
                                       uint64_t frame_source_publish_copy_us,
                                       uint64_t video_input_publish_copy_us,
                                       uint64_t video_input_acquire_copy_us,
                                       uint64_t dqbuf_to_encode_start_us,
                                       uint64_t encode_start_to_done_us,
                                       const MppEncoderTiming *encoder_timing,
                                       uint64_t dqbuf_to_encode_done_us,
                                       uint64_t dqbuf_to_output_queued_us)
{
    MediaGatewayBenchmarkWindow *window;

    if (!ctx || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
        return;

    window = &ctx->bench.streams[stream_idx];
    window->sample_count++;
    bench_metric_record(&window->camera_buffer_wait, camera_buffer_wait_us);
    bench_metric_record(&window->dqbuf_ioctl_duration, dqbuf_ioctl_duration_us);
    bench_metric_record(&window->capture_call_duration, capture_call_duration_us);
    bench_metric_record(&window->mmap_to_frame_cache_copy, mmap_to_frame_cache_copy_us);
    bench_metric_record(&window->frame_source_publish, frame_source_publish_us);
    bench_metric_record(&window->frame_source_publish_copy, frame_source_publish_copy_us);
    bench_metric_record(&window->video_input_publish_copy, video_input_publish_copy_us);
    bench_metric_record(&window->video_input_acquire_copy, video_input_acquire_copy_us);
    bench_metric_record(&window->dqbuf_to_encode_start, dqbuf_to_encode_start_us);
    bench_metric_record(&window->encode_start_to_done, encode_start_to_done_us);
    if (encoder_timing)
    {
        bench_metric_record(&window->encoder_input_buffer_copy,
                            encoder_timing->encoder_input_buffer_copy_us);
        bench_metric_record(&window->encoder_submit_frame_call,
                            encoder_timing->encoder_submit_frame_call_us);
        bench_metric_record(&window->encoder_poll_packet_call,
                            encoder_timing->encoder_poll_packet_call_us);
        bench_metric_record(&window->encoder_packet_copy,
                            encoder_timing->encoder_packet_copy_us);
        bench_metric_record(&window->encode_frame_total,
                            encoder_timing->encode_frame_total_us);
    }
    bench_metric_record(&window->dqbuf_to_encode_done, dqbuf_to_encode_done_us);
    bench_metric_record(&window->dqbuf_to_output_queued, dqbuf_to_output_queued_us);
}

static void media_gateway_bench_log_stream(MediaGatewayCtx *ctx, int stream_idx)
{
    MediaGatewayBenchmarkWindow *window = &ctx->bench.streams[stream_idx];
    double sample_count;
    const char *stream_name;

    stream_name = (stream_idx < ctx->config.stream_count && ctx->config.streams[stream_idx].name)
                      ? ctx->config.streams[stream_idx].name
                      : "unknown";
    if (window->sample_count <= 0)
    {
        LOG_INFO("[BENCH_SUMMARY] stream=%d name=%s samples=0 no sampled frames in this interval",
                 stream_idx,
                 stream_name);
        return;
    }

    sample_count = (double)window->sample_count;
    LOG_WARN("[BENCH_SUMMARY] stream=%d name=%s samples=%" PRIu64
             " interval_sec=%d sample_every=%d",
             stream_idx,
             stream_name,
             window->sample_count,
             ctx->bench.print_interval_sec,
             ctx->bench.sample_every);
    LOG_WARN("[BENCH_SUMMARY_CAPTURE] stream=%d camera_wait_avg_us=%.2f camera_wait_max_us=%" PRIu64
             " dqbuf_ioctl_avg_us=%.2f dqbuf_ioctl_max_us=%" PRIu64
             " capture_call_avg_us=%.2f capture_call_max_us=%" PRIu64,
             stream_idx,
             bench_metric_avg(&window->camera_buffer_wait, sample_count),
             window->camera_buffer_wait.max_us,
             bench_metric_avg(&window->dqbuf_ioctl_duration, sample_count),
             window->dqbuf_ioctl_duration.max_us,
             bench_metric_avg(&window->capture_call_duration, sample_count),
             window->capture_call_duration.max_us);
    LOG_WARN("[BENCH_SUMMARY_COPY] stream=%d mmap_to_cache_avg_us=%.2f mmap_to_cache_max_us=%" PRIu64
             " frame_publish_avg_us=%.2f frame_publish_max_us=%" PRIu64
             " frame_publish_copy_avg_us=%.2f frame_publish_copy_max_us=%" PRIu64
             " video_input_publish_copy_avg_us=%.2f video_input_publish_copy_max_us=%" PRIu64
             " video_input_acquire_copy_avg_us=%.2f video_input_acquire_copy_max_us=%" PRIu64,
             stream_idx,
             bench_metric_avg(&window->mmap_to_frame_cache_copy, sample_count),
             window->mmap_to_frame_cache_copy.max_us,
             bench_metric_avg(&window->frame_source_publish, sample_count),
             window->frame_source_publish.max_us,
             bench_metric_avg(&window->frame_source_publish_copy, sample_count),
             window->frame_source_publish_copy.max_us,
             bench_metric_avg(&window->video_input_publish_copy, sample_count),
             window->video_input_publish_copy.max_us,
             bench_metric_avg(&window->video_input_acquire_copy, sample_count),
             window->video_input_acquire_copy.max_us);
    LOG_WARN("[BENCH_SUMMARY_ENCODE] stream=%d dqbuf_to_encode_start_avg_us=%.2f dqbuf_to_encode_start_max_us=%" PRIu64
             " encode_start_to_done_avg_us=%.2f encode_start_to_done_max_us=%" PRIu64
             " encoder_input_copy_avg_us=%.2f encoder_input_copy_max_us=%" PRIu64
             " encoder_submit_avg_us=%.2f encoder_submit_max_us=%" PRIu64
             " encoder_poll_avg_us=%.2f encoder_poll_max_us=%" PRIu64
             " encoder_packet_copy_avg_us=%.2f encoder_packet_copy_max_us=%" PRIu64
             " encode_total_avg_us=%.2f encode_total_max_us=%" PRIu64,
             stream_idx,
             bench_metric_avg(&window->dqbuf_to_encode_start, sample_count),
             window->dqbuf_to_encode_start.max_us,
             bench_metric_avg(&window->encode_start_to_done, sample_count),
             window->encode_start_to_done.max_us,
             bench_metric_avg(&window->encoder_input_buffer_copy, sample_count),
             window->encoder_input_buffer_copy.max_us,
             bench_metric_avg(&window->encoder_submit_frame_call, sample_count),
             window->encoder_submit_frame_call.max_us,
             bench_metric_avg(&window->encoder_poll_packet_call, sample_count),
             window->encoder_poll_packet_call.max_us,
             bench_metric_avg(&window->encoder_packet_copy, sample_count),
             window->encoder_packet_copy.max_us,
             bench_metric_avg(&window->encode_frame_total, sample_count),
             window->encode_frame_total.max_us);
    LOG_WARN("[BENCH_SUMMARY_E2E] stream=%d dqbuf_to_encode_done_avg_us=%.2f dqbuf_to_encode_done_max_us=%" PRIu64
             " dqbuf_to_output_queued_avg_us=%.2f dqbuf_to_output_queued_max_us=%" PRIu64,
             stream_idx,
             bench_metric_avg(&window->dqbuf_to_encode_done, sample_count),
             window->dqbuf_to_encode_done.max_us,
             bench_metric_avg(&window->dqbuf_to_output_queued, sample_count),
             window->dqbuf_to_output_queued.max_us);
}

void media_gateway_bench_log_and_reset_if_due(MediaGatewayCtx *ctx)
{
    uint64_t now;
    uint64_t span_us;
    int i;

    if (!ctx || !ctx->bench.enable)
        return;
    now = media_gateway_get_now_us();
    span_us = now - ctx->bench.last_ts_us;
    if (span_us < (uint64_t)ctx->bench.print_interval_sec * 1000000ULL)
        return;

    for (i = 0; i < ctx->config.stream_count; ++i)
    {
        if (!ctx->stream_enabled[i])
            continue;
        media_gateway_bench_log_stream(ctx, i);
    }
    ctx->bench.last_ts_us = now;
    media_gateway_bench_reset_window(ctx);
}
