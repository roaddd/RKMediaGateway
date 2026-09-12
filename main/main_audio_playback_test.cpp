#include "audioPlayback.h"
#include "logger.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/** 测试音默认频率，单位 Hz。 */
static const double AUDIO_PLAYBACK_TEST_TONE_HZ = 1000.0;

/** 测试音幅度占 S16 满量程的比例，使用低音量保护耳机和听力。 */
static const double AUDIO_PLAYBACK_TEST_AMPLITUDE = 0.08;

/** 高精度圆周率，避免依赖非标准 M_PI 宏。 */
static const double AUDIO_PLAYBACK_TEST_PI = 3.14159265358979323846;

/**
 * @description: 解析测试时长并限制合理范围。
 */
static MediaResult parse_duration_seconds(const char *text, int *duration_seconds) {
    char *end = NULL;
    long value = 0;

    if (text == NULL || duration_seconds == NULL) {
        LOG_ERROR("parse_duration_seconds failed: text=%p duration_seconds=%p",
                  (const void *)text,
                  (void *)duration_seconds);
        return MEDIA_ERR_INVALID_PARAM;
    }

    value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 3600) {
        LOG_ERROR("parse_duration_seconds failed: invalid duration=%s, expected 1..3600",
                  text);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    *duration_seconds = static_cast<int>(value);
    return MEDIA_OK;
}

/**
 * @description: 生成一个 period 的低音量双声道交错 S16_LE 正弦波。
 */
