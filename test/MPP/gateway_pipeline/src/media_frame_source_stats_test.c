/**
 * @file media_frame_source_stats_test.c
 * @brief 正式视频链路帧源统计模块测试。
 */

#include "mediaFrameSourceStats.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_ASSERT(condition, message)                       \
    do                                                        \
    {                                                         \
        if (!(condition))                                     \
        {                                                     \
            fprintf(stderr, "[FAIL] %s\n", (message));       \
            return EXIT_FAILURE;                              \
        }                                                     \
    } while (0)

/**
 * @brief 验证连续序号不会产生V4L2 sequence gap。
 */
static int test_contiguous_capture_sequence_has_no_gap(void)
{
    MediaFrameSourceStatsTracker tracker = {0};
    MediaFrameSourceStats snapshot = {0};

    media_frame_source_stats_init(&tracker);
    TEST_ASSERT(media_frame_source_stats_record_capture(&tracker, 100U) == MEDIA_OK,
                "record sequence 100");
    TEST_ASSERT(media_frame_source_stats_record_capture(&tracker, 101U) == MEDIA_OK,
                "record sequence 101");
    TEST_ASSERT(media_frame_source_stats_snapshot(&tracker, &snapshot) == MEDIA_OK,
                "read contiguous snapshot");
    TEST_ASSERT(snapshot.captured_frames == 2U, "captured frame count should be 2");
    TEST_ASSERT(snapshot.sequence_gap_events == 0U, "contiguous sequence must not create gap event");
    TEST_ASSERT(snapshot.sequence_gap_frames == 0U, "contiguous sequence must not lose frames");
    TEST_ASSERT(snapshot.last_capture_sequence == 101U, "last sequence should be 101");
    return EXIT_SUCCESS;
}

/**
 * @brief 验证100、101、103只产生一次事件并缺失一帧。
 */
static int test_capture_sequence_gap_counts_event_and_missing_frames(void)
{
    MediaFrameSourceStatsTracker tracker = {0};
    MediaFrameSourceStats snapshot = {0};

    media_frame_source_stats_init(&tracker);
    TEST_ASSERT(media_frame_source_stats_record_capture(&tracker, 100U) == MEDIA_OK,
                "record sequence 100");
    TEST_ASSERT(media_frame_source_stats_record_capture(&tracker, 101U) == MEDIA_OK,
                "record sequence 101");
    TEST_ASSERT(media_frame_source_stats_record_capture(&tracker, 103U) == MEDIA_OK,
                "record sequence 103");
    TEST_ASSERT(media_frame_source_stats_snapshot(&tracker, &snapshot) == MEDIA_OK,
                "read gap snapshot");
    TEST_ASSERT(snapshot.captured_frames == 3U, "captured frame count should be 3");
    TEST_ASSERT(snapshot.sequence_gap_events == 1U, "one discontinuity should create one gap event");
    TEST_ASSERT(snapshot.sequence_gap_frames == 1U, "sequence 102 should be counted as one missing frame");
    TEST_ASSERT(snapshot.last_capture_sequence == 103U, "last sequence should be 103");
    return EXIT_SUCCESS;
}

/**
 * @brief 验证帧源发布、覆盖、无槽位丢帧和消费事件分别累计。
 */
static int test_frame_source_events_are_reported_separately(void)
{
    MediaFrameSourceStatsTracker tracker = {0};
    MediaFrameSourceStats snapshot = {0};

    media_frame_source_stats_init(&tracker);
    TEST_ASSERT(media_frame_source_stats_record_publish(&tracker) == MEDIA_OK,
                "record first publish");
    TEST_ASSERT(media_frame_source_stats_record_publish(&tracker) == MEDIA_OK,
                "record second publish");
    TEST_ASSERT(media_frame_source_stats_record_overwrite(&tracker) == MEDIA_OK,
                "record latest-frame overwrite");
    TEST_ASSERT(media_frame_source_stats_record_no_writable_slot_drop(&tracker) == MEDIA_OK,
                "record no writable slot drop");
    TEST_ASSERT(media_frame_source_stats_record_acquire(&tracker) == MEDIA_OK,
                "record frame acquire");
    TEST_ASSERT(media_frame_source_stats_snapshot(&tracker, &snapshot) == MEDIA_OK,
                "read event snapshot");
    TEST_ASSERT(snapshot.published_frames == 2U, "published frame count should be 2");
    TEST_ASSERT(snapshot.overwritten_frames == 1U, "overwritten frame count should be 1");
    TEST_ASSERT(snapshot.no_writable_slot_drops == 1U, "no-slot drop count should be 1");
    TEST_ASSERT(snapshot.acquired_frames == 1U, "acquired frame count should be 1");
    return EXIT_SUCCESS;
}

int main(void)
{
    if (test_contiguous_capture_sequence_has_no_gap() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_capture_sequence_gap_counts_event_and_missing_frames() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_frame_source_events_are_reported_separately() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    printf("media frame source stats tests passed\n");
    return EXIT_SUCCESS;
}
