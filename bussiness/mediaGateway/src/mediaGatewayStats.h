#ifndef __MEDIA_GATEWAY_STATS_H__
#define __MEDIA_GATEWAY_STATS_H__

#include "mediaGateway.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void media_gateway_log_throughput_if_due(MediaGatewayCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
