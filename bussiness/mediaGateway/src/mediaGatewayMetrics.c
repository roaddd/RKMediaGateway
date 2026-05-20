#include "mediaGatewayMetrics.h"

#include "mediaGatewayClock.h"

#include "logger.h"

#include <inttypes.h>

void media_gateway_bench_reset_window(MediaGatewayCtx *ctx)
{
    if (!ctx)
        return;
    ctx->bench.sample_count = 0;
    ctx->bench.camera_buffer_wait_sum_us = 0;
    ctx->bench.camera_buffer_wait_max_us = 0;
    ctx->bench.dqbuf_ioctl_duration_sum_us = 0;
    ctx->bench.dqbuf_ioctl_duration_max_us = 0;
    ctx->bench.capture_call_duration_sum_us = 0;
    ctx->bench.capture_call_duration_max_us = 0;
    ctx->bench.mmap_to_frame_cache_copy_sum_us = 0;
    ctx->bench.mmap_to_frame_cache_copy_max_us = 0;
    ctx->bench.frame_source_publish_sum_us = 0;
    ctx->bench.frame_source_publish_max_us = 0;
    ctx->bench.frame_source_publish_copy_sum_us = 0;
    ctx->bench.frame_source_publish_copy_max_us = 0;
    ctx->bench.video_input_publish_copy_sum_us = 0;
    ctx->bench.video_input_publish_copy_max_us = 0;
    ctx->bench.video_input_acquire_copy_sum_us = 0;
    ctx->bench.video_input_acquire_copy_max_us = 0;
    ctx->bench.dqbuf_to_encode_start_sum_us = 0;
    ctx->bench.dqbuf_to_encode_start_max_us = 0;
    ctx->bench.encode_start_to_done_sum_us = 0;
    ctx->bench.encode_start_to_done_max_us = 0;
    ctx->bench.encoder_input_buffer_copy_sum_us = 0;
    ctx->bench.encoder_input_buffer_copy_max_us = 0;
    ctx->bench.encoder_submit_frame_call_sum_us = 0;
    ctx->bench.encoder_submit_frame_call_max_us = 0;
    ctx->bench.encoder_poll_packet_call_sum_us = 0;
    ctx->bench.encoder_poll_packet_call_max_us = 0;
    ctx->bench.encoder_packet_copy_sum_us = 0;
    ctx->bench.encoder_packet_copy_max_us = 0;
    ctx->bench.encode_frame_total_sum_us = 0;
    ctx->bench.encode_frame_total_max_us = 0;
    ctx->bench.dqbuf_to_encode_done_sum_us = 0;
    ctx->bench.dqbuf_to_encode_done_max_us = 0;
    ctx->bench.dqbuf_to_output_queued_sum_us = 0;
    ctx->bench.dqbuf_to_output_queued_max_us = 0;
}

