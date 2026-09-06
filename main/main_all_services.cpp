/*** 
 * @Author: huangkelong
 * @Date: 2026-04-27 23:38:26
 * @LastEditTime: 2026-05-06 23:11:36
 * @LastEditors: huangkelong
 * @Description: 主程序入口，初始化媒体网关并运行
 * @FilePath: \RKMediaGateway\main\main_all_services.cpp
 * @Copyright (c) 2026 by huangkelong, All Rights Reserved.
 */
#include <stdio.h>

#include "defValue.h"
#include "logger.h"

extern "C"
{
#include "mediaGateway.h"
#include "debugCommandServer.h"
#include "systemDebug.h"
}

/* 打印配置快照 */
static void log_main_config_snapshot(const MediaGatewayConfig *config)
{
    const MediaGatewayAudioEncoderGroupConfig *audio_group = NULL;
    const MediaGatewayStreamConfig *s = NULL;
    int i = 0;

    if (!config)
    {
        return;
    }

    LOG_INFO("[MAIN_CFG] source=%s loaded=%d",
             def_value_source_path(),
             def_value_loaded());
    LOG_INFO("[MAIN_CFG] parsed stream_count=%d bench(enable=%d sample_every=%d print_interval_sec=%d) log_level=%d",
             config->video.stream_count,
             config->system.bench.enabled,
             config->system.bench.sample_every,
             config->system.bench.print_interval_sec,
             config->system.log.level);
    LOG_INFO("[MAIN_CFG] parsed light_fps enable=%d normal=%d low_light=%d bright=%d min_switch_ms=%d eval_ms=%d",
             config->policy.light_fps.enabled,
             config->policy.light_fps.targets.normal_fps,
             config->policy.light_fps.targets.low_light_fps,
             config->policy.light_fps.targets.bright_fps,
             config->policy.light_fps.timing.min_switch_interval_ms,
             config->policy.light_fps.timing.evaluate_interval_ms);
    LOG_INFO("[MAIN_CFG] parsed audio enabled=%d device=%s rate=%d capture_channels=%d period_frames=%d encoder_groups=%d",
             config->audio.source.enabled,
             config->audio.source.capture.device_name ? config->audio.source.capture.device_name : "unknown",
             config->audio.source.capture.sample_rate,
             config->audio.source.capture.channels,
             config->audio.source.capture.period_frames,
             config->audio.source.encoder_group_count);
    for (i = 0; i < config->audio.source.encoder_group_count; ++i)
    {
        audio_group = &config->audio.source.encoder_groups[i];
        LOG_INFO("[MAIN_CFG] audio_encoder_group index=%d name=%s codec=%d rate=%d channels=%d input_channel=%d",
                 i,
                 audio_group->name,
                 audio_group->encoder.codec,
                 audio_group->encoder.sample_rate,
                 audio_group->encoder.channels,
                 audio_group->encoder.input_channel);
    }

    for (i = 0; i < config->video.stream_count && i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        s = &config->video.streams[i];
        LOG_INFO("[MAIN_CFG] parsed stream=%d name=%s enabled=%d source=%d size=%dx%d fps=%d bitrate=%d rc=%d out(rtsp=%d rtmp=%d gb28181=%d webrtc=%d) rtsp_immediate_sps_pps=%d",
                 i,
                 s->name ? s->name : "unknown",
                 s->enabled,
                 s->source_index,
                 s->width,
                 s->height,
                 s->fps,
                 s->bitrate,
                 s->rc_mode,
                 s->enable_rtsp,
                 s->enable_rtmp,
                 s->enable_gb28181,
                 s->enable_webrtc,
                 s->rtsp.immediate_sps_pps_on_new_client);
        LOG_INFO("[MAIN_CFG] audio_bindings stream=%d rtsp=%s rtmp=%s gb28181=%s webrtc=%s",
                 i,
                 s->rtsp_audio.encoder_group,
                 s->rtmp_audio.encoder_group,
                 s->gb28181_audio.encoder_group,
                 s->webrtc_audio.encoder_group);
    }
}

int main(int argc, char **argv)
{
    MediaGatewayCtx gateway;
    MediaGatewayConfig config;
    const char *config_path = (argc > 1 && argv[1] && argv[1][0] != '\0') ? argv[1] : "media_gateway.toml";
    int shell_ready = 0;
    int ret = 0;

    if (def_value_init(config_path) != 0)
    {
        fprintf(stderr, "failed to load TOML config: %s\n", config_path);
        return -1;
    }
    def_value_get_media_gateway_config(&config);
    log_set_level((LogLevel)config.system.log.level);
    LOG_INFO("loaded TOML config: %s", def_value_source_path());

    log_main_config_snapshot(&config);

    if (debug_command_server_init(NULL) == 0)
    {
        shell_ready = 1;
        if (system_debug_init() != 0)
        {
            LOG_WARN("system debug init failed");
        }
    }
    else
    {
        LOG_WARN("shell command server init failed, runtime shell debug disabled");
    }

    if (media_gateway_init(&gateway, &config) < 0)
    {
        if (shell_ready)
            debug_command_server_deinit();
        return -1;
    }

    if (shell_ready && debug_command_server_start() != 0)
    {
        LOG_WARN("shell command server start failed, runtime shell debug disabled");
    }

    ret = media_gateway_run(&gateway);
    media_gateway_deinit(&gateway);
    if (shell_ready)
        debug_command_server_deinit();
    return ret;
}
