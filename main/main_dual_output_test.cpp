extern "C" {
#include "mediaGateway.h"
#include "shellCommandServer.h"
#include "systemDebug.h"
}

int main() {
    MediaGatewayCtx gateway;
    MediaGatewayConfig config = {0};
    int shell_ready = 0;

    /* This executable is a manual integration test for concurrent RTSP + RTMP publishing.
     * The gateway keeps one encoder and fans the resulting H.264 stream out to two independent outputs.
     * Use it when validating queue isolation, reconnect behavior and dual-protocol interoperability.
     */
    config.output.switches.enable_rtsp = 1;
    config.output.switches.enable_rtmp = 1;
    config.system.runtime.low_latency_mode = 1;
    config.system.runtime.stats_interval_sec = 1;
    config.system.runtime.capture_retry_ms = 5;
    config.system.runtime.max_consecutive_failures = 30;
    config.video.encode.rc_mode = MPP_ENC_RC_MODE_CBR;
    config.video.encode.h264_profile = 100;
    config.video.encode.h264_level = 40;
    config.video.encode.h264_cabac_en = 1;
    config.system.record.flush_interval_frames = 30;

    /* RTSP side exposes the live stream for local player validation. */
    config.output.rtsp.session_name = "live";
    config.output.rtsp.server_ip = "0.0.0.0";
    config.output.rtsp.server_port = 8554;
    config.output.rtsp.queue_capacity = 32;

    /* RTMP side pushes the same encoded stream to an ingest server.
     * Replace the URL below with the real publish endpoint in your lab environment.
     */
    config.output.rtmp.publish_url = "rtmp://192.168.1.2/live/stream";
    config.output.rtmp.queue_capacity = 64;
    config.output.rtmp.audio_enabled = 0;

    if (shell_command_server_init(NULL) == 0) {
        shell_ready = 1;
        system_debug_init();
    }

    if (media_gateway_init(&gateway, &config) < 0) {
        if (shell_ready) {
            shell_command_server_deinit();
        }
        return -1;
    }

    {
        if (shell_ready) {
            shell_command_server_start();
        }
        int ret = media_gateway_run(&gateway);
        media_gateway_deinit(&gateway);
        if (shell_ready) {
            shell_command_server_deinit();
        }
        return ret;
    }
}
