#include <stdio.h>

#include "defValue.h"

extern "C"
{
#include "mediaGateway.h"
}

static void log_main_config_snapshot(const MediaGatewayConfig *config)
{
    if (!config)
    {
        return;
    }

    printf("[MAIN_CFG] source=%s loaded=%d\n",
           def_value_source_path(),
           def_value_loaded());
    printf("[MAIN_CFG] parsed stream_count=%d bench(enable=%d sample_every=%d print_interval_sec=%d)\n",
           config->stream_count,
           config->bench_enable,
           config->bench_sample_every,
           config->bench_print_interval_sec);

    for (int i = 0; i < config->stream_count && i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        const MediaGatewayStreamConfig *s = &config->streams[i];
        printf("[MAIN_CFG] parsed stream=%d name=%s enabled=%d source=%d size=%dx%d fps=%d bitrate=%d rc=%d out(rtsp=%d rtmp=%d gb28181=%d) rtsp_immediate_sps_pps=%d\n",
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
    const char *config_path = (argc > 1 && argv[1] && argv[1][0] != '\0') ? argv[1] : "rtsp_gateway.conf";

    def_value_init(config_path);
    if (def_value_loaded())
    {
        printf("[INFO] loaded config file: %s\n", def_value_source_path());
    }
    else
    {
        printf("[WARN] config file not found, fallback to defaults: %s\n", def_value_source_path());
    }

    def_value_get_media_gateway_config(&config);
    log_main_config_snapshot(&config);

    if (media_gateway_init(&gateway, &config) < 0)
    {
        return -1;
    }

    int ret = media_gateway_run(&gateway);
    media_gateway_deinit(&gateway);
    return ret;
}