void media_gateway_bench_record_sample(MediaGatewayCtx *ctx,
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
    if (!ctx)
        return;

    ctx->bench.sample_count++;
    ctx->bench.camera_buffer_wait_sum_us += camera_buffer_wait_us;
    ctx->bench.dqbuf_ioctl_duration_sum_us += dqbuf_ioctl_duration_us;
    ctx->bench.capture_call_duration_sum_us += capture_call_duration_us;
    ctx->bench.mmap_to_frame_cache_copy_sum_us += mmap_to_frame_cache_copy_us;
    ctx->bench.frame_source_publish_sum_us += frame_source_publish_us;
    ctx->bench.frame_source_publish_copy_sum_us += frame_source_publish_copy_us;
    ctx->bench.video_input_publish_copy_sum_us += video_input_publish_copy_us;
    ctx->bench.video_input_acquire_copy_sum_us += video_input_acquire_copy_us;
    ctx->bench.dqbuf_to_encode_start_sum_us += dqbuf_to_encode_start_us;
    ctx->bench.encode_start_to_done_sum_us += encode_start_to_done_us;
    if (encoder_timing)
    {
        ctx->bench.encoder_input_buffer_copy_sum_us += encoder_timing->encoder_input_buffer_copy_us;
        ctx->bench.encoder_submit_frame_call_sum_us += encoder_timing->encoder_submit_frame_call_us;
        ctx->bench.encoder_poll_packet_call_sum_us += encoder_timing->encoder_poll_packet_call_us;
        ctx->bench.encoder_packet_copy_sum_us += encoder_timing->encoder_packet_copy_us;
        ctx->bench.encode_frame_total_sum_us += encoder_timing->encode_frame_total_us;
    }
    ctx->bench.dqbuf_to_encode_done_sum_us += dqbuf_to_encode_done_us;
    ctx->bench.dqbuf_to_output_queued_sum_us += dqbuf_to_output_queued_us;

    if (camera_buffer_wait_us > ctx->bench.camera_buffer_wait_max_us)
        ctx->bench.camera_buffer_wait_max_us = camera_buffer_wait_us;
    if (dqbuf_ioctl_duration_us > ctx->bench.dqbuf_ioctl_duration_max_us)
        ctx->bench.dqbuf_ioctl_duration_max_us = dqbuf_ioctl_duration_us;
    if (capture_call_duration_us > ctx->bench.capture_call_duration_max_us)
        ctx->bench.capture_call_duration_max_us = capture_call_duration_us;
    if (mmap_to_frame_cache_copy_us > ctx->bench.mmap_to_frame_cache_copy_max_us)
        ctx->bench.mmap_to_frame_cache_copy_max_us = mmap_to_frame_cache_copy_us;
    if (frame_source_publish_us > ctx->bench.frame_source_publish_max_us)
        ctx->bench.frame_source_publish_max_us = frame_source_publish_us;
    if (frame_source_publish_copy_us > ctx->bench.frame_source_publish_copy_max_us)
        ctx->bench.frame_source_publish_copy_max_us = frame_source_publish_copy_us;
    if (video_input_publish_copy_us > ctx->bench.video_input_publish_copy_max_us)
        ctx->bench.video_input_publish_copy_max_us = video_input_publish_copy_us;
    if (video_input_acquire_copy_us > ctx->bench.video_input_acquire_copy_max_us)
        ctx->bench.video_input_acquire_copy_max_us = video_input_acquire_copy_us;
    if (dqbuf_to_encode_start_us > ctx->bench.dqbuf_to_encode_start_max_us)
        ctx->bench.dqbuf_to_encode_start_max_us = dqbuf_to_encode_start_us;
    if (encode_start_to_done_us > ctx->bench.encode_start_to_done_max_us)
        ctx->bench.encode_start_to_done_max_us = encode_start_to_done_us;
    if (encoder_timing)
    {
        if (encoder_timing->encoder_input_buffer_copy_us > ctx->bench.encoder_input_buffer_copy_max_us)
            ctx->bench.encoder_input_buffer_copy_max_us = encoder_timing->encoder_input_buffer_copy_us;
        if (encoder_timing->encoder_submit_frame_call_us > ctx->bench.encoder_submit_frame_call_max_us)
            ctx->bench.encoder_submit_frame_call_max_us = encoder_timing->encoder_submit_frame_call_us;
        if (encoder_timing->encoder_poll_packet_call_us > ctx->bench.encoder_poll_packet_call_max_us)
            ctx->bench.encoder_poll_packet_call_max_us = encoder_timing->encoder_poll_packet_call_us;
        if (encoder_timing->encoder_packet_copy_us > ctx->bench.encoder_packet_copy_max_us)
            ctx->bench.encoder_packet_copy_max_us = encoder_timing->encoder_packet_copy_us;
        if (encoder_timing->encode_frame_total_us > ctx->bench.encode_frame_total_max_us)
            ctx->bench.encode_frame_total_max_us = encoder_timing->encode_frame_total_us;
    }
    if (dqbuf_to_encode_done_us > ctx->bench.dqbuf_to_encode_done_max_us)
        ctx->bench.dqbuf_to_encode_done_max_us = dqbuf_to_encode_done_us;
    if (dqbuf_to_output_queued_us > ctx->bench.dqbuf_to_output_queued_max_us)
        ctx->bench.dqbuf_to_output_queued_max_us = dqbuf_to_output_queued_us;
}