static MediaResult fill_sine_period(int16_t *buffer,
                                    size_t period_frames,
                                    int channels,
                                    int sample_rate,
                                    uint64_t first_frame_index) {
    double phase = 0.0;
    double normalized_sample = 0.0;
    int16_t sample = 0;
    size_t frame_index = 0;
    int channel_index = 0;

    if (buffer == NULL || period_frames == 0 || channels <= 0 || sample_rate <= 0) {
        LOG_ERROR("fill_sine_period failed: buffer=%p period_frames=%zu channels=%d rate=%d",
                  (void *)buffer,
                  period_frames,
                  channels,
                  sample_rate);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /*
     * 使用全局帧索引计算相位，保证相邻 period 之间波形连续；
     * 同一帧的样本复制到所有声道，模拟后续 Opus 单声道解码到 RK809 双声道的转换。
     */
    for (frame_index = 0; frame_index < period_frames; ++frame_index) {
        phase = 2.0 * AUDIO_PLAYBACK_TEST_PI * AUDIO_PLAYBACK_TEST_TONE_HZ *
                static_cast<double>(first_frame_index + frame_index) /
                static_cast<double>(sample_rate);
        normalized_sample = std::sin(phase) * AUDIO_PLAYBACK_TEST_AMPLITUDE;
        sample = static_cast<int16_t>(normalized_sample * 32767.0);

        for (channel_index = 0; channel_index < channels; ++channel_index) {
            buffer[frame_index * static_cast<size_t>(channels) +
                   static_cast<size_t>(channel_index)] = sample;
        }
    }

    return MEDIA_OK;
}

/**
 * @description: audioPlayback 独立测试入口，循环生成正弦波并按 period 写入 ALSA。
 */
int main(int argc, char **argv) {
    rkmedia::AudioPlayback playback;
    rkmedia::AudioPlaybackConfig config;
    rkmedia::AudioPlaybackConfig actual_config;
    rkmedia::AudioPlaybackStats stats;
    MediaResult result = MEDIA_OK;
    MediaResult deinit_result = MEDIA_OK;
    const char *device_name = AUDIO_PLAYBACK_DEFAULT_DEVICE;
    int16_t *period_buffer = NULL;
    int duration_seconds = 10;
    uint64_t total_frames = 0;
    uint64_t generated_frames = 0;
    size_t current_period_frames = 0;
    size_t period_sample_count = 0;
    int exit_code = EXIT_SUCCESS;

    memset(&config, 0, sizeof(config));
    memset(&actual_config, 0, sizeof(actual_config));
    memset(&stats, 0, sizeof(stats));

    if (argc > 1) {
        device_name = argv[1];
    }
    if (argc > 2) {
        result = parse_duration_seconds(argv[2], &duration_seconds);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio playback test failed: parse duration result=%d", (int)result);
            return EXIT_FAILURE;
        }
    }

    config.device_name = device_name;
    config.sample_rate = AUDIO_PLAYBACK_DEFAULT_SAMPLE_RATE;
    config.channels = AUDIO_PLAYBACK_DEFAULT_CHANNELS;
    config.format = rkmedia::AUDIO_PLAYBACK_SAMPLE_FORMAT_S16_LE;
    config.period_frames = AUDIO_PLAYBACK_DEFAULT_PERIOD_FRAMES;
    config.buffer_periods = AUDIO_PLAYBACK_DEFAULT_BUFFER_PERIODS;
    config.start_threshold_periods = AUDIO_PLAYBACK_DEFAULT_START_THRESHOLD_PERIODS;

    result = playback.init(&config);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio playback test failed: init device=%s result=%d",
                  device_name,
                  (int)result);
        return EXIT_FAILURE;
    }

    result = playback.getConfig(&actual_config);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio playback test failed: get actual config result=%d", (int)result);
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }

    period_sample_count = (size_t)actual_config.period_frames *
                          (size_t)actual_config.channels;
    period_buffer = static_cast<int16_t *>(
        std::malloc(period_sample_count * sizeof(*period_buffer)));
    if (period_buffer == NULL) {
        LOG_ERROR("audio playback test failed: allocate period buffer samples=%zu",
                  period_sample_count);
        result = MEDIA_ERR_NO_MEMORY;
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }

    total_frames = (uint64_t)duration_seconds *
                   (uint64_t)actual_config.sample_rate;
    LOG_INFO("audio playback test start: device=%s duration=%d tone_hz=%.0f "
             "rate=%d channels=%d period_frames=%d total_frames=%llu",
             actual_config.device_name,
             duration_seconds,
             AUDIO_PLAYBACK_TEST_TONE_HZ,
             actual_config.sample_rate,
             actual_config.channels,
             actual_config.period_frames,
             (unsigned long long)total_frames);

    /*
     * 主循环每次生成并写入一个 period。最后一次只写入剩余帧，
     * 使总播放时长与命令行指定值一致。AudioPlayback::write() 内部负责
     * 部分写入、阻塞等待和 XRUN 恢复。
     */
    while (generated_frames < total_frames) {
        current_period_frames = (size_t)(total_frames - generated_frames);
        if (current_period_frames > (size_t)actual_config.period_frames) {
            current_period_frames = (size_t)actual_config.period_frames;
        }

        result = fill_sine_period(period_buffer,
                                  current_period_frames,
                                  actual_config.channels,
                                  actual_config.sample_rate,
                                  generated_frames);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio playback test failed: generate tone frame=%llu result=%d",
                      (unsigned long long)generated_frames,
                      (int)result);
            exit_code = EXIT_FAILURE;
            goto cleanup;
        }

        result = playback.write(period_buffer, current_period_frames);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio playback test failed: write frame=%llu count=%zu result=%d",
                      (unsigned long long)generated_frames,
                      current_period_frames,
                      (int)result);
            exit_code = EXIT_FAILURE;
            goto cleanup;
        }
        generated_frames += (uint64_t)current_period_frames;
    }

    /* 测试程序需要听到完整尾音，因此在关闭设备前先 drain。 */
    result = playback.drain();
    if (result != MEDIA_OK) {
        LOG_ERROR("audio playback test failed: drain result=%d", (int)result);
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }

    result = playback.getStats(&stats);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio playback test failed: get stats result=%d", (int)result);
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }
    LOG_INFO("audio playback test complete: written_frames=%llu xrun_count=%llu",
             (unsigned long long)stats.written_frames,
             (unsigned long long)stats.xrun_count);

cleanup:
    std::free(period_buffer);
    period_buffer = NULL;

    deinit_result = playback.deinit();
    if (deinit_result != MEDIA_OK) {
        LOG_ERROR("audio playback test failed: deinit result=%d", (int)deinit_result);
        exit_code = EXIT_FAILURE;
    }

    return exit_code;
}
