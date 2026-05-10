#include "audioCapture.h"

#include "logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(ENABLE_ALSA_AUDIO)
#include <alsa/asoundlib.h>
#endif

static uint64_t audio_capture_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* 设备名兜底，避免上层传空字符串导致 ALSA open 行为不可控。 */
static const char *audio_capture_safe_device(const char *device_name) {
    return (device_name && device_name[0] != '\0') ? device_name : AUDIO_CAPTURE_DEFAULT_DEVICE;
}

/* 归一化配置：所有默认值、边界修正和当前能力检查集中在入口完成。 */
static int audio_capture_normalize_config(AudioCaptureConfig *dst, const AudioCaptureConfig *src) {
    memset(dst, 0, sizeof(*dst));
    if (src) {
        *dst = *src;
    }

    dst->device_name = audio_capture_safe_device(dst->device_name);
    if (dst->sample_rate <= 0) dst->sample_rate = AUDIO_CAPTURE_DEFAULT_SAMPLE_RATE;
    if (dst->channels <= 0) dst->channels = AUDIO_CAPTURE_DEFAULT_CHANNELS;
    if (dst->format == 0) dst->format = AUDIO_SAMPLE_FORMAT_S16LE;
    if (dst->period_frames <= 0) dst->period_frames = AUDIO_CAPTURE_DEFAULT_PERIOD_FRAMES;
    if (dst->buffer_periods <= 0) dst->buffer_periods = AUDIO_CAPTURE_DEFAULT_BUFFER_PERIODS;

    if (dst->channels > 2) {
        LOG_ERROR("audio_capture_normalize_config failed: unsupported channel count=%d", dst->channels);
        return -1;
    }
    if (dst->format != AUDIO_SAMPLE_FORMAT_S16LE) {
        LOG_ERROR("audio_capture_normalize_config failed: unsupported sample format=%d", dst->format);
        return -1;
    }
    if (dst->period_frames < 40) {
        dst->period_frames = 40;
    }
    if (dst->buffer_periods < 2) {
        dst->buffer_periods = 2;
    }
    return 0;
}

/* 当前只支持 S16LE；独立成函数是为了后续扩展 S24/S32 时不碰调用方。 */
static int audio_capture_bytes_per_sample(AudioSampleFormat format) {
    if (format == AUDIO_SAMPLE_FORMAT_S16LE) {
        return 2;
    }
    return 0;
}

#if defined(ENABLE_ALSA_AUDIO)
/* 将项目内部格式枚举映射到 ALSA 格式枚举。 */
static snd_pcm_format_t audio_capture_to_alsa_format(AudioSampleFormat format) {
    if (format == AUDIO_SAMPLE_FORMAT_S16LE) {
        return SND_PCM_FORMAT_S16_LE;
    }
    return SND_PCM_FORMAT_UNKNOWN;
}

/*
 * ALSA 实时采集最常见故障是 xrun/suspend。
 * 这里在底层完成恢复，上层 audioFrameSource 只看到“成功帧”或“不可恢复失败”。
 */
static int audio_capture_recover(AudioCaptureCtx *ctx, int err) {
    snd_pcm_t *pcm;
    int ret;

    if (!ctx || !ctx->pcm_handle) {
        LOG_ERROR("audio_capture_recover failed: invalid ctx=%p pcm=%p",
                  (void *)ctx,
                  ctx ? ctx->pcm_handle : NULL);
        return -1;
    }
    pcm = (snd_pcm_t *)ctx->pcm_handle;

    if (err == -EPIPE) {
        ctx->xrun_count++;
        ret = snd_pcm_prepare(pcm);
        if (ret < 0) {
            LOG_ERROR("audio_capture_recover failed: xrun prepare %s", snd_strerror(ret));
            return -1;
        }
        return 0;
    }
    if (err == -ESTRPIPE) {
        do {
            ret = snd_pcm_resume(pcm);
        } while (ret == -EAGAIN);

        if (ret < 0) {
            ret = snd_pcm_prepare(pcm);
            if (ret < 0) {
                LOG_ERROR("audio_capture_recover failed: suspend prepare %s", snd_strerror(ret));
                return -1;
            }
        }
        return 0;
    }
    if (err == -EAGAIN) {
        return 0;
    }

    LOG_ERROR("audio_capture_recover failed: unrecoverable read error %s", snd_strerror(err));
    return -1;
}
#endif