void media_gateway_bench_log_and_reset_if_due(MediaGatewayCtx *ctx)
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
        LOG_WARN("[BENCH_SUMMARY] samples=%" PRIu64
                 " avg_camera_buffer_wait_us=%.2f max_camera_buffer_wait_us=%" PRIu64
                 " avg_dqbuf_ioctl_duration_us=%.2f max_dqbuf_ioctl_duration_us=%" PRIu64
                 " avg_capture_call_duration_us=%.2f max_capture_call_duration_us=%" PRIu64
                 " avg_mmap_to_frame_cache_copy_us=%.2f max_mmap_to_frame_cache_copy_us=%" PRIu64
                 " avg_frame_source_publish_us=%.2f max_frame_source_publish_us=%" PRIu64
                 " avg_frame_source_publish_copy_us=%.2f max_frame_source_publish_copy_us=%" PRIu64
                 " avg_video_input_publish_copy_us=%.2f max_video_input_publish_copy_us=%" PRIu64
                 " avg_video_input_acquire_copy_us=%.2f max_video_input_acquire_copy_us=%" PRIu64
                 " avg_dqbuf_to_encode_start_us=%.2f max_dqbuf_to_encode_start_us=%" PRIu64
                 " avg_encode_start_to_done_us=%.2f max_encode_start_to_done_us=%" PRIu64
                 " avg_encoder_input_buffer_copy_us=%.2f max_encoder_input_buffer_copy_us=%" PRIu64
                 " avg_encoder_submit_frame_call_us=%.2f max_encoder_submit_frame_call_us=%" PRIu64
                 " avg_encoder_poll_packet_call_us=%.2f max_encoder_poll_packet_call_us=%" PRIu64
                 " avg_encoder_packet_copy_us=%.2f max_encoder_packet_copy_us=%" PRIu64
                 " avg_encode_frame_total_us=%.2f max_encode_frame_total_us=%" PRIu64
                 " avg_dqbuf_to_encode_done_us=%.2f max_dqbuf_to_encode_done_us=%" PRIu64
                 " avg_dqbuf_to_output_queued_us=%.2f max_dqbuf_to_output_queued_us=%" PRIu64,
                 ctx->bench.sample_count,
                 (double)ctx->bench.camera_buffer_wait_sum_us / sample_count, ctx->bench.camera_buffer_wait_max_us,
                 (double)ctx->bench.dqbuf_ioctl_duration_sum_us / sample_count, ctx->bench.dqbuf_ioctl_duration_max_us,
                 (double)ctx->bench.capture_call_duration_sum_us / sample_count, ctx->bench.capture_call_duration_max_us,
                 (double)ctx->bench.mmap_to_frame_cache_copy_sum_us / sample_count, ctx->bench.mmap_to_frame_cache_copy_max_us,
                 (double)ctx->bench.frame_source_publish_sum_us / sample_count, ctx->bench.frame_source_publish_max_us,
                 (double)ctx->bench.frame_source_publish_copy_sum_us / sample_count, ctx->bench.frame_source_publish_copy_max_us,
                 (double)ctx->bench.video_input_publish_copy_sum_us / sample_count, ctx->bench.video_input_publish_copy_max_us,
                 (double)ctx->bench.video_input_acquire_copy_sum_us / sample_count, ctx->bench.video_input_acquire_copy_max_us,
                 (double)ctx->bench.dqbuf_to_encode_start_sum_us / sample_count, ctx->bench.dqbuf_to_encode_start_max_us,
                 (double)ctx->bench.encode_start_to_done_sum_us / sample_count, ctx->bench.encode_start_to_done_max_us,
                 (double)ctx->bench.encoder_input_buffer_copy_sum_us / sample_count, ctx->bench.encoder_input_buffer_copy_max_us,
                 (double)ctx->bench.encoder_submit_frame_call_sum_us / sample_count, ctx->bench.encoder_submit_frame_call_max_us,
                 (double)ctx->bench.encoder_poll_packet_call_sum_us / sample_count, ctx->bench.encoder_poll_packet_call_max_us,
                 (double)ctx->bench.encoder_packet_copy_sum_us / sample_count, ctx->bench.encoder_packet_copy_max_us,
                 (double)ctx->bench.encode_frame_total_sum_us / sample_count, ctx->bench.encode_frame_total_max_us,
                 (double)ctx->bench.dqbuf_to_encode_done_sum_us / sample_count, ctx->bench.dqbuf_to_encode_done_max_us,
                 (double)ctx->bench.dqbuf_to_output_queued_sum_us / sample_count, ctx->bench.dqbuf_to_output_queued_max_us);
    }
    else
    {
        LOG_INFO("[BENCH_SUMMARY] samples=0 no sampled frames in this interval");
    }
    ctx->bench.last_ts_us = now;
    media_gateway_bench_reset_window(ctx);
}
