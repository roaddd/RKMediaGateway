/**
 * @file mediaCapability.c
 * @brief RK3568整机媒体能力数据及统一校验实现。
 */

#include "mediaCapability.h"

#include <stdio.h>
#include <string.h>

#define RK3568_PROVISIONAL_MAX_ENCODER_INSTANCES 2
#define RK3568_PROVISIONAL_MAX_WIDTH 1920
#define RK3568_PROVISIONAL_MAX_HEIGHT 1080
#define RK3568_PROVISIONAL_MAX_SINGLE_STREAM_FPS 60
#define RK3568_PROVISIONAL_MAX_PIXELS_PER_SECOND \
    ((uint64_t)1920 * (uint64_t)1080 * (uint64_t)60)
#define RK3568_VERIFIED_COMBINATION_COUNT 1U

/**
 * 当前先采用芯片资料中的1080p60作为H.264总像素预算，因此置信级别必须保持
 * PROVISIONAL。MPP矩阵和整机链路测试完成后，可调整本条目并增加实测组合，
 * 但调用方和校验流程无需变化。
 */
static const MediaVideoEncodeCapability g_h264_capability = {
    MEDIA_CODEC_H264,
    RK3568_PROVISIONAL_MAX_ENCODER_INSTANCES,
    RK3568_PROVISIONAL_MAX_WIDTH,
    RK3568_PROVISIONAL_MAX_HEIGHT,
    RK3568_PROVISIONAL_MAX_SINGLE_STREAM_FPS,
    RK3568_PROVISIONAL_MAX_PIXELS_PER_SECOND,
    RK3568_VERIFIED_COMBINATION_COUNT,
    MEDIA_CAPABILITY_CONFIDENCE_PROVISIONAL
};

/**
 * @brief 初始化校验结果，确保任何提前返回都带有稳定的默认字段。
 */
static void init_validation(MediaVideoCapabilityValidation *validation) {
    if (validation == NULL) {
        return;
    }
    memset(validation, 0, sizeof(*validation));
    validation->code = MEDIA_VIDEO_CAPABILITY_INVALID_ARGUMENT;
    validation->confidence = g_h264_capability.confidence;
    validation->rejected_stream_index = -1;
    validation->limit_pixels_per_second = g_h264_capability.max_pixels_per_second;
}

/**
 * @brief 返回候选配置是否命中实测组合。
 *
 * 当前精确放行1080p60 + 720p30，用于继续执行正式gateway链路的固定负载验证。
 * 该白名单不会提高通用总像素预算，也不会放行720p60等其它超预算组合。
 * 匹配按启用码流集合进行，不依赖主、子码流在配置数组中的排列顺序。
 */
static int matches_verified_combination(
    const MediaVideoEncodeRequirement *streams,
    size_t stream_count) {
    const MediaVideoEncodeRequirement *stream = NULL;
    size_t stream_index = 0;
    int active_stream_count = 0;
    int matched_1080p60 = 0;
    int matched_720p30 = 0;

    if (streams == NULL || stream_count == 0) {
        return 0;
    }

    for (stream_index = 0; stream_index < stream_count; ++stream_index) {
        stream = &streams[stream_index];
        if (!stream->enabled) {
            continue;
        }

        ++active_stream_count;
        if (stream->codec == MEDIA_CODEC_H264 &&
            stream->width == 1920 && stream->height == 1080 && stream->fps == 60) {
            matched_1080p60 = 1;
            continue;
        }
        if (stream->codec == MEDIA_CODEC_H264 &&
            stream->width == 1280 && stream->height == 720 && stream->fps == 30) {
            matched_720p30 = 1;
            continue;
        }

        /* 任意其它启用码流都表示候选配置不是该精确白名单组合。 */
        return 0;
    }

    return active_stream_count == 2 && matched_1080p60 && matched_720p30;
}