/*
 * 初始化采集设备。
 * 性能策略：period_buffer 在这里一次性分配，后续 read_frame 热路径不做 malloc/free。
 */
int audio_capture_init(AudioCaptureCtx *ctx, const AudioCaptureConfig *config) {
    AudioCaptureConfig normalized;
    int bytes_per_sample;
    size_t period_buffer_size;

    if (!ctx) {
        LOG_ERROR("audio_capture_init failed: ctx is NULL");
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));

    if (audio_capture_normalize_config(&normalized, config) != 0) {
        LOG_ERROR("audio_capture_init failed: normalize config");
        return -1;
    }
    /* 获取每个声道的一个采样点占多少字节 */
    bytes_per_sample = audio_capture_bytes_per_sample(normalized.format);
    if (bytes_per_sample <= 0) {
        LOG_ERROR("audio_capture_init failed: bytes_per_sample format=%d", normalized.format);
        return -1;
    }
    /**
     * period_frames 是时间维度上的采样帧数，每次read返回的音频数据量是 period_frames * channels * bytes_per_sample 字节。
     * period_frames 过小会增加 CPU 负载和系统调用次数，过大则增加端到端延迟和 xrun 风险。160 帧在 8 kHz 下是 20ms，通常是语音链路的一个合理选择。
     */
    period_buffer_size = (size_t)normalized.period_frames * normalized.channels * bytes_per_sample;
    ctx->period_buffer = (uint8_t *)malloc(period_buffer_size);
    if (!ctx->period_buffer) {
        LOG_ERROR("audio_capture_init failed: period buffer alloc size=%zu", period_buffer_size);
        return -1;
    }
    ctx->period_buffer_size = period_buffer_size;
    ctx->bytes_per_sample = bytes_per_sample;
    ctx->frame_bytes = normalized.channels * bytes_per_sample;
    ctx->config = normalized;

#if defined(ENABLE_ALSA_AUDIO)
    {
        snd_pcm_t *pcm = NULL;
        snd_pcm_hw_params_t *hw_params = NULL;
        snd_pcm_sw_params_t *sw_params = NULL;
        snd_pcm_uframes_t period_frames = (snd_pcm_uframes_t)normalized.period_frames;
        snd_pcm_uframes_t buffer_frames = (snd_pcm_uframes_t)(normalized.period_frames * normalized.buffer_periods);
        unsigned int rate = (unsigned int)normalized.sample_rate;
        snd_pcm_format_t alsa_format = audio_capture_to_alsa_format(normalized.format);
        int dir = 0;
        int ret;

        ret = snd_pcm_open(&pcm, normalized.device_name, SND_PCM_STREAM_CAPTURE, 0);
        if (ret < 0) {
            LOG_ERROR("audio_capture_init failed: open device=%s err=%s",
                      normalized.device_name,
                      snd_strerror(ret));
            audio_capture_deinit(ctx);
            return -1;
        }
        ctx->pcm_handle = pcm;

        snd_pcm_hw_params_alloca(&hw_params);
        ret = snd_pcm_hw_params_any(pcm, hw_params);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_hw_params_set_format(pcm, hw_params, alsa_format);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_hw_params_set_channels(pcm, hw_params, (unsigned int)normalized.channels);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, &dir);
        if (ret < 0) goto alsa_failed;
        /*
         * period 决定采集回调粒度。8kHz/160 samples 即 20ms，
         * 是语音编码、GB28181 和实时传输里比较常用的低延迟粒度。
         */
        ret = snd_pcm_hw_params_set_period_size_near(pcm, hw_params, &period_frames, &dir);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params, &buffer_frames);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_hw_params(pcm, hw_params);
        if (ret < 0) goto alsa_failed;

        snd_pcm_sw_params_alloca(&sw_params);
        ret = snd_pcm_sw_params_current(pcm, sw_params);
        if (ret < 0) goto alsa_failed;
        /* avail_min/start_threshold 设成一个 period，降低等待整块大缓冲的额外延迟。 */
        ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, period_frames);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_sw_params_set_avail_min(pcm, sw_params, period_frames);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_sw_params(pcm, sw_params);
        if (ret < 0) goto alsa_failed;

        normalized.sample_rate = (int)rate;
        normalized.period_frames = (int)period_frames;
        ctx->config = normalized;
        ctx->period_buffer_size = (size_t)normalized.period_frames * normalized.channels * bytes_per_sample;
        ret = snd_pcm_prepare(pcm);
        if (ret < 0) goto alsa_failed;

        ctx->initialized = 1;
        printf("[AUDIO] capture ready device=%s rate=%d channels=%d period_frames=%d buffer_periods=%d\n",
               normalized.device_name,
               normalized.sample_rate,
               normalized.channels,
               normalized.period_frames,
               normalized.buffer_periods);
        return 0;

