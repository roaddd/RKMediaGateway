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
}

/* 打印配置快照 */
static void log_main_config_snapshot(const MediaGatewayConfig *config)
{
    if (!config)
    {
        return;
    }

    LOG_INFO("[MAIN_CFG] source=%s loaded=%d",
             def_value_source_path(),
             def_value_loaded());
    LOG_INFO("[MAIN_CFG] parsed stream_count=%d bench(enable=%d sample_every=%d print_interval_sec=%d) log_level=%d",
             config->stream_count,
             config->bench_enable,
             config->bench_sample_every,
             config->bench_print_interval_sec,
             config->log_level);
    LOG_INFO("[MAIN_CFG] parsed audio enabled=%d device=%s rate=%d channels=%d period_frames=%d codec=%s bind_stream=%d",
             config->audio.enabled,
             config->audio.device_name ? config->audio.device_name : "unknown",
             config->audio.sample_rate,
             config->audio.channels,
             config->audio.period_frames,
             config->audio.g711_mode == G711_ENCODER_MODE_ULAW ? "g711u" : "g711a",
             config->audio.bind_stream_index);

    for (int i = 0; i < config->stream_count && i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        const MediaGatewayStreamConfig *s = &config->streams[i];
        LOG_INFO("[MAIN_CFG] parsed stream=%d name=%s enabled=%d source=%d size=%dx%d fps=%d bitrate=%d rc=%d out(rtsp=%d rtmp=%d gb28181=%d) rtsp_immediate_sps_pps=%d",
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
                 s->rtsp.immediate_sps_pps_on_new_client);
    }
}

int main(int argc, char **argv)
{
    MediaGatewayCtx gateway;
    MediaGatewayConfig config;
    const char *config_path = (argc > 1 && argv[1] && argv[1][0] != '\0') ? argv[1] : "media_gateway.conf";

    def_value_init(config_path);
    def_value_get_media_gateway_config(&config);
    log_set_level((LogLevel)config.log_level);
    if (def_value_loaded())
    {
        LOG_INFO("loaded config file: %s", def_value_source_path());
    }
    else
    {
        LOG_WARN("config file not found, fallback to defaults: %s", def_value_source_path());
    }

    log_main_config_snapshot(&config);

    if (media_gateway_init(&gateway, &config) < 0)
    {
        return -1;
    }

    int ret = media_gateway_run(&gateway);
    media_gateway_deinit(&gateway);
    return ret;
}
