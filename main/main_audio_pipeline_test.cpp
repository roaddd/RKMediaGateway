#define __STDC_FORMAT_MACROS

#include "audioCapture.h"
#include "audioFrameSource.h"
#include "g711Encoder.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 音频生产管线测试：
 * ALSA capture -> audioFrameSource ring -> G711 encoder -> 本地文件。
 * 这个测试比 main_audio_capture_test 更接近 gateway 运行时路径。
 */
int main(int argc, char **argv) {
    AudioCaptureCtx capture;
    AudioFrameSource source;
    G711EncoderCtx encoder;
    AudioCaptureConfig capture_config;
    G711EncoderConfig encoder_config;
    AudioFrame frame;
    FILE *fp = NULL;
    const uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    MediaCodecType codec = MEDIA_CODEC_NONE;
    const char *device = (argc > 1) ? argv[1] : AUDIO_CAPTURE_DEFAULT_DEVICE;
    const char *output_path = (argc > 2) ? argv[2] : "capture_audio_g711a.bin";
    int seconds = (argc > 3) ? atoi(argv[3]) : 10;
    int target_frames;
    int i;
    int slot_index = -1;

    if (seconds <= 0) seconds = 10;
    memset(&capture, 0, sizeof(capture));
    memset(&source, 0, sizeof(source));
    memset(&encoder, 0, sizeof(encoder));

    /* 8kHz/mono/160 samples 是 G711 语音链路常用的 20ms 帧粒度。 */
    memset(&capture_config, 0, sizeof(capture_config));
    capture_config.device_name = device;
    capture_config.sample_rate = 8000;
    capture_config.channels = 1;
    capture_config.format = AUDIO_SAMPLE_FORMAT_S16LE;
    capture_config.period_frames = 160;
    capture_config.buffer_periods = 4;
    if (audio_capture_init(&capture, &capture_config) != 0) {
        fprintf(stderr, "[AUDIO_PIPELINE][ERROR] capture init failed device=%s\n", device);
        return 1;
    }

    memset(&encoder_config, 0, sizeof(encoder_config));
    encoder_config.mode = G711_ENCODER_MODE_ALAW;
    encoder_config.sample_rate = capture.config.sample_rate;
    encoder_config.channels = capture.config.channels;
    encoder_config.max_samples_per_frame = capture.config.period_frames;
    if (g711_encoder_init(&encoder, &encoder_config) != 0) {
        fprintf(stderr, "[AUDIO_PIPELINE][ERROR] g711 init failed\n");
        audio_capture_deinit(&capture);
        return 1;
    }

    if (audio_frame_source_init(&source, &capture, 8, 5, 30) != 0 ||
        audio_frame_source_start(&source) != 0) {
        fprintf(stderr, "[AUDIO_PIPELINE][ERROR] source start failed\n");
        audio_frame_source_deinit(&source);
        g711_encoder_deinit(&encoder);
        audio_capture_deinit(&capture);
        return 1;
    }

    fp = fopen(output_path, "wb");
    if (!fp) {
        perror("[AUDIO_PIPELINE][ERROR] open output failed");
        audio_frame_source_deinit(&source);
        g711_encoder_deinit(&encoder);
        audio_capture_deinit(&capture);
        return 1;
    }

    target_frames = seconds * capture.config.sample_rate / capture.config.period_frames;
    /*
     * 从 audioFrameSource 按顺序取帧并编码。
     * acquire/release 模型和 gateway 主循环一致，便于暴露积压和丢帧问题。
     */
    for (i = 0; i < target_frames; ++i) {
        int ret = audio_frame_source_acquire(&source, &frame, &slot_index, 1000);
        if (ret <= 0) {
            fprintf(stderr, "[AUDIO_PIPELINE][ERROR] acquire failed ret=%d i=%d\n", ret, i);
            break;
        }
        if (g711_encoder_encode_s16le(&encoder,
                                      (const int16_t *)frame.data,
                                      frame.samples_per_channel,
                                      &encoded,
                                      &encoded_size,
                                      &codec) != 0) {
            fprintf(stderr, "[AUDIO_PIPELINE][ERROR] encode failed frame=%" PRIu64 "\n", frame.frame_id);
            audio_frame_source_release(&source, slot_index);
            break;
        }
        fwrite(encoded, 1, encoded_size, fp);
        if ((i % 50) == 0) {
            printf("[AUDIO_PIPELINE] frame=%" PRIu64 " pts=%" PRIu64 " pcm=%zu g711=%zu codec=%d dropped=%" PRIu64 "\n",
                   frame.frame_id,
                   frame.pts_us,
                   frame.size,
                   encoded_size,
                   codec,
                   audio_frame_source_get_dropped_frames(&source));
        }
        audio_frame_source_release(&source, slot_index);
    }

    fclose(fp);
    audio_frame_source_deinit(&source);
    g711_encoder_deinit(&encoder);
    audio_capture_deinit(&capture);
    printf("[AUDIO_PIPELINE] done output=%s\n", output_path);
    return 0;
}
