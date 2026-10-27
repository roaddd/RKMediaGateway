/**
 * @file video_frame_rate_plan_test.c
 * @brief 共享Sensor帧率规划与逐流帧率门控行为测试。
 */

#include "videoFrameRate.h"

#include <stdio.h>
#include <stdlib.h>

#define TEST_ASSERT(condition, message)                  \
    do                                                   \
    {                                                    \
        if (!(condition))                                \
        {                                                \
            fprintf(stderr, "[FAIL] %s\n", (message)); \
            return EXIT_FAILURE;                         \
        }                                                \
    } while (0)

/** @brief 验证共享Sensor采用所有启用码流中的最高目标帧率。 */
static int test_sensor_uses_highest_enabled_stream_fps(void)
{
    VideoFrameRateRequirement requirements[2] = {{0}};
    int sensor_fps = 0;

    requirements[0].enabled = 1;
    requirements[0].target_fps = 60;
    requirements[1].enabled = 1;
    requirements[1].target_fps = 30;

    TEST_ASSERT(video_frame_rate_select_source_fps(requirements, 2U, &sensor_fps) == MEDIA_OK,
                "select sensor fps for 60+30 streams");
    TEST_ASSERT(sensor_fps == 60, "sensor fps should be 60");
    return EXIT_SUCCESS;
}

/** @brief 验证60 FPS输入按确定性节奏门控为30 FPS，且首帧立即通过。 */
static int test_gate_reduces_60_fps_input_to_30_fps(void)
{
    VideoFrameRateGate gate = {0};
    const int expected[6] = {1, 0, 1, 0, 1, 0};
    int index = 0;
    int accepted = 0;

    TEST_ASSERT(video_frame_rate_gate_init(&gate, 60, 30) == MEDIA_OK,
                "initialize 60 to 30 gate");
    for (index = 0; index < 6; ++index)
    {
        TEST_ASSERT(video_frame_rate_gate_accept(&gate, &accepted) == MEDIA_OK,
                    "evaluate 60 to 30 gate");
        TEST_ASSERT(accepted == expected[index], "60 to 30 gate cadence mismatch");
    }
    return EXIT_SUCCESS;
}

/** @brief 验证目标帧率等于输入帧率时每一帧都通过。 */
static int test_gate_keeps_every_frame_when_rates_match(void)
{
    VideoFrameRateGate gate = {0};
    int index = 0;
    int accepted = 0;

    TEST_ASSERT(video_frame_rate_gate_init(&gate, 60, 60) == MEDIA_OK,
                "initialize 60 to 60 gate");
    for (index = 0; index < 6; ++index)
    {
        TEST_ASSERT(video_frame_rate_gate_accept(&gate, &accepted) == MEDIA_OK,
                    "evaluate 60 to 60 gate");
        TEST_ASSERT(accepted == 1, "matching rates must accept every frame");
    }
    return EXIT_SUCCESS;
}

/** @brief 验证门控不能凭空将低帧率输入提升为更高目标帧率。 */
static int test_gate_rejects_target_above_input(void)
{
    VideoFrameRateGate gate = {0};

    TEST_ASSERT(video_frame_rate_gate_init(&gate, 30, 60) == MEDIA_ERR_INVALID_PARAM,
                "reject 30 to 60 gate");
    return EXIT_SUCCESS;
}

int main(void)
{
    if (test_sensor_uses_highest_enabled_stream_fps() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_gate_reduces_60_fps_input_to_30_fps() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_gate_keeps_every_frame_when_rates_match() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_gate_rejects_target_above_input() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    printf("video frame rate plan tests passed\n");
    return EXIT_SUCCESS;
}
