extern "C" {
#include "rtspStreamer.h"
}

int main() {
    RtspStreamerCtx streamer;

    // Zero-init keeps this as the smallest possible smoke test.
    // rtsp_streamer_init() fills in the default RTSP and encoder settings.
    RtspStreamerConfig config = {0};

    // Initialization order:
    // 1. V4L2 capture
    // 2. MPP encoder
    // 3. RTSP session registration
    if (rtsp_streamer_init(&streamer, &config) < 0) {
        return -1;
    }

    // Run the capture -> encode -> stream loop.
    int ret = rtsp_streamer_run(&streamer);

    // Always release resources through one path.
    rtsp_streamer_deinit(&streamer);
    return ret;
}