MediaResult media_capability_get_video_encode(
    MediaCodecType codec,
    MediaVideoEncodeCapability *capability) {
    if (capability == NULL) {
        fprintf(stderr, "[ERROR] media capability query failed: output is null\n");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (codec != MEDIA_CODEC_H264) {
        fprintf(stderr, "[ERROR] media capability query failed: unsupported codec=%d\n", codec);
        memset(capability, 0, sizeof(*capability));
        return MEDIA_ERR_UNSUPPORTED;
    }

    *capability = g_h264_capability;
    return MEDIA_OK;
}

MediaResult media_capability_validate_video_encode(
    const MediaVideoEncodeRequirement *streams,
    size_t stream_count,
    MediaVideoCapabilityValidation *validation) {
    size_t stream_index = 0;
    const MediaVideoEncodeRequirement *stream = NULL;
    uint64_t stream_pixels_per_second = 0;

    init_validation(validation);
    if (validation == NULL || stream_count > MEDIA_CAPABILITY_MAX_VIDEO_STREAMS ||
        (stream_count > 0 && streams == NULL)) {
        fprintf(stderr,
                "[ERROR] video capability validation failed: streams=%p count=%lu result=%p\n",
                (const void *)streams,
                (unsigned long)stream_count,
                (void *)validation);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* 第一遍只检查基础参数和编码格式，同时统计需要占用的编码实例数。 */
    for (stream_index = 0; stream_index < stream_count; ++stream_index) {
        stream = &streams[stream_index];
        if (!stream->enabled) {
            continue;
        }

        ++validation->active_encoder_instances;
        if (stream->width <= 0 || stream->height <= 0 || stream->fps <= 0) {
            validation->code = MEDIA_VIDEO_CAPABILITY_INVALID_STREAM_CONFIG;
            validation->rejected_stream_index = (int)stream_index;
            return MEDIA_ERR_INVALID_CONFIG;
        }
        if (stream->codec != g_h264_capability.codec) {
            validation->code = MEDIA_VIDEO_CAPABILITY_UNSUPPORTED_CODEC;
            validation->rejected_stream_index = (int)stream_index;
            return MEDIA_ERR_UNSUPPORTED;
        }
    }

    if (validation->active_encoder_instances >
        g_h264_capability.max_encoder_instances) {
        validation->code = MEDIA_VIDEO_CAPABILITY_TOO_MANY_INSTANCES;
        return MEDIA_ERR_UNSUPPORTED;
    }

    /*
     * 实例数量合法后再逐路检查单实例边界并累计负载。乘法前使用uint64_t
     * 转换，避免较大分辨率和帧率在32位平台上发生整数溢出。
     */
    for (stream_index = 0; stream_index < stream_count; ++stream_index) {
        stream = &streams[stream_index];
        if (!stream->enabled) {
            continue;
        }
        if (stream->width > g_h264_capability.max_width) {
            validation->code = MEDIA_VIDEO_CAPABILITY_WIDTH_EXCEEDED;
            validation->rejected_stream_index = (int)stream_index;
            return MEDIA_ERR_UNSUPPORTED;
        }
        if (stream->height > g_h264_capability.max_height) {
            validation->code = MEDIA_VIDEO_CAPABILITY_HEIGHT_EXCEEDED;
            validation->rejected_stream_index = (int)stream_index;
            return MEDIA_ERR_UNSUPPORTED;
        }
        if (stream->fps > g_h264_capability.max_single_stream_fps) {
            validation->code = MEDIA_VIDEO_CAPABILITY_FPS_EXCEEDED;
            validation->rejected_stream_index = (int)stream_index;
            return MEDIA_ERR_UNSUPPORTED;
        }

        stream_pixels_per_second =
            (uint64_t)stream->width * (uint64_t)stream->height * (uint64_t)stream->fps;
        if (UINT64_MAX - validation->requested_pixels_per_second <
            stream_pixels_per_second) {
            validation->code = MEDIA_VIDEO_CAPABILITY_AGGREGATE_LOAD_EXCEEDED;
            return MEDIA_ERR_UNSUPPORTED;
        }
        validation->requested_pixels_per_second += stream_pixels_per_second;
    }

    /* 实测白名单优先于通用预算，可精确放行经过稳定性验证的特殊组合。 */
    validation->matched_verified_combination =
        matches_verified_combination(streams, stream_count);
    if (!validation->matched_verified_combination &&
        validation->requested_pixels_per_second >
            g_h264_capability.max_pixels_per_second) {
        validation->code = MEDIA_VIDEO_CAPABILITY_AGGREGATE_LOAD_EXCEEDED;
        return MEDIA_ERR_UNSUPPORTED;
    }

    validation->code = MEDIA_VIDEO_CAPABILITY_OK;
    return MEDIA_OK;
}

const char *media_capability_confidence_string(MediaCapabilityConfidence confidence) {
    switch (confidence) {
        case MEDIA_CAPABILITY_CONFIDENCE_PROVISIONAL:
            return "provisional";
        case MEDIA_CAPABILITY_CONFIDENCE_VERIFIED:
            return "verified";
        default:
            return "unknown";
    }
}

const char *media_video_capability_code_string(MediaVideoCapabilityCode code) {
    switch (code) {
        case MEDIA_VIDEO_CAPABILITY_OK:
            return "ok";
        case MEDIA_VIDEO_CAPABILITY_INVALID_ARGUMENT:
            return "invalid_argument";
        case MEDIA_VIDEO_CAPABILITY_INVALID_STREAM_CONFIG:
            return "invalid_stream_config";
        case MEDIA_VIDEO_CAPABILITY_UNSUPPORTED_CODEC:
            return "unsupported_codec";
        case MEDIA_VIDEO_CAPABILITY_TOO_MANY_INSTANCES:
            return "too_many_instances";
        case MEDIA_VIDEO_CAPABILITY_WIDTH_EXCEEDED:
            return "width_exceeded";
        case MEDIA_VIDEO_CAPABILITY_HEIGHT_EXCEEDED:
            return "height_exceeded";
        case MEDIA_VIDEO_CAPABILITY_FPS_EXCEEDED:
            return "fps_exceeded";
        case MEDIA_VIDEO_CAPABILITY_AGGREGATE_LOAD_EXCEEDED:
            return "aggregate_load_exceeded";
        default:
            return "unknown";
    }
}
