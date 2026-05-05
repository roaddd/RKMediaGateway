#define __STDC_FORMAT_MACROS

#include "audioCapture.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 单模块音频采集测试：
 * 只验证 AudioCapture 是否能从 ALSA 设备稳定读出 PCM S16LE。
 * 输出文件可用 ffplay 按 raw PCM 参数播放。
 */
int main(int argc, char **argv) {
    AudioCaptureCtx capture;
    AudioCaptureConfig config;
    AudioCaptureFrame frame;
    FILE *fp = NULL;
    const char *device = (argc > 1) ? argv[1] : AUDIO_CAPTURE_DEFAULT_DEVICE;
    const char *output_path = (argc > 2) ? argv[2] : "capture_audio_s16le.pcm";
    int seconds = (argc > 3) ? atoi(argv[3]) : 10;
    int target_frames;
    int i;

    if (seconds <= 0) {
        seconds = 10;
    }

    /* 使用 GB28181/G711 友好的默认参数：8kHz、单声道、20ms 一帧。 */
    memset(&config, 0, sizeof(config));
    config.device_name = device;
    config.sample_rate = 8000;
    config.channels = 1;
    config.format = AUDIO_SAMPLE_FORMAT_S16LE;
    config.period_frames = 160;
    config.buffer_periods = 4;

    if (audio_capture_init(&capture, &config) != 0) {
        fprintf(stderr, "[AUDIO_TEST][ERROR] audio_capture_init failed device=%s\n", device);
        return 1;
    }

    fp = fopen(output_path, "wb");
    if (!fp) {
        perror("[AUDIO_TEST][ERROR] open output failed");
        audio_capture_deinit(&capture);
        return 1;
    }

    target_frames = seconds * capture.config.sample_rate / capture.config.period_frames;
    printf("[AUDIO_TEST] recording device=%s output=%s seconds=%d frames=%d\n",
           device,
           output_path,
           seconds,
           target_frames);

    /* 逐 period 读取并落盘，日志里观察 read 耗时和 xrun 次数。 */
    for (i = 0; i < target_frames; ++i) {
        if (audio_capture_read_frame(&capture, &frame) != 0) {
            fprintf(stderr, "[AUDIO_TEST][ERROR] read frame failed i=%d\n", i);
            break;
        }
        if (fwrite(frame.data, 1, frame.size, fp) != frame.size) {
            fprintf(stderr, "[AUDIO_TEST][ERROR] write short frame=%" PRIu64 "\n", frame.frame_id);
            break;
        }
        if ((i % 50) == 0) {
            printf("[AUDIO_TEST] frame=%" PRIu64 " pts_us=%" PRIu64 " bytes=%zu read_us=%" PRIu64 " xruns=%" PRIu64 "\n",
                   frame.frame_id,
                   frame.pts_us,
                   frame.size,
                   frame.read_us,
                   frame.xrun_count);
        }
    }

    fclose(fp);
    audio_capture_deinit(&capture);
    printf("[AUDIO_TEST] done, play with: ffplay -f s16le -ar %d -ac %d %s\n",
           config.sample_rate,
           config.channels,
           output_path);
    return 0;
}