alsa_failed:
        LOG_ERROR("audio_capture_init failed: configure device=%s err=%s",
                  normalized.device_name,
                  snd_strerror(ret));
        audio_capture_deinit(ctx);
        return -1;
    }
#else
    LOG_ERROR("audio_capture_init failed: ALSA support is not compiled in");
    audio_capture_deinit(ctx);
    return -1;
#endif
}

/*
 * 读取一个完整 period。
 * 即使 ALSA 单次 readi 返回短读，也会循环补齐，保证上层拿到固定采样数的音频帧。
 */
int audio_capture_read_frame(AudioCaptureCtx *ctx, AudioCaptureFrame *frame) {
#if defined(ENABLE_ALSA_AUDIO)
    snd_pcm_t *pcm;
    snd_pcm_sframes_t got;
    int frames_read = 0;
    uint64_t start_us;
    uint64_t end_us;

    if (!ctx || !ctx->initialized || !ctx->pcm_handle || !frame) {
        LOG_ERROR("audio_capture_read_frame failed: invalid args ctx=%p initialized=%d pcm=%p frame=%p",
                  (void *)ctx,
                  ctx ? ctx->initialized : 0,
                  ctx ? ctx->pcm_handle : NULL,
                  (void *)frame);
        return -1;
    }
    pcm = (snd_pcm_t *)ctx->pcm_handle;
    memset(frame, 0, sizeof(*frame));

    start_us = audio_capture_now_us();
    while (frames_read < ctx->config.period_frames) {
        uint8_t *dst = ctx->period_buffer + (size_t)frames_read * ctx->frame_bytes;
        int frames_left = ctx->config.period_frames - frames_read;

        got = snd_pcm_readi(pcm, dst, (snd_pcm_uframes_t)frames_left);
        if (got < 0) {
            if (audio_capture_recover(ctx, (int)got) != 0) {
                LOG_ERROR("audio_capture_read_frame failed: recover got=%ld", (long)got);
                return -1;
            }
            continue;
        }
        if (got == 0) {
            continue;
        }
        frames_read += (int)got;
    }
    end_us = audio_capture_now_us();

    ctx->frame_id++;
    frame->data = ctx->period_buffer;
    frame->size = (size_t)frames_read * ctx->frame_bytes;
    frame->frame_id = ctx->frame_id;
    frame->sample_rate = ctx->config.sample_rate;
    frame->channels = ctx->config.channels;
    frame->format = ctx->config.format;
    frame->samples_per_channel = frames_read;
    frame->read_us = end_us - start_us;
    frame->capture_call_us = frame->read_us;
    frame->xrun_count = ctx->xrun_count;
    /*
     * read 返回时刻接近本 period 的结束点，因此用 end_us 回推一个 period 时长，
     * 得到音频帧起点 PTS，便于后续和视频时间线对齐。
     */
    frame->pts_us = end_us - ((uint64_t)frames_read * 1000000ULL / (uint64_t)ctx->config.sample_rate);
    return 0;
#else
    (void)ctx;
    (void)frame;
    LOG_ERROR("audio_capture_read_frame failed: ALSA support is not compiled in");
    return -1;
#endif
}

/* 释放采集设备和周期缓冲；允许重复调用。 */
void audio_capture_deinit(AudioCaptureCtx *ctx) {
    if (!ctx) {
        return;
    }

#if defined(ENABLE_ALSA_AUDIO)
    if (ctx->pcm_handle) {
        snd_pcm_drop((snd_pcm_t *)ctx->pcm_handle);
        snd_pcm_close((snd_pcm_t *)ctx->pcm_handle);
        ctx->pcm_handle = NULL;
    }
#endif
    free(ctx->period_buffer);
    memset(ctx, 0, sizeof(*ctx));
}
