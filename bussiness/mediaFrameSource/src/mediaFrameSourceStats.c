/**
 * @file mediaFrameSourceStats.c
 * @brief 视频帧源运行统计实现。
 */

#include "mediaFrameSourceStats.h"

#include "logger.h"

#include <limits.h>
#include <string.h>

MediaResult media_frame_source_stats_init(MediaFrameSourceStatsTracker *tracker)
{
    if (!tracker)
    {
        LOG_ERROR("media_frame_source_stats_init failed: tracker is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    memset(tracker, 0, sizeof(*tracker));
    return MEDIA_OK;
}

MediaResult media_frame_source_stats_record_capture(MediaFrameSourceStatsTracker *tracker,
                                                    uint32_t sequence)
{
    uint32_t sequence_delta = 0;

    if (!tracker)
    {
        LOG_ERROR("media_frame_source_stats_record_capture failed: tracker is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    /*
     * uint32_t减法自然支持sequence从UINT32_MAX回绕到0。小于半个序号空间的正向差值
     * 视为正常前进；更大的差值视为设备重启、流重建或乱序，不把它误报成巨量丢帧。
     */
    if (tracker->has_capture_sequence)
    {
        sequence_delta = sequence - tracker->totals.last_capture_sequence;
        if (sequence_delta > 1U && sequence_delta < (UINT32_MAX / 2U))
        {
            tracker->totals.sequence_gap_events++;
            tracker->totals.sequence_gap_frames += (uint64_t)sequence_delta - 1ULL;
        }
    }
    else
    {
        tracker->has_capture_sequence = 1;
    }

    tracker->totals.captured_frames++;
    tracker->totals.last_capture_sequence = sequence;
    return MEDIA_OK;
}

MediaResult media_frame_source_stats_record_publish(MediaFrameSourceStatsTracker *tracker)
{
    if (!tracker)
    {
        LOG_ERROR("media_frame_source_stats_record_publish failed: tracker is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    tracker->totals.published_frames++;
    return MEDIA_OK;
}

MediaResult media_frame_source_stats_record_overwrite(MediaFrameSourceStatsTracker *tracker)
{
    if (!tracker)
    {
        LOG_ERROR("media_frame_source_stats_record_overwrite failed: tracker is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    tracker->totals.overwritten_frames++;
    return MEDIA_OK;
}

MediaResult media_frame_source_stats_record_no_writable_slot_drop(
    MediaFrameSourceStatsTracker *tracker)
{
    if (!tracker)
    {
        LOG_ERROR("media_frame_source_stats_record_no_writable_slot_drop failed: tracker is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    tracker->totals.no_writable_slot_drops++;
    return MEDIA_OK;
}

MediaResult media_frame_source_stats_record_acquire(MediaFrameSourceStatsTracker *tracker)
{
    if (!tracker)
    {
        LOG_ERROR("media_frame_source_stats_record_acquire failed: tracker is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    tracker->totals.acquired_frames++;
    return MEDIA_OK;
}

MediaResult media_frame_source_stats_snapshot(const MediaFrameSourceStatsTracker *tracker,
                                              MediaFrameSourceStats *snapshot)
{
    if (!tracker || !snapshot)
    {
        LOG_ERROR("media_frame_source_stats_snapshot failed: tracker=%p snapshot=%p",
                  (const void *)tracker,
                  (void *)snapshot);
        return MEDIA_ERR_INVALID_PARAM;
    }

    *snapshot = tracker->totals;
    return MEDIA_OK;
}
