extern "C" {
#include "mediaGateway.h"
}

int main() {
    MediaGatewayCtx gateway;

    MediaGatewayConfig config = {0};

    config.low_latency_mode = 1;
    config.stats_interval_sec = 1;
    config.capture_retry_ms = 5;
    config.max_consecutive_failures = 30;
    config.rc_mode = MPP_ENC_RC_MODE_CBR;
    config.h264_profile = 100;
    config.h264_level = 40;
    config.h264_cabac_en = 1;
    config.enable_rtsp = 1;
    config.enable_rtmp = 0;
    config.rtsp.session_name = "live";
    config.rtsp.server_ip = "0.0.0.0";
    config.rtsp.server_port = 8554;
    config.rtmp.publish_url = "rtmp://192.168.1.2/live/stream";
    config.rtmp.audio_enabled = 0;
    config.record_flush_interval_frames = 30;

    if (media_gateway_init(&gateway, &config) < 0) {
        return -1;
    }

    int ret = media_gateway_run(&gateway);

    media_gateway_deinit(&gateway);
    return ret;
}
