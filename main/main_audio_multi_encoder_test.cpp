#define __STDC_FORMAT_MACROS

#include "audioCapture.h"
#include "audioEncoder.h"
#include "audioFrameSource.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @description: 以“长度前缀 + 编码负载”格式保存一个编码包，便于保留 Opus/AAC 包边界。
 */
static int write_encoded_packet(FILE *file, const AudioEncoderOutput *output)
{
    uint32_t packet_size = 0;
    size_t written = 0;

    if (!file || !output || !output->data || output->size == 0 || output->size > UINT32_MAX) {
        fprintf(stderr,
                "[AUDIO_MULTI][ERROR] write packet invalid file=%p output=%p data=%p size=%zu\n",
                (void *)file,
                (const void *)output,
                output ? (const void *)output->data : NULL,
                output ? output->size : 0);
        return -1;
    }
    packet_size = (uint32_t)output->size;
    written = fwrite(&packet_size, 1, sizeof(packet_size), file);
    if (written != sizeof(packet_size)) {
        fprintf(stderr, "[AUDIO_MULTI][ERROR] write packet size failed written=%zu\n", written);
        return -1;
    }
    written = fwrite(output->data, 1, output->size, file);
    if (written != output->size) {
        fprintf(stderr,
                "[AUDIO_MULTI][ERROR] write packet payload failed expected=%zu written=%zu\n",
                output->size,
                written);
        return -1;
    }
    return 0;
}

/**
 * @description: 验证同一 48kHz 双声道采集帧可同时驱动四种编码组，重点检查 AAC+Opus 并行输出。
 */
