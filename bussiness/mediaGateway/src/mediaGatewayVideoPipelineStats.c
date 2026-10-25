/**
 * @file mediaGatewayVideoPipelineStats.c
 * @brief 正式视频编码pipeline运行统计实现。
 */

#include "mediaGatewayVideoPipelineStats.h"

#include "logger.h"

#include <string.h>

MediaResult media_gateway_video_pipeline_stats_init(
    MediaGatewayVideoPipelineStatsTracker *tracker)
{
    if (!tracker)
    {
        LOG_ERROR("media_gateway_video_pipeline_stats_init failed: tracker is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    memset(tracker, 0, sizeof(*tracker));
    return MEDIA_OK;
}

MediaResult media_gateway_video_pipeline_stats_record_acquire(
    MediaGatewayVideoPipelineStatsTracker *tracker)
{
    if (!tracker)
    {
        LOG_ERROR("media_gateway_video_pipeline_stats_record_acquire failed: tracker is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    tracker->totals.acquired_frames++;
    return MEDIA_OK;
}

MediaResult media_gateway_video_pipeline_stats_record_busy_overwrite_window(
    MediaGatewayVideoPipelineStatsTracker *tracker,
    uint64_t overwrites_before,
    uint64_t overwrites_after)
{
    if (!tracker)
    {
        LOG_ERROR("media_gateway_video_pipeline_stats_record_busy_overwrite_window failed: tracker is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (overwrites_after < overwrites_before)
    {
        LOG_ERROR("media_gateway_video_pipeline_stats_record_busy_overwrite_window failed: counter rollback before=%llu after=%llu",
                  (unsigned long long)overwrites_before,
                  (unsigned long long)overwrites_after);
        return MEDIA_ERR_INVALID_PARAM;
    }

    tracker->totals.overwrites_while_encoder_busy +=
        overwrites_after - overwrites_before;
    return MEDIA_OK;
}

MediaResult media_gateway_video_pipeline_stats_record_encode(
    MediaGatewayVideoPipelineStatsTracker *tracker,
    uint64_t frame_period_us,
    int encode_result,
    const MediaGatewayVideoEncodeTiming *timing)
{
    if (!tracker || !timing || frame_period_us == 0U)
    {
        LOG_ERROR("media_gateway_video_pipeline_stats_record_encode failed: tracker=%p timing=%p period=%llu",
                  (void *)tracker,
                  (const void *)timing,
                  (unsigned long long)frame_period_us);
        return MEDIA_ERR_INVALID_PARAM;
    }

    if (encode_result != MEDIA_OK)
    {
        tracker->totals.encode_failures++;
        tracker->consecutive_deadline_misses = 0;
        return MEDIA_OK;
    }

    tracker->totals.encoded_frames++;
    tracker->totals.encode_total_us += timing->total_us;
    tracker->totals.dmabuf_import_total_us += timing->input_import_us;
    tracker->totals.submit_total_us += timing->submit_us;
    tracker->totals.packet_wait_total_us += timing->packet_wait_us;
    tracker->totals.packet_copy_total_us += timing->packet_copy_us;
    if (timing->total_us > tracker->totals.encode_max_us)
        tracker->totals.encode_max_us = timing->total_us;

    if (timing->total_us > frame_period_us)
    {
        tracker->totals.deadline_miss_frames++;
        tracker->consecutive_deadline_misses++;
        if (tracker->consecutive_deadline_misses >
            tracker->totals.max_consecutive_deadline_misses)
        {
            tracker->totals.max_consecutive_deadline_misses =
                tracker->consecutive_deadline_misses;
        }
    }
    else
    {
        tracker->consecutive_deadline_misses = 0;
    }
    return MEDIA_OK;
}

MediaResult media_gateway_video_pipeline_stats_snapshot(
    const MediaGatewayVideoPipelineStatsTracker *tracker,
    MediaGatewayVideoPipelineStats *snapshot)
{
    if (!tracker || !snapshot)
    {
        LOG_ERROR("media_gateway_video_pipeline_stats_snapshot failed: tracker=%p snapshot=%p",
                  (const void *)tracker,
                  (void *)snapshot);
        return MEDIA_ERR_INVALID_PARAM;
    }

    *snapshot = tracker->totals;
    return MEDIA_OK;
}
