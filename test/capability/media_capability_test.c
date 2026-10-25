/**
 * @file media_capability_test.c
 * @brief 全局媒体能力查询和视频编码组合校验测试。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mediaCapability.h"

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "[FAIL] %s\n", message); \
            return EXIT_FAILURE; \
        } \
    } while (0)

/**
 * @brief 验证RK3568暂定H.264能力查询结果。
 */
static int test_query_provisional_h264_capability(void) {
    MediaVideoEncodeCapability capability;
    MediaResult ret = MEDIA_ERR;

    memset(&capability, 0, sizeof(capability));
    ret = media_capability_get_video_encode(MEDIA_CODEC_H264, &capability);
    TEST_ASSERT(ret == MEDIA_OK, "query H.264 capability");
    TEST_ASSERT(capability.max_encoder_instances == 2, "provisional instance limit");
    TEST_ASSERT(capability.max_width == 1920, "provisional width limit");
    TEST_ASSERT(capability.max_height == 1080, "provisional height limit");
    TEST_ASSERT(capability.max_single_stream_fps == 60, "provisional FPS limit");
    TEST_ASSERT(capability.max_pixels_per_second == 124416000ULL,
                "provisional aggregate pixel rate");
    TEST_ASSERT(capability.confidence == MEDIA_CAPABILITY_CONFIDENCE_PROVISIONAL,
                "provisional confidence");
    TEST_ASSERT(strcmp(media_capability_confidence_string(capability.confidence),
                       "provisional") == 0,
                "provisional confidence name");
    TEST_ASSERT(capability.verified_combination_count == 0,
                "verified combination list must initially be empty");
    return EXIT_SUCCESS;
}

/**
 * @brief 验证当前1080p30和720p30组合能够通过暂定预算。
 */
static int test_accept_current_combination(void) {
    MediaVideoEncodeRequirement streams[2];
    MediaVideoCapabilityValidation validation;
    MediaResult ret = MEDIA_ERR;

    memset(streams, 0, sizeof(streams));
    memset(&validation, 0, sizeof(validation));
    streams[0].enabled = 1;
    streams[0].codec = MEDIA_CODEC_H264;
    streams[0].width = 1920;
    streams[0].height = 1080;
    streams[0].fps = 30;
    streams[1].enabled = 1;
    streams[1].codec = MEDIA_CODEC_H264;
    streams[1].width = 1280;
    streams[1].height = 720;
    streams[1].fps = 30;

    ret = media_capability_validate_video_encode(streams, 2, &validation);
    TEST_ASSERT(ret == MEDIA_OK, "accept 1080p30 plus 720p30");
    TEST_ASSERT(validation.code == MEDIA_VIDEO_CAPABILITY_OK, "current combination code");
    TEST_ASSERT(validation.active_encoder_instances == 2, "current active instances");
    TEST_ASSERT(validation.requested_pixels_per_second == 89856000ULL,
                "current aggregate pixel rate");
    return EXIT_SUCCESS;
}

/**
 * @brief 验证1080p60和720p30组合在尚未实测前被暂定预算拒绝。
 */
static int test_reject_unverified_target_combination(void) {
    MediaVideoEncodeRequirement streams[2];
    MediaVideoCapabilityValidation validation;
    MediaResult ret = MEDIA_ERR;

    memset(streams, 0, sizeof(streams));
    memset(&validation, 0, sizeof(validation));
    streams[0].enabled = 1;
    streams[0].codec = MEDIA_CODEC_H264;
    streams[0].width = 1920;
    streams[0].height = 1080;
    streams[0].fps = 60;
    streams[1].enabled = 1;
    streams[1].codec = MEDIA_CODEC_H264;
    streams[1].width = 1280;
    streams[1].height = 720;
    streams[1].fps = 30;

    ret = media_capability_validate_video_encode(streams, 2, &validation);
    TEST_ASSERT(ret == MEDIA_ERR_UNSUPPORTED, "reject unverified target combination");
    TEST_ASSERT(validation.code == MEDIA_VIDEO_CAPABILITY_AGGREGATE_LOAD_EXCEEDED,
                "target rejection reason");
    TEST_ASSERT(validation.requested_pixels_per_second == 152064000ULL,
                "target aggregate pixel rate");
    TEST_ASSERT(validation.matched_verified_combination == 0,
                "target combination must not be marked verified");
    return EXIT_SUCCESS;
}

/**
 * @brief 验证单路约束、禁用码流和不支持格式的处理。
 */
static int test_stream_level_validation(void) {
    MediaVideoEncodeRequirement streams[2];
    MediaVideoCapabilityValidation validation;
    MediaResult ret = MEDIA_ERR;

    memset(streams, 0, sizeof(streams));
    memset(&validation, 0, sizeof(validation));
    streams[0].enabled = 1;
    streams[0].codec = MEDIA_CODEC_H264;
    streams[0].width = 1920;
    streams[0].height = 1080;
    streams[0].fps = 61;
    streams[1].enabled = 0;
    streams[1].codec = MEDIA_CODEC_OPUS;

    ret = media_capability_validate_video_encode(streams, 2, &validation);
    TEST_ASSERT(ret == MEDIA_ERR_UNSUPPORTED, "reject excessive single stream FPS");
    TEST_ASSERT(validation.code == MEDIA_VIDEO_CAPABILITY_FPS_EXCEEDED,
                "single stream FPS rejection reason");
    TEST_ASSERT(validation.rejected_stream_index == 0, "rejected stream index");

    streams[0].fps = 30;
    streams[0].codec = MEDIA_CODEC_OPUS;
    ret = media_capability_validate_video_encode(streams, 2, &validation);
    TEST_ASSERT(ret == MEDIA_ERR_UNSUPPORTED, "reject non-video codec");
    TEST_ASSERT(validation.code == MEDIA_VIDEO_CAPABILITY_UNSUPPORTED_CODEC,
                "unsupported codec rejection reason");
    return EXIT_SUCCESS;
}

/**
 * @brief 验证未配置视频流时不会被误判为非法或超出平台能力。
 */
static int test_accept_empty_video_configuration(void) {
    MediaVideoCapabilityValidation validation;
    MediaResult ret = MEDIA_ERR;

    memset(&validation, 0, sizeof(validation));
    ret = media_capability_validate_video_encode(NULL, 0, &validation);
    TEST_ASSERT(ret == MEDIA_OK, "accept empty video configuration");
    TEST_ASSERT(validation.code == MEDIA_VIDEO_CAPABILITY_OK,
                "empty video configuration code");
    TEST_ASSERT(validation.active_encoder_instances == 0,
                "empty video configuration active instances");
    TEST_ASSERT(validation.requested_pixels_per_second == 0,
                "empty video configuration load");
    return EXIT_SUCCESS;
}

int main(void) {
    TEST_ASSERT(test_query_provisional_h264_capability() == EXIT_SUCCESS,
                "query provisional H.264 capability test");
    TEST_ASSERT(test_accept_current_combination() == EXIT_SUCCESS,
                "accept current combination test");
    TEST_ASSERT(test_reject_unverified_target_combination() == EXIT_SUCCESS,
                "reject target combination test");
    TEST_ASSERT(test_stream_level_validation() == EXIT_SUCCESS,
                "stream-level validation test");
    TEST_ASSERT(test_accept_empty_video_configuration() == EXIT_SUCCESS,
                "empty video configuration test");
    printf("media capability tests passed\n");
    return EXIT_SUCCESS;
}
