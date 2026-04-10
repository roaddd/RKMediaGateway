#include <cstdlib>

extern "C" {
#include "mediaGateway.h"
}

static const char *env_or_default(const char *name, const char *fallback) {
    const char *value = std::getenv(name);
    if (value && value[0] != '\0') {
        return value;
    }
    return fallback;
}

static int env_or_default_int(const char *name, int fallback) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    return std::atoi(value);
}

int main() {
    MediaGatewayCtx gateway;
    MediaGatewayConfig config = {0};
    const char *local_ip = env_or_default("GB28181_LOCAL_IP", "192.168.1.3");

    config.low_latency_mode = 1;
    config.stats_interval_sec = 1;
    config.capture_retry_ms = 5;
    config.max_consecutive_failures = 30;
    config.rc_mode = MPP_ENC_RC_MODE_CBR;
    config.h264_profile = env_or_default_int("GB28181_H264_PROFILE", 100);
    config.h264_level = env_or_default_int("GB28181_H264_LEVEL", 40);
    config.h264_cabac_en = env_or_default_int("GB28181_H264_CABAC_EN", 1);
    config.fps = env_or_default_int("GB28181_FPS", 25);
    config.bitrate = env_or_default_int("GB28181_BITRATE", 2 * 1024 * 1024);
    config.gop = env_or_default_int("GB28181_GOP", 25);

    config.enable_rtsp = 0;
    config.enable_rtmp = 0;
    config.enable_gb28181 = 1;

    config.gb28181.server_ip = env_or_default("GB28181_SERVER_IP", "192.168.1.1");
    config.gb28181.server_port = env_or_default_int("GB28181_SERVER_PORT", 5060);
    config.gb28181.server_domain = env_or_default("GB28181_SERVER_DOMAIN", "4401020049");
    config.gb28181.server_id = env_or_default("GB28181_SERVER_ID", "44010200492000000001");
    config.gb28181.device_id = env_or_default("GB28181_DEVICE_ID", "34020000001320000001");
    config.gb28181.device_domain = env_or_default("GB28181_DEVICE_DOMAIN", config.gb28181.server_domain);
    config.gb28181.device_password = env_or_default("GB28181_DEVICE_PASSWORD", "admin123");
    config.gb28181.bind_ip = env_or_default("GB28181_BIND_IP", "0.0.0.0");
    config.gb28181.local_sip_port = env_or_default_int("GB28181_LOCAL_SIP_PORT", 5060);
    config.gb28181.sip_contact_ip = env_or_default("GB28181_CONTACT_IP", local_ip);
    config.gb28181.media_ip = env_or_default("GB28181_MEDIA_IP", config.gb28181.sip_contact_ip);
    config.gb28181.media_port = env_or_default_int("GB28181_MEDIA_PORT", 30000);
    config.gb28181.register_expires = env_or_default_int("GB28181_REGISTER_EXPIRES", 3600);
    config.gb28181.keepalive_interval_sec = env_or_default_int("GB28181_KEEPALIVE_INTERVAL", 60);
    config.gb28181.register_retry_interval_sec = env_or_default_int("GB28181_REGISTER_RETRY_INTERVAL", 5);
    config.gb28181.device_name = env_or_default("GB28181_DEVICE_NAME", "RK3568 Camera");
    config.gb28181.manufacturer = env_or_default("GB28181_MANUFACTURER", "Topeet");
    config.gb28181.model = env_or_default("GB28181_MODEL", "RKMediaGateway");
    config.gb28181.firmware = env_or_default("GB28181_FIRMWARE", "1.0.0");
    config.gb28181.channel_id = env_or_default("GB28181_CHANNEL_ID", config.gb28181.device_id);
    config.gb28181.user_agent = env_or_default("GB28181_USER_AGENT", "RKMediaGateway-GB28181/1.0");

    if (media_gateway_init(&gateway, &config) < 0) {
        return -1;
    }
    int ret = media_gateway_run(&gateway);
    media_gateway_deinit(&gateway);
    return ret;
}
