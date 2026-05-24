#ifndef __MEDIA_GATEWAY_METRICS_H__
#define __MEDIA_GATEWAY_METRICS_H__

#include "mediaGateway.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void media_gateway_bench_reset_window(MediaGatewayCtx *ctx);
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
                                       uint64_t dqbuf_to_output_queued_us);
void media_gateway_bench_log_and_reset_if_due(MediaGatewayCtx *ctx);
void media_gateway_metrics_log_frame_trace(MediaGatewayCtx *ctx,
                                           int stream_idx,
                                           const MediaFrame *frame,
                                           const MediaPacket *packet,
                                           size_t h264_len);

#ifdef __cplusplus
}
#endif

#endif
