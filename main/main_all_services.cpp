#include <list>
#include <stdio.h>
#include <string>

#include "simple_config.h"

extern "C"
{
#include "mediaGateway.h"
}

static void fill_stream_config(MediaGatewayStreamConfig *stream,
                               int is_main,
                               simple_config::Reader &file_config,
                               std::list<std::string> &string_pool) {
    const char *prefix = is_main ? "STREAM_MAIN_" : "STREAM_SUB_";
    auto cfg_str = [&](const std::string &suffix, const char *fallback) -> const char * {
        std::string key = prefix + suffix;
        string_pool.push_back(file_config.get_string(key.c_str(), fallback));
        return string_pool.back().c_str();
    };
    auto cfg_int = [&](const std::string &suffix, int fallback) -> int {
        std::string key = prefix + suffix;
        return file_config.get_int(key.c_str(), fallback);
    };

    stream->enabled = cfg_int("ENABLE", is_main ? 1 : 0);
    stream->name = cfg_str("NAME", is_main ? "main" : "sub");
    stream->width = cfg_int("WIDTH", is_main ? CAPTURE_WIDTH : (CAPTURE_WIDTH / 2));
    stream->height = cfg_int("HEIGHT", is_main ? CAPTURE_HEIGHT : (CAPTURE_HEIGHT / 2));
    stream->fps = cfg_int("FPS", is_main ? 30 : 15);
    stream->bitrate = cfg_int("BITRATE", is_main ? (2 * 1024 * 1024) : (1024 * 1024));
    stream->gop = cfg_int("GOP", is_main ? 30 : 30);
    stream->rc_mode = cfg_int("RC_MODE", MPP_ENC_RC_MODE_CBR);
    stream->h264_profile = cfg_int("H264_PROFILE", 100);
    stream->h264_level = cfg_int("H264_LEVEL", 40);
    stream->h264_cabac_en = cfg_int("H264_CABAC_EN", 1);
    stream->qp_init = cfg_int("QP_INIT", 30);
    stream->qp_min = cfg_int("QP_MIN", 22);
    stream->qp_max = cfg_int("QP_MAX", 44);
    stream->qp_min_i = cfg_int("QP_MIN_I", 20);
    stream->qp_max_i = cfg_int("QP_MAX_I", 40);
    stream->qp_max_step = cfg_int("QP_MAX_STEP", 8);

    stream->enable_rtsp = cfg_int("ENABLE_RTSP", is_main ? 1 : 1);
    stream->enable_rtmp = cfg_int("ENABLE_RTMP", 0);
    stream->enable_gb28181 = cfg_int("ENABLE_GB28181", is_main ? 1 : 0);

    stream->rtsp.name = cfg_str("RTSP_NAME", is_main ? "rtsp-main" : "rtsp-sub");
    stream->rtsp.session_name = cfg_str("RTSP_SESSION_NAME", is_main ? "live_main" : "live_sub");
    stream->rtsp.server_ip = cfg_str("RTSP_SERVER_IP", "0.0.0.0");
    stream->rtsp.server_port = cfg_int("RTSP_SERVER_PORT", 8554);
    stream->rtsp.auth_enable = cfg_int("RTSP_AUTH_ENABLE", 0);
    stream->rtsp.user = cfg_str("RTSP_USER", "admin");
    stream->rtsp.password = cfg_str("RTSP_PASSWORD", "123456");
    stream->rtsp.queue_capacity = cfg_int("RTSP_QUEUE_CAPACITY", 32);
    stream->rtsp.immediate_sps_pps_on_new_client = cfg_int("RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT", 0);

    stream->rtmp.name = cfg_str("RTMP_NAME", is_main ? "rtmp-main" : "rtmp-sub");
    stream->rtmp.publish_url = cfg_str("RTMP_PUBLISH_URL", "");
    stream->rtmp.queue_capacity = cfg_int("RTMP_QUEUE_CAPACITY", 64);
    stream->rtmp.reconnect_interval_ms = cfg_int("RTMP_RECONNECT_INTERVAL_MS", 1000);
    stream->rtmp.connect_timeout_ms = cfg_int("RTMP_CONNECT_TIMEOUT_MS", 3000);
    stream->rtmp.audio_enabled = cfg_int("RTMP_AUDIO_ENABLED", 0);
    stream->rtmp.video_width = cfg_int("RTMP_VIDEO_WIDTH", stream->width);
    stream->rtmp.video_height = cfg_int("RTMP_VIDEO_HEIGHT", stream->height);
    stream->rtmp.video_fps = cfg_int("RTMP_VIDEO_FPS", stream->fps);
    stream->rtmp.video_bitrate = cfg_int("RTMP_VIDEO_BITRATE", stream->bitrate);
    stream->rtmp.video_codec_name = cfg_str("RTMP_VIDEO_CODEC_NAME", "H264");
    stream->rtmp.encoder_name = cfg_str("RTMP_ENCODER_NAME", "RKMediaGateway");

    stream->gb28181.name = cfg_str("GB28181_NAME", is_main ? "gb28181-main" : "gb28181-sub");
    stream->gb28181.server_ip = cfg_str("GB28181_SERVER_IP", "192.168.1.1");
    stream->gb28181.server_port = cfg_int("GB28181_SERVER_PORT", 5060);
    stream->gb28181.server_domain = cfg_str("GB28181_SERVER_DOMAIN", "3402000000");
    stream->gb28181.server_id = cfg_str("GB28181_SERVER_ID", "34020000002000000001");
    stream->gb28181.device_id = cfg_str("GB28181_DEVICE_ID", "34020000001320000001");
    stream->gb28181.device_domain = cfg_str("GB28181_DEVICE_DOMAIN", stream->gb28181.server_domain);
    stream->gb28181.device_password = cfg_str("GB28181_DEVICE_PASSWORD", "12345678");
    stream->gb28181.bind_ip = cfg_str("GB28181_BIND_IP", "0.0.0.0");
    stream->gb28181.local_sip_port = cfg_int("GB28181_LOCAL_SIP_PORT", 5060);
    stream->gb28181.sip_contact_ip = cfg_str("GB28181_CONTACT_IP", "192.168.1.100");
    stream->gb28181.media_ip = cfg_str("GB28181_MEDIA_IP", stream->gb28181.sip_contact_ip);
    stream->gb28181.media_port = cfg_int("GB28181_MEDIA_PORT", 30000);
    stream->gb28181.register_expires = cfg_int("GB28181_REGISTER_EXPIRES", 3600);
    stream->gb28181.keepalive_interval_sec = cfg_int("GB28181_KEEPALIVE_INTERVAL", 60);
    stream->gb28181.register_retry_interval_sec = cfg_int("GB28181_REGISTER_RETRY_INTERVAL", 5);
    stream->gb28181.device_name = cfg_str("GB28181_DEVICE_NAME", "RK3568 Camera");
    stream->gb28181.manufacturer = cfg_str("GB28181_MANUFACTURER", "Topeet");
    stream->gb28181.model = cfg_str("GB28181_MODEL", "RKMediaGateway");
    stream->gb28181.firmware = cfg_str("GB28181_FIRMWARE", "1.0.0");
    stream->gb28181.channel_id = cfg_str("GB28181_CHANNEL_ID", stream->gb28181.device_id);
    stream->gb28181.user_agent = cfg_str("GB28181_USER_AGENT", "RKMediaGateway-GB28181/1.0");
    stream->gb28181.queue_capacity = cfg_int("GB28181_QUEUE_CAPACITY", 64);
}

