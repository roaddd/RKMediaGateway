/*
 * @Author: huangkelong
 * @Date: 2026-05-17 23:45:25
 * @LastEditTime: 2026-05-18 22:25:29
 * @LastEditors: huangkelong
 * @Description: 媒体输出路径指标相关函数实现
 * @FilePath: \Fork\RKMediaGateway\bussiness\mediaOutput\src\mediaOutputPathMetrics.c
 * 可以输入预定的版权声明、个性签名、空行等
 */
#include "mediaOutputPathMetrics.h"

#include "logger.h"

#include <inttypes.h>
#include <time.h>

static const char *media_output_metric_media_name(MediaFrameType frame_type)
{
    switch (frame_type)
    {
    case MEDIA_FRAME_TYPE_VIDEO:
        return "video";
    case MEDIA_FRAME_TYPE_AUDIO:
        return "audio";
    default:
        return "unknown";
    }
}

uint64_t media_output_metrics_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void media_output_log_path_latency(const MediaPacket *packet,
                                   uint64_t send_start_us,
                                   uint64_t send_done_us)
{
    uint64_t dqbuf_to_send_us = 0; /* 从v4l2缓冲区取出该帧到发送完成的时间 */
    uint64_t queue_us = 0; /* 该帧从输出队列中取出到发送完成的时间 */
    uint64_t output_us = 0; /* 该帧发送开始到发送完成的时间 */

    if (!packet || !packet->path_metrics.sample)
        return;

    dqbuf_to_send_us = (send_done_us >= packet->pts_us) ? (send_done_us - packet->pts_us) : 0;
    queue_us = (send_start_us >= packet->path_metrics.enqueue_ts_us)
                   ? (send_start_us - packet->path_metrics.enqueue_ts_us)
                   : 0;
    output_us = send_done_us - send_start_us;

    LOG_WARN("[PATH_LATENCY] stream=%s media=%s frame_id=%" PRIu64
             " dqbuf_to_send_us=%" PRIu64
             " encode_us=%" PRIu64
             " queue_us=%" PRIu64
             " output_us=%" PRIu64,
             packet->path_metrics.stream_name ? packet->path_metrics.stream_name : "unknown",
             media_output_metric_media_name(packet->frame_type),
             packet->frame_id,
             dqbuf_to_send_us,
             packet->path_metrics.encode_us,
             queue_us,
             output_us);
}
