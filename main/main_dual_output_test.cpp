extern "C" {
#include "mediaGateway.h"
}

int main() {
    MediaGatewayCtx gateway;
    MediaGatewayConfig config = {0};

    /* This executable is a manual integration test for concurrent RTSP + RTMP publishing.
     * The gateway keeps one encoder and fans the resulting H.264 stream out to two independent sinks.
     * Use it when validating queue isolation, reconnect behavior and dual-protocol interoperability.
     */
    config.enable_rtsp = 1;
    config.enable_rtmp = 1;
    config.low_latency_mode = 1;
    config.stats_interval_sec = 1;
    config.capture_retry_ms = 5;
    config.max_consecutive_failures = 30;
    config.rc_mode = MPP_ENC_RC_MODE_CBR;
    config.h264_profile = 100;
    config.h264_level = 40;
    config.h264_cabac_en = 1;
    config.record_flush_interval_frames = 30;

    /* RTSP side exposes the live stream for local player validation. */
    config.rtsp.session_name = "live";
    config.rtsp.server_ip = "0.0.0.0";
    config.rtsp.server_port = 8554;
    config.rtsp.queue_capacity = 32;

    /* RTMP side pushes the same encoded stream to an ingest server.
     * Replace the URL below with the real publish endpoint in your lab environment.
     */
    config.rtmp.publish_url = "rtmp://127.0.0.1/live/stream";
    config.rtmp.queue_capacity = 64;
    config.rtmp.audio_enabled = 0;

    if (media_gateway_init(&gateway, &config) < 0) {
        return -1;
    }

    {
        int ret = media_gateway_run(&gateway);
        media_gateway_deinit(&gateway);
        return ret;
    }
}