static void log_main_config_snapshot(const MediaGatewayConfig *config, simple_config::Reader &file_config) {
    if (!config) return;
    printf("[MAIN_CFG] source=%s loaded=%d\n",
           file_config.source_path().c_str(),
           file_config.loaded() ? 1 : 0);
    printf("[MAIN_CFG] raw GATEWAY_STREAM_COUNT=%d STREAM_MAIN_ENABLE=%d STREAM_SUB_ENABLE=%d\n",
           file_config.get_int("GATEWAY_STREAM_COUNT", -999),
           file_config.get_int("STREAM_MAIN_ENABLE", -999),
           file_config.get_int("STREAM_SUB_ENABLE", -999));
    printf("[MAIN_CFG] raw MAIN out rtsp=%d rtmp=%d gb28181=%d\n",
           file_config.get_int("STREAM_MAIN_ENABLE_RTSP", -999),
           file_config.get_int("STREAM_MAIN_ENABLE_RTMP", -999),
           file_config.get_int("STREAM_MAIN_ENABLE_GB28181", -999));
    printf("[MAIN_CFG] raw SUB  out rtsp=%d rtmp=%d gb28181=%d\n",
           file_config.get_int("STREAM_SUB_ENABLE_RTSP", -999),
           file_config.get_int("STREAM_SUB_ENABLE_RTMP", -999),
           file_config.get_int("STREAM_SUB_ENABLE_GB28181", -999));

    printf("[MAIN_CFG] parsed stream_count=%d\n", config->stream_count);
    for (int i = 0; i < config->stream_count && i < MEDIA_GATEWAY_MAX_STREAMS; ++i) {
        const MediaGatewayStreamConfig *s = &config->streams[i];
        printf("[MAIN_CFG] parsed stream=%d name=%s enabled=%d size=%dx%d fps=%d bitrate=%d rc=%d out(rtsp=%d rtmp=%d gb28181=%d) rtsp_immediate_sps_pps=%d\n",
               i,
               s->name ? s->name : "unknown",
               s->enabled,
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
    MediaGatewayConfig config = {0};
    simple_config::Reader file_config;
    std::list<std::string> string_pool;
    const char *config_path = (argc > 1 && argv[1] && argv[1][0] != '\0') ? argv[1] : "rtsp_gateway.conf";

    if (file_config.load(config_path))
    {
        printf("[INFO] loaded config file: %s\n", config_path);
    }
    else
    {
        printf("[WARN] config file not found, fallback to defaults: %s\n", config_path);
    }

    auto cfg_str = [&](const char *key, const char *fallback) -> const char * {
        string_pool.push_back(file_config.get_string(key, fallback));
        return string_pool.back().c_str();
    };
    auto cfg_int = [&](const char *key, int fallback) -> int {
        return file_config.get_int(key, fallback);
    };

    config.low_latency_mode = cfg_int("GATEWAY_LOW_LATENCY_MODE", 1);
    config.stats_interval_sec = cfg_int("GATEWAY_STATS_INTERVAL_SEC", 1);
    config.capture_retry_ms = cfg_int("GATEWAY_CAPTURE_RETRY_MS", 5);
    config.max_consecutive_failures = cfg_int("GATEWAY_MAX_CONSECUTIVE_FAILURES", 30);
    config.record_file_path = cfg_str("GATEWAY_RECORD_FILE_PATH", "");
    config.record_flush_interval_frames = cfg_int("GATEWAY_RECORD_FLUSH_INTERVAL_FRAMES", 30);
    config.config_file_path = cfg_str("GATEWAY_CONFIG_FILE_PATH", config_path);
    config.stream_count = cfg_int("GATEWAY_STREAM_COUNT", 2);

    fill_stream_config(&config.streams[0], 1, file_config, string_pool);
    fill_stream_config(&config.streams[1], 0, file_config, string_pool);

    /* Legacy compatibility path: if STREAM_* keys are absent, preserve existing single-stream keys. */
    if (file_config.get_int("STREAM_MAIN_ENABLE", -1) < 0)
    {
        config.stream_count = 1;
        config.streams[0].enabled = 1;
        config.streams[0].name = "main";
        config.streams[0].width = CAPTURE_WIDTH;
        config.streams[0].height = CAPTURE_HEIGHT;
        config.streams[0].fps = cfg_int("GATEWAY_FPS", 30);
        config.streams[0].bitrate = cfg_int("GATEWAY_BITRATE", 2 * 1024 * 1024);
        config.streams[0].gop = cfg_int("GATEWAY_GOP", 30);
        config.streams[0].rc_mode = cfg_int("GATEWAY_RC_MODE", MPP_ENC_RC_MODE_CBR);
        config.streams[0].h264_profile = cfg_int("GATEWAY_H264_PROFILE", 100);
        config.streams[0].h264_level = cfg_int("GATEWAY_H264_LEVEL", 40);
        config.streams[0].h264_cabac_en = cfg_int("GATEWAY_H264_CABAC_EN", 1);
        config.streams[0].qp_init = cfg_int("GATEWAY_QP_INIT", 30);
        config.streams[0].qp_min = cfg_int("GATEWAY_QP_MIN", 22);
        config.streams[0].qp_max = cfg_int("GATEWAY_QP_MAX", 44);
        config.streams[0].qp_min_i = cfg_int("GATEWAY_QP_MIN_I", 20);
        config.streams[0].qp_max_i = cfg_int("GATEWAY_QP_MAX_I", 40);
        config.streams[0].qp_max_step = cfg_int("GATEWAY_QP_MAX_STEP", 8);
        config.streams[0].enable_rtsp = cfg_int("GATEWAY_ENABLE_RTSP", 1);
        config.streams[0].enable_rtmp = cfg_int("GATEWAY_ENABLE_RTMP", 0);
        config.streams[0].enable_gb28181 = cfg_int("GATEWAY_ENABLE_GB28181", 1);
        config.streams[0].rtsp.immediate_sps_pps_on_new_client =
            cfg_int("GATEWAY_RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT", 0);
    }

    log_main_config_snapshot(&config, file_config);

    if (media_gateway_init(&gateway, &config) < 0)
    {
        return -1;
    }

    int ret = media_gateway_run(&gateway);
    media_gateway_deinit(&gateway);
    return ret;
}
