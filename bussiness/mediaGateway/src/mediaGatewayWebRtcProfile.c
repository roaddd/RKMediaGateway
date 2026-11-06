/**
 * @file mediaGatewayWebRtcProfile.c
 * @brief WebRTC视频质量档位的启动期校验与编码流绑定实现。
 */

#include "mediaGatewayWebRtcProfile.h"

#include "logger.h"

#include <string.h>

/**
 * @description: 返回WebRTC视频质量档位名称，用于配置日志和错误诊断。
 * @param profile WebRTC视频质量档位。
 * @return 稳定的档位名称字符串。
 */
static const char *webrtc_quality_profile_name(MediaGatewayWebRtcQualityProfile profile)
{
    switch (profile)
    {
    case MEDIA_GATEWAY_WEBRTC_QUALITY_SMOOTH:
        return "smooth";
    case MEDIA_GATEWAY_WEBRTC_QUALITY_HD:
        return "hd";
    default:
        return "unknown";
    }
}

/**
 * @description: 校验一个档位所引用的WebRTC输出参数。
 * @param config WebRTC输出配置。
 * @param stream_index 该配置所属的视频流下标，仅用于错误诊断。
 * @return 0表示有效，-1表示参数无效。
 */
static int validate_webrtc_output_config(const MediaOutputWebRtcConfig *config,
                                         int stream_index)
{
    if (!config)
    {
        LOG_ERROR("WebRTC profile validation failed: output config is NULL stream=%d",
                  stream_index);
        return -1;
    }
    if (config->name[0] == '\0' ||
        config->queue_capacity <= 0 ||
        config->media.video_fps <= 0)
    {
        LOG_ERROR("WebRTC profile validation failed: stream=%d name=%s queue=%d fps=%d",
                  stream_index,
                  config->name,
                  config->queue_capacity,
                  config->media.video_fps);
        return -1;
    }
    if (config->signaling.stream_id[0] == '\0')
    {
        LOG_ERROR("WebRTC profile validation failed: logical stream_id is empty stream=%d",
                  stream_index);
        return -1;
    }
    return 0;
}

/**
 * @description: 按配置名称查找已启用的视频编码流。
 * @param config 网关配置。
 * @param stream_name 需要查找的码流名称。
 * @return 找到时返回码流下标，失败时返回-1并打印错误日志。
 */
static int find_enabled_stream_by_name(const MediaGatewayConfig *config,
                                       const char *stream_name)
{
    int stream_index = 0;

    if (!config || !stream_name || stream_name[0] == '\0')
    {
        LOG_ERROR("WebRTC video binding invalid: config or stream name is empty");
        return -1;
    }

    for (stream_index = 0; stream_index < config->video.stream_count; ++stream_index)
    {
        if (!config->video.streams[stream_index].enabled ||
            !config->video.streams[stream_index].name)
            continue;
        if (strcmp(config->video.streams[stream_index].name, stream_name) == 0)
            return stream_index;
    }

    LOG_ERROR("WebRTC video binding invalid: enabled stream name=%s was not found",
              stream_name);
    return -1;
}

