#ifndef __MEDIA_GATEWAY_STATS_H__
#define __MEDIA_GATEWAY_STATS_H__

#include "mediaGateway.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void media_gateway_bench_reset_window(MediaGatewayCtx *ctx);
void media_gateway_bench_record_sample(MediaGatewayCtx *ctx,
                                       uint64_t driver_to_dqbuf_us,
                                       uint64_t dqbuf_ioctl_us,
                                       uint64_t capture_call_us,
                                       uint64_t capture_copy_us,
                                       uint64_t dqbuf_to_put_us,
                                       uint64_t put_to_get_us,
                                       const MppEncoderTiming *mpp_timing,
                                       uint64_t dqbuf_to_get_us,
                                       uint64_t dqbuf_to_fanout_us);
void media_gateway_log_throughput_if_due(MediaGatewayCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
