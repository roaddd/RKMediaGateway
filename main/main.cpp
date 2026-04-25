#include <list>
#include <stdio.h>
#include <string>

#include "simple_config.h"

extern "C"
{
#include "mediaGateway.h"
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

    /* MediaGatewayConfig */
    config.enable_rtsp = cfg_int("GATEWAY_ENABLE_RTSP", 1);
    config.enable_rtmp = cfg_int("GATEWAY_ENABLE_RTMP", 0);
    config.enable_gb28181 = cfg_int("GATEWAY_ENABLE_GB28181", 1);
    config.fps = cfg_int("GATEWAY_FPS", 30);
    config.bitrate = cfg_int("GATEWAY_BITRATE", 2 * 1024 * 1024);
    config.gop = cfg_int("GATEWAY_GOP", 30);
    config.rc_mode = cfg_int("GATEWAY_RC_MODE", MPP_ENC_RC_MODE_CBR);
    config.h264_profile = cfg_int("GATEWAY_H264_PROFILE", 100);
    config.h264_level = cfg_int("GATEWAY_H264_LEVEL", 40);
    config.h264_cabac_en = cfg_int("GATEWAY_H264_CABAC_EN", 1);
    config.qp_init = cfg_int("GATEWAY_QP_INIT", 30);
    config.qp_min = cfg_int("GATEWAY_QP_MIN", 22);
    config.qp_max = cfg_int("GATEWAY_QP_MAX", 44);
    config.qp_min_i = cfg_int("GATEWAY_QP_MIN_I", 20);
    config.qp_max_i = cfg_int("GATEWAY_QP_MAX_I", 40);
    config.qp_max_step = cfg_int("GATEWAY_QP_MAX_STEP", 8);
    config.low_latency_mode = cfg_int("GATEWAY_LOW_LATENCY_MODE", 1);
    config.stats_interval_sec = cfg_int("GATEWAY_STATS_INTERVAL_SEC", 1);
    config.capture_retry_ms = cfg_int("GATEWAY_CAPTURE_RETRY_MS", 5);
    config.max_consecutive_failures = cfg_int("GATEWAY_MAX_CONSECUTIVE_FAILURES", 30);
    config.record_file_path = cfg_str("GATEWAY_RECORD_FILE_PATH", "");
    config.record_flush_interval_frames = cfg_int("GATEWAY_RECORD_FLUSH_INTERVAL_FRAMES", 30);
    config.config_file_path = cfg_str("GATEWAY_CONFIG_FILE_PATH", config_path);
    config.bench_enable = cfg_int("GATEWAY_BENCH_ENABLE", 0);
    config.bench_sample_every = cfg_int("GATEWAY_BENCH_SAMPLE_EVERY", 1);
    config.bench_print_interval_sec = cfg_int("GATEWAY_BENCH_PRINT_INTERVAL_SEC", 1);

    /* RtspSinkConfig */
    config.rtsp.name = cfg_str("RTSP_NAME", "rtsp");
    config.rtsp.session_name = cfg_str("RTSP_SESSION_NAME", "live");
    config.rtsp.server_ip = cfg_str("RTSP_SERVER_IP", "0.0.0.0");
    config.rtsp.server_port = cfg_int("RTSP_SERVER_PORT", 8554);
    config.rtsp.auth_enable = cfg_int("RTSP_AUTH_ENABLE", 0);
    config.rtsp.user = cfg_str("RTSP_USER", "admin");
    config.rtsp.password = cfg_str("RTSP_PASSWORD", "123456");
    config.rtsp.queue_capacity = cfg_int("RTSP_QUEUE_CAPACITY", 32);

    /* RtmpSinkConfig */
    config.rtmp.name = cfg_str("RTMP_NAME", "rtmp");
    config.rtmp.publish_url = cfg_str("RTMP_PUBLISH_URL", "rtmp://192.168.1.2/live/stream");
    config.rtmp.queue_capacity = cfg_int("RTMP_QUEUE_CAPACITY", 64);
    config.rtmp.reconnect_interval_ms = cfg_int("RTMP_RECONNECT_INTERVAL_MS", 1000);
    config.rtmp.connect_timeout_ms = cfg_int("RTMP_CONNECT_TIMEOUT_MS", 3000);
    config.rtmp.audio_enabled = cfg_int("RTMP_AUDIO_ENABLED", 0);
    config.rtmp.video_width = cfg_int("RTMP_VIDEO_WIDTH", CAPTURE_WIDTH);
    config.rtmp.video_height = cfg_int("RTMP_VIDEO_HEIGHT", CAPTURE_HEIGHT);
    config.rtmp.video_fps = cfg_int("RTMP_VIDEO_FPS", config.fps);
    config.rtmp.video_bitrate = cfg_int("RTMP_VIDEO_BITRATE", config.bitrate);
    config.rtmp.video_codec_name = cfg_str("RTMP_VIDEO_CODEC_NAME", "H264");
    config.rtmp.encoder_name = cfg_str("RTMP_ENCODER_NAME", "RKMediaGateway");

    /* Gb28181SinkConfig */
    config.gb28181.name = cfg_str("GB28181_NAME", "gb28181");
    config.gb28181.server_ip = cfg_str("GB28181_SERVER_IP", "192.168.1.1");
    config.gb28181.server_port = cfg_int("GB28181_SERVER_PORT", 5060);
    config.gb28181.server_domain = cfg_str("GB28181_SERVER_DOMAIN", "3402000000");
    config.gb28181.server_id = cfg_str("GB28181_SERVER_ID", "34020000002000000001");
    config.gb28181.device_id = cfg_str("GB28181_DEVICE_ID", "34020000001320000001");
    config.gb28181.device_domain = cfg_str("GB28181_DEVICE_DOMAIN", config.gb28181.server_domain);
    config.gb28181.device_password = cfg_str("GB28181_DEVICE_PASSWORD", "12345678");
    config.gb28181.bind_ip = cfg_str("GB28181_BIND_IP", "0.0.0.0");
    config.gb28181.local_sip_port = cfg_int("GB28181_LOCAL_SIP_PORT", 5060);
    config.gb28181.sip_contact_ip = cfg_str("GB28181_CONTACT_IP", "192.168.1.100");
    config.gb28181.media_ip = cfg_str("GB28181_MEDIA_IP", config.gb28181.sip_contact_ip);
    config.gb28181.media_port = cfg_int("GB28181_MEDIA_PORT", 30000);
    config.gb28181.register_expires = cfg_int("GB28181_REGISTER_EXPIRES", 3600);
    config.gb28181.keepalive_interval_sec = cfg_int("GB28181_KEEPALIVE_INTERVAL", 60);
    config.gb28181.register_retry_interval_sec = cfg_int("GB28181_REGISTER_RETRY_INTERVAL", 5);
    config.gb28181.device_name = cfg_str("GB28181_DEVICE_NAME", "RK3568 Camera");
    config.gb28181.manufacturer = cfg_str("GB28181_MANUFACTURER", "Topeet");
    config.gb28181.model = cfg_str("GB28181_MODEL", "RKMediaGateway");
    config.gb28181.firmware = cfg_str("GB28181_FIRMWARE", "1.0.0");
    config.gb28181.channel_id = cfg_str("GB28181_CHANNEL_ID", config.gb28181.device_id);
    config.gb28181.user_agent = cfg_str("GB28181_USER_AGENT", "RKMediaGateway-GB28181/1.0");
    config.gb28181.queue_capacity = cfg_int("GB28181_QUEUE_CAPACITY", 64);

    if (media_gateway_init(&gateway, &config) < 0)
    {
        return -1;
    }

    int ret = media_gateway_run(&gateway);

    media_gateway_deinit(&gateway);
    return ret;
}
