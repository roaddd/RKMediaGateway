#ifndef __MEDIA_OUTPUT_PATH_METRICS_H__
#define __MEDIA_OUTPUT_PATH_METRICS_H__

#include "mediaOutput.h"
#include "mediaPacket.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t media_output_metrics_now_us(void);

typedef struct {
    const char *output_name;         /* 输出通道名称，用于区分 RTSP/RTMP/GB28181 等具体通道。 */
    MediaOutputType output_type;     /* 输出协议类型，用于日志辅助定位。 */
    const MediaPacket *packet;       /* 已完成发送的媒体包。 */
    uint64_t send_start_us;          /* 输出通道开始发送该包的时间戳。 */
    uint64_t send_done_us;           /* 输出通道完成发送该包的时间戳。 */
} MediaOutputPathLatencySample;

void media_output_log_path_latency(const MediaOutputPathLatencySample *sample);

#ifdef __cplusplus
}
#endif

#endif
