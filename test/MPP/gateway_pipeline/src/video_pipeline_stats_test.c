/**
 * @file video_pipeline_stats_test.c
 * @brief 正式视频编码pipeline统计模块测试。
 */

#include "mediaGatewayVideoPipelineStats.h"

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
 * @brief 验证deadline miss按帧周期判断并统计最大连续次数。
 */
static int test_deadline_miss_uses_frame_period(void)
{
    MediaGatewayVideoPipelineStatsTracker tracker = {0};
    MediaGatewayVideoPipelineStats snapshot = {0};
    MediaGatewayVideoEncodeTiming timing = {0};

    media_gateway_video_pipeline_stats_init(&tracker);

    timing.total_us = 14000U;
    timing.packet_wait_us = 12000U;
    TEST_ASSERT(media_gateway_video_pipeline_stats_record_encode(
                    &tracker, 16667U, MEDIA_OK, &timing) == MEDIA_OK,
                "record on-time frame");

    timing.total_us = 25000U;
    timing.packet_wait_us = 23000U;
    TEST_ASSERT(media_gateway_video_pipeline_stats_record_encode(
                    &tracker, 16667U, MEDIA_OK, &timing) == MEDIA_OK,
                "record first missed frame");

    timing.total_us = 18000U;
    timing.packet_wait_us = 16000U;
    TEST_ASSERT(media_gateway_video_pipeline_stats_record_encode(
                    &tracker, 16667U, MEDIA_OK, &timing) == MEDIA_OK,
                "record second consecutive missed frame");

    timing.total_us = 10000U;
    timing.packet_wait_us = 8000U;
    TEST_ASSERT(media_gateway_video_pipeline_stats_record_encode(
                    &tracker, 16667U, MEDIA_OK, &timing) == MEDIA_OK,
                "record recovery frame");

    TEST_ASSERT(media_gateway_video_pipeline_stats_snapshot(&tracker, &snapshot) == MEDIA_OK,
                "read pipeline snapshot");
    TEST_ASSERT(snapshot.encoded_frames == 4U, "encoded frame count should be 4");
    TEST_ASSERT(snapshot.encode_failures == 0U, "encode failure count should be 0");
    TEST_ASSERT(snapshot.deadline_miss_frames == 2U, "deadline miss count should be 2");
    TEST_ASSERT(snapshot.max_consecutive_deadline_misses == 2U,
                "maximum consecutive deadline misses should be 2");
    TEST_ASSERT(snapshot.encode_total_us == 67000U, "total encode time should be 67000 us");
    TEST_ASSERT(snapshot.encode_max_us == 25000U, "maximum encode time should be 25000 us");
    TEST_ASSERT(snapshot.packet_wait_total_us == 59000U,
                "total packet wait should be 59000 us");
    return EXIT_SUCCESS;
}

/**
 * @brief 验证同步编码调用期间的帧源覆盖使用前后快照差值累计。
 */
static int test_overwrites_during_encode_are_accumulated(void)
{
    MediaGatewayVideoPipelineStatsTracker tracker = {0};
    MediaGatewayVideoPipelineStats snapshot = {0};

    media_gateway_video_pipeline_stats_init(&tracker);
    TEST_ASSERT(media_gateway_video_pipeline_stats_record_acquire(&tracker) == MEDIA_OK,
                "record acquired frame");
    TEST_ASSERT(media_gateway_video_pipeline_stats_record_frame_rate_gate_drop(&tracker) == MEDIA_OK,
                "record frame rate gate drop");
    TEST_ASSERT(media_gateway_video_pipeline_stats_record_busy_overwrite_window(
                    &tracker, 10U, 13U) == MEDIA_OK,
                "record busy overwrite delta");
    TEST_ASSERT(media_gateway_video_pipeline_stats_record_busy_overwrite_window(
                    &tracker, 13U, 12U) == MEDIA_ERR_INVALID_PARAM,
                "reject rolled-back overwrite counter");
    TEST_ASSERT(media_gateway_video_pipeline_stats_snapshot(&tracker, &snapshot) == MEDIA_OK,
                "read overwrite snapshot");
    TEST_ASSERT(snapshot.acquired_frames == 1U, "acquired frame count should be 1");
    TEST_ASSERT(snapshot.frame_rate_gate_drops == 1U,
                "frame rate gate drop count should be 1");
    TEST_ASSERT(snapshot.overwrites_while_encoder_busy == 3U,
                "busy overwrite count should be 3");
    return EXIT_SUCCESS;
}

int main(void)
{
    if (test_deadline_miss_uses_frame_period() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_overwrites_during_encode_are_accumulated() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    printf("video pipeline stats tests passed\n");
    return EXIT_SUCCESS;
}