int media_gateway_resolve_webrtc_video_binding(MediaGatewayConfig *config)
{
    MediaGatewayWebRtcVideoBindingConfig *binding = NULL;
    const MediaGatewayStreamConfig *smooth_stream = NULL;
    const MediaGatewayStreamConfig *hd_stream = NULL;
    const char *selected_stream_name = NULL;
    int smooth_stream_index = -1;
    int hd_stream_index = -1;
    int selected_stream_index = -1;
    int stream_index = 0;

    if (!config)
    {
        LOG_ERROR("WebRTC video binding failed: config is NULL");
        return -1;
    }

    binding = &config->output.webrtc_video;
    binding->selected_stream_index = -1;
    for (stream_index = 0; stream_index < config->video.stream_count; ++stream_index)
        config->video.streams[stream_index].enable_webrtc = 0;

    /* WebRTC总开关关闭时不解析档位，也不为任何视频流创建WebRTC输出。 */
    if (!config->output.switches.enable_webrtc)
        return 0;

    if (binding->active_profile < MEDIA_GATEWAY_WEBRTC_QUALITY_SMOOTH ||
        binding->active_profile >= MEDIA_GATEWAY_WEBRTC_QUALITY_BUTT)
    {
        LOG_ERROR("WebRTC video binding invalid: active_profile=%d must be smooth(0) or hd(1)",
                  binding->active_profile);
        return -1;
    }
    if (!binding->smooth.stream_name || binding->smooth.stream_name[0] == '\0' ||
        !binding->hd.stream_name || binding->hd.stream_name[0] == '\0')
    {
        LOG_ERROR("WebRTC video binding invalid: smooth and hd stream names must be set");
        return -1;
    }
    if (strcmp(binding->smooth.stream_name, binding->hd.stream_name) == 0)
    {
        LOG_ERROR("WebRTC video binding invalid: smooth and hd both reference stream=%s",
                  binding->smooth.stream_name);
        return -1;
    }

    /* 启动时同时解析两个档位，避免以后切换配置才发现备用码流不存在。 */
    smooth_stream_index = find_enabled_stream_by_name(config, binding->smooth.stream_name);
    if (smooth_stream_index < 0)
    {
        LOG_ERROR("WebRTC smooth profile binding failed: stream=%s",
                  binding->smooth.stream_name);
        return -1;
    }
    hd_stream_index = find_enabled_stream_by_name(config, binding->hd.stream_name);
    if (hd_stream_index < 0)
    {
        LOG_ERROR("WebRTC hd profile binding failed: stream=%s",
                  binding->hd.stream_name);
        return -1;
    }

    smooth_stream = &config->video.streams[smooth_stream_index];
    hd_stream = &config->video.streams[hd_stream_index];
    if (validate_webrtc_output_config(&smooth_stream->webrtc, smooth_stream_index) != 0 ||
        validate_webrtc_output_config(&hd_stream->webrtc, hd_stream_index) != 0)
    {
        LOG_ERROR("WebRTC video binding invalid: smooth or hd output configuration failed validation");
        return -1;
    }

    /* RTP时钟配置必须与实际编码流帧率一致，防止接收端计算出错误的播放节奏。 */
    if (smooth_stream->webrtc.media.video_fps != smooth_stream->fps ||
        hd_stream->webrtc.media.video_fps != hd_stream->fps)
    {
        LOG_ERROR("WebRTC video binding invalid: RTP fps must match bound stream fps smooth=%d/%d hd=%d/%d",
                  smooth_stream->webrtc.media.video_fps,
                  smooth_stream->fps,
                  hd_stream->webrtc.media.video_fps,
                  hd_stream->fps);
        return -1;
    }

    /* 两档只切换内部物理码流，云端和浏览器继续使用同一个逻辑stream_id。 */
    if (strcmp(smooth_stream->webrtc.signaling.stream_id,
               hd_stream->webrtc.signaling.stream_id) != 0)
    {
        LOG_ERROR("WebRTC video binding invalid: quality profiles must share one logical stream_id smooth=%s hd=%s",
                  smooth_stream->webrtc.signaling.stream_id,
                  hd_stream->webrtc.signaling.stream_id);
        return -1;
    }

    if (binding->active_profile == MEDIA_GATEWAY_WEBRTC_QUALITY_SMOOTH)
    {
        selected_stream_index = smooth_stream_index;
        selected_stream_name = binding->smooth.stream_name;
    }
    else
    {
        selected_stream_index = hd_stream_index;
        selected_stream_name = binding->hd.stream_name;
    }

    /* 派生出唯一启用标志，后续输出初始化无需理解质量档位。 */
    binding->selected_stream_index = selected_stream_index;
    config->video.streams[selected_stream_index].enable_webrtc = 1;
    LOG_INFO("WebRTC video profile resolved: profile=%s stream=%s index=%d size=%dx%d fps=%d bitrate=%d",
             webrtc_quality_profile_name(binding->active_profile),
             selected_stream_name,
             selected_stream_index,
             config->video.streams[selected_stream_index].width,
             config->video.streams[selected_stream_index].height,
             config->video.streams[selected_stream_index].fps,
             config->video.streams[selected_stream_index].bitrate);
    return 0;
}