int main(int argc, char **argv)
{
    AudioCaptureCtx capture = {0};
    AudioFrameSource source = {0};
    AudioCaptureConfig capture_config = {0};
    AudioEncoderManagerHandle *manager = NULL;
    AudioEncoderParams g711a_config = {};
    AudioEncoderParams g711u_config = {};
    AudioEncoderParams aac_config = {};
    AudioEncoderParams opus_config = {};
    AudioEncoderRuntimeGroupId g711a_group_id = AUDIO_ENCODER_INVALID_GROUP_ID;
    AudioEncoderRuntimeGroupId g711u_group_id = AUDIO_ENCODER_INVALID_GROUP_ID;
    AudioEncoderRuntimeGroupId aac_group_id = AUDIO_ENCODER_INVALID_GROUP_ID;
    AudioEncoderRuntimeGroupId opus_group_id = AUDIO_ENCODER_INVALID_GROUP_ID;
    AudioEncoderRuntimeGroupId duplicate_group_id = AUDIO_ENCODER_INVALID_GROUP_ID;
    AudioEncoderPcmInput input = {0};
    AudioEncoderOutput aac_output = {0};
    AudioEncoderOutput opus_output = {0};
    AudioEncoderOutput g711a_output = {0};
    AudioEncoderOutput g711u_output = {0};
    AudioFrame frame = {0};
    FILE *aac_file = NULL;
    FILE *opus_file = NULL;
    const char *device = NULL;
    const char *aac_path = NULL;
    const char *opus_path = NULL;
    int seconds = 0;
    int target_frames = 0;
    int slot_index = -1;
    int reused = 0;
    int ret = 1;
    int acquire_ret = 0;
    int i = 0;
    uint64_t aac_packets = 0;
    uint64_t aac_bytes = 0;
    uint64_t aac_empty = 0;
    uint64_t opus_packets = 0;
    uint64_t opus_bytes = 0;
    uint64_t g711a_packets = 0;
    uint64_t g711u_packets = 0;

    device = (argc > 1) ? argv[1] : "hw:0,0";
    seconds = (argc > 2) ? atoi(argv[2]) : 10;
    aac_path = (argc > 3) ? argv[3] : "audio_multi_aac.packets";
    opus_path = (argc > 4) ? argv[4] : "audio_multi_opus.packets";
    if (seconds <= 0)
        seconds = 10;

    capture_config.device_name = device;
    capture_config.sample_rate = 48000;
    capture_config.channels = 2;
    capture_config.format = AUDIO_SAMPLE_FORMAT_S16LE;
    capture_config.period_frames = 960;
    capture_config.buffer_periods = 4;
    if (audio_capture_init(&capture, &capture_config) != 0) {
        fprintf(stderr, "[AUDIO_MULTI][ERROR] capture init failed device=%s\n", device);
        goto cleanup;
    }
    if (audio_frame_source_init(&source, &capture, 8, 5, 30) != 0 ||
        audio_frame_source_start(&source) != 0) {
        fprintf(stderr, "[AUDIO_MULTI][ERROR] audio frame source init/start failed\n");
        goto cleanup;
    }
    if (audio_encoder_manager_create(&manager) != 0) {
        fprintf(stderr, "[AUDIO_MULTI][ERROR] encoder manager create failed\n");
        goto cleanup;
    }

    g711a_config.codec = MEDIA_CODEC_G711A;
    g711a_config.sample_rate = 8000;
    g711a_config.channels = 1;
    g711a_config.input_channel = 0;
    g711a_config.max_samples_per_frame = 160;
    reused = 0;
    if (audio_encoder_manager_register(manager, &g711a_config, &g711a_group_id, &reused) != 0 || reused) {
        fprintf(stderr, "[AUDIO_MULTI][ERROR] G711A group register failed reused=%d\n", reused);
        goto cleanup;
    }
    g711u_config = g711a_config;
    g711u_config.codec = MEDIA_CODEC_G711U;
    reused = 0;
    if (audio_encoder_manager_register(manager, &g711u_config, &g711u_group_id, &reused) != 0 || reused) {
        fprintf(stderr, "[AUDIO_MULTI][ERROR] G711U group register failed reused=%d\n", reused);
        goto cleanup;
    }

    aac_config.codec = MEDIA_CODEC_AAC;
    aac_config.sample_rate = 48000;
    aac_config.channels = 1;
    aac_config.input_channel = 0;
    aac_config.max_samples_per_frame = 960;
    aac_config.codec_params.aac.bitrate = 32000;
    aac_config.codec_params.aac.profile = 2;
    reused = 0;
    if (audio_encoder_manager_register(manager, &aac_config, &aac_group_id, &reused) != 0 || reused) {
        fprintf(stderr, "[AUDIO_MULTI][ERROR] AAC group register failed reused=%d\n", reused);
        goto cleanup;
    }

    opus_config.codec = MEDIA_CODEC_OPUS;
    opus_config.sample_rate = 48000;
    opus_config.channels = 1;
    opus_config.input_channel = 0;
    opus_config.max_samples_per_frame = 960;
    opus_config.codec_params.opus.bitrate = 24000;
    opus_config.codec_params.opus.complexity = 6;
    opus_config.codec_params.opus.enable_vbr = 1;
    opus_config.codec_params.opus.enable_fec = 1;
    opus_config.codec_params.opus.packet_loss_percent = 10;
    reused = 0;
    if (audio_encoder_manager_register(manager, &opus_config, &opus_group_id, &reused) != 0 || reused) {
        fprintf(stderr, "[AUDIO_MULTI][ERROR] Opus group register failed reused=%d\n", reused);
        goto cleanup;
    }

    /* 再注册一次相同 Opus 参数，验证参数键去重不会创建第三个编码器。 */
    reused = 0;
    if (audio_encoder_manager_register(manager,
                                       &opus_config,
                                       &duplicate_group_id,
                                       &reused) != 0 ||
        !reused || duplicate_group_id != opus_group_id ||
        audio_encoder_manager_group_count(manager) != 4) {
        fprintf(stderr,
                "[AUDIO_MULTI][ERROR] group dedup failed reused=%d opus=%llu duplicate=%llu count=%zu\n",
                reused,
                (unsigned long long)opus_group_id,
                (unsigned long long)duplicate_group_id,
                audio_encoder_manager_group_count(manager));
        goto cleanup;
    }

    aac_file = fopen(aac_path, "wb");
    opus_file = fopen(opus_path, "wb");
    if (!aac_file || !opus_file) {
        fprintf(stderr,
                "[AUDIO_MULTI][ERROR] open output failed aac=%s opus=%s\n",
                aac_path,
                opus_path);
        goto cleanup;
    }

    target_frames = seconds * capture.config.sample_rate / capture.config.period_frames;
    for (i = 0; i < target_frames; ++i) {
        slot_index = -1;
        acquire_ret = audio_frame_source_acquire(&source, &frame, &slot_index, 1000);
        if (acquire_ret <= 0) {
            fprintf(stderr,
                    "[AUDIO_MULTI][ERROR] acquire failed ret=%d frame_index=%d\n",
                    acquire_ret,
                    i);
            goto cleanup;
        }
        input.data = (const int16_t *)frame.data;
        input.samples_per_channel = frame.samples_per_channel;
        input.sample_rate = frame.sample_rate;
        input.channels = frame.channels;
        input.pts_us = frame.pts_us;

        if (audio_encoder_manager_encode(manager, g711a_group_id, &input, &g711a_output) != 0 ||
            g711a_output.codec != MEDIA_CODEC_G711A || g711a_output.size != 160) {
            fprintf(stderr,
                    "[AUDIO_MULTI][ERROR] G711A encode failed frame=%" PRIu64 " size=%zu codec=%d\n",
                    frame.frame_id,
                    g711a_output.size,
                    g711a_output.codec);
            goto cleanup;
        }
        g711a_packets++;
        if (audio_encoder_manager_encode(manager, g711u_group_id, &input, &g711u_output) != 0 ||
            g711u_output.codec != MEDIA_CODEC_G711U || g711u_output.size != 160) {
            fprintf(stderr,
                    "[AUDIO_MULTI][ERROR] G711U encode failed frame=%" PRIu64 " size=%zu codec=%d\n",
                    frame.frame_id,
                    g711u_output.size,
                    g711u_output.codec);
            goto cleanup;
        }
        g711u_packets++;

        if (audio_encoder_manager_encode(manager, aac_group_id, &input, &aac_output) != 0) {
            fprintf(stderr, "[AUDIO_MULTI][ERROR] AAC encode failed frame=%" PRIu64 "\n", frame.frame_id);
            goto cleanup;
        }
        if (aac_output.size > 0) {
            if (write_encoded_packet(aac_file, &aac_output) != 0)
                goto cleanup;
            aac_packets++;
            aac_bytes += aac_output.size;
        } else {
            aac_empty++;
        }

        if (audio_encoder_manager_encode(manager, opus_group_id, &input, &opus_output) != 0) {
            fprintf(stderr, "[AUDIO_MULTI][ERROR] Opus encode failed frame=%" PRIu64 "\n", frame.frame_id);
            goto cleanup;
        }
        if (opus_output.size == 0 || write_encoded_packet(opus_file, &opus_output) != 0) {
            fprintf(stderr,
                    "[AUDIO_MULTI][ERROR] Opus produced invalid output frame=%" PRIu64 " size=%zu\n",
                    frame.frame_id,
                    opus_output.size);
            goto cleanup;
        }
        opus_packets++;
        opus_bytes += opus_output.size;
        audio_frame_source_release(&source, slot_index);
        slot_index = -1;
    }

    if (aac_packets == 0 || opus_packets == 0) {
        fprintf(stderr,
                "[AUDIO_MULTI][ERROR] missing codec output AAC=%" PRIu64 " Opus=%" PRIu64 "\n",
                aac_packets,
                opus_packets);
        goto cleanup;
    }
    printf("[AUDIO_MULTI] PASS groups=%zu G711A_packets=%" PRIu64 " G711U_packets=%" PRIu64
           " AAC_packets=%" PRIu64 " AAC_empty=%" PRIu64 " AAC_bytes=%" PRIu64
           " Opus_packets=%" PRIu64 " Opus_bytes=%" PRIu64 "\n",
           audio_encoder_manager_group_count(manager),
           g711a_packets,
           g711u_packets,
           aac_packets,
           aac_empty,
           aac_bytes,
           opus_packets,
           opus_bytes);
    ret = 0;

cleanup:
    if (slot_index >= 0)
        audio_frame_source_release(&source, slot_index);
    if (aac_file)
        fclose(aac_file);
    if (opus_file)
        fclose(opus_file);
    audio_encoder_manager_destroy(manager);
    audio_frame_source_deinit(&source);
    audio_capture_deinit(&capture);
    return ret;
}
