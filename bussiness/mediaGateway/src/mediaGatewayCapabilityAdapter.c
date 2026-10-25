/**
 * @file mediaGatewayCapabilityAdapter.c
 * @brief MediaGateway 视频配置到全局视频编码能力需求的转换实现。
 */

#include "mediaGatewayCapabilityAdapter.h"

#include "logger.h"

#include <string.h>

/**
 * @brief 把网关码流配置转换为 capability 模块的通用需求数组。
 *
 * 转换时保留原始码流下标和 enabled 状态，使能力模块返回的
 * rejected_stream_index 能够直接对应 MediaGateway 的 stream 下标。
 */
static MediaResult build_video_requirements(
    const MediaGatewayVideoConfig *video_config,
    MediaVideoEncodeRequirement *requirements,
    size_t requirement_capacity,
    size_t *requirement_count)
{
    int stream_index = 0;

    if (!video_config || !requirements || !requirement_count)
    {
        LOG_ERROR("build_video_requirements failed: invalid argument config=%p requirements=%p count=%p",
                  (const void *)video_config,
                  (void *)requirements,
                  (void *)requirement_count);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (video_config->stream_count < 0 ||
        video_config->stream_count > MEDIA_GATEWAY_MAX_STREAMS ||
        (size_t)video_config->stream_count > requirement_capacity)
    {
        LOG_ERROR("build_video_requirements failed: stream_count=%d capacity=%lu",
                  video_config->stream_count,
                  (unsigned long)requirement_capacity);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    memset(requirements, 0, requirement_capacity * sizeof(requirements[0]));
    for (stream_index = 0; stream_index < video_config->stream_count; ++stream_index)
    {
        requirements[stream_index].enabled = video_config->streams[stream_index].enabled;
        requirements[stream_index].codec = MEDIA_CODEC_H264;
        requirements[stream_index].width = video_config->streams[stream_index].width;
        requirements[stream_index].height = video_config->streams[stream_index].height;
        requirements[stream_index].fps = video_config->streams[stream_index].fps;
    }

    *requirement_count = (size_t)video_config->stream_count;
    return MEDIA_OK;
}

MediaResult media_gateway_evaluate_video_capability(
    const MediaGatewayVideoConfig *video_config,
    MediaVideoEncodeCapability *capability,
    MediaVideoCapabilityValidation *validation)
{
    MediaVideoEncodeRequirement requirements[MEDIA_GATEWAY_MAX_STREAMS];
    size_t requirement_count = 0;
    MediaResult result = MEDIA_ERR;

    if (!video_config || !capability || !validation)
    {
        LOG_ERROR("media_gateway_evaluate_video_capability failed: config=%p capability=%p validation=%p",
                  (const void *)video_config,
                  (void *)capability,
                  (void *)validation);
        return MEDIA_ERR_INVALID_PARAM;
    }

    memset(requirements, 0, sizeof(requirements));
    memset(capability, 0, sizeof(*capability));
    memset(validation, 0, sizeof(*validation));

    /* 先取得平台能力快照，保证调试输出和随后执行的准入判断来自同一能力条目。 */
    result = media_capability_get_video_encode(MEDIA_CODEC_H264, capability);
    if (result != MEDIA_OK)
    {
        LOG_ERROR("media_gateway_evaluate_video_capability failed: query H.264 capability result=%d",
                  result);
        return result;
    }

    /* 再转换当前网关配置，并交给全局能力模块完成全部边界和组合负载校验。 */
    result = build_video_requirements(video_config,
                                      requirements,
                                      MEDIA_GATEWAY_MAX_STREAMS,
                                      &requirement_count);
    if (result != MEDIA_OK)
    {
        LOG_ERROR("media_gateway_evaluate_video_capability failed: build requirements result=%d",
                  result);
        return result;
    }

    result = media_capability_validate_video_encode(requirements,
                                                    requirement_count,
                                                    validation);
    if (result != MEDIA_OK)
    {
        LOG_ERROR("media_gateway_evaluate_video_capability failed: result=%d reason=%s rejected_stream=%d active_instances=%d requested_pixels_per_second=%llu limit_pixels_per_second=%llu",
                  result,
                  media_video_capability_code_string(validation->code),
                  validation->rejected_stream_index,
                  validation->active_encoder_instances,
                  (unsigned long long)validation->requested_pixels_per_second,
                  (unsigned long long)validation->limit_pixels_per_second);
    }
    return result;
}

MediaResult media_gateway_evaluate_video_source_fps(
    const MediaGatewayVideoConfig *video_config,
    int source_index,
    int target_fps,
    MediaVideoEncodeCapability *capability,
    MediaVideoCapabilityValidation *validation)
{
    MediaGatewayVideoConfig candidate_config = {0};
    MediaResult result = MEDIA_ERR;
    int stream_index = 0;
    int matched_stream_count = 0;

    if (!video_config || !capability || !validation || source_index < 0 || target_fps <= 0)
    {
        LOG_ERROR("media_gateway_evaluate_video_source_fps failed: config=%p source=%d target_fps=%d capability=%p validation=%p",
                  (const void *)video_config,
                  source_index,
                  target_fps,
                  (void *)capability,
                  (void *)validation);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (video_config->stream_count < 0 || video_config->stream_count > MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("media_gateway_evaluate_video_source_fps failed: invalid stream_count=%d",
                  video_config->stream_count);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    candidate_config = *video_config;
    /* 同一采集源绑定的启用码流必须作为一个候选组合同时切换和校验。 */
    for (stream_index = 0; stream_index < candidate_config.stream_count; ++stream_index)
    {
        if (!candidate_config.streams[stream_index].enabled ||
            candidate_config.streams[stream_index].source_index != source_index)
        {
            continue;
        }
        candidate_config.streams[stream_index].fps = target_fps;
        ++matched_stream_count;
    }
    if (matched_stream_count == 0)
    {
        LOG_ERROR("media_gateway_evaluate_video_source_fps failed: no enabled stream for source=%d",
                  source_index);
        return MEDIA_ERR_NOT_FOUND;
    }

    result = media_gateway_evaluate_video_capability(&candidate_config,
                                                      capability,
                                                      validation);
    return result;
}
