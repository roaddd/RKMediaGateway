extern "C" {
#include "rtspStreamer.h"
}

int main() {
    RtspStreamerCtx streamer;

    // Zero-init keeps this as the smallest possible smoke test.
    // rtsp_streamer_init() fills in the default RTSP and encoder settings.
    RtspStreamerConfig config = {0};

    config.low_latency_mode = 1;
    config.stats_interval_sec = 1;
    config.capture_retry_ms = 5;
    config.max_consecutive_failures = 30;
    config.rc_mode = MPP_ENC_RC_MODE_CBR;
    config.h264_profile = 100;
    config.h264_level = 40;
    config.h264_cabac_en = 1;
    // config.record_file_path = "/tmp/record.h264";
    config.record_flush_interval_frames = 30;

    if (rtsp_streamer_init(&streamer, &config) < 0) {
        return -1;
    }

    // Run the capture -> encode -> stream loop.
    int ret = rtsp_streamer_run(&streamer);

    // Always release resources through one path.
    rtsp_streamer_deinit(&streamer);
    return ret;
}
