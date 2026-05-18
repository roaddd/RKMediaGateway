#ifndef __MEDIA_OUTPUT_PATH_METRICS_H__
#define __MEDIA_OUTPUT_PATH_METRICS_H__

#include "mediaPacket.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t media_output_metrics_now_us(void);
void media_output_log_path_latency(const MediaPacket *packet,
                                   uint64_t send_start_us,
                                   uint64_t send_done_us);

#ifdef __cplusplus
}
#endif

#endif
