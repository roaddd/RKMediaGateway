#include "audioCapture.h"

#include "logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <alsa/asoundlib.h>

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
    snd_pcm_state_t state_before_recover;
    const char *state_name;
    uint64_t xrun_count;
    int ret;

    if (!ctx || !ctx->pcm_handle) {
        LOG_ERROR("audio_capture_recover failed: invalid ctx=%p pcm=%p",
                  (void *)ctx,
                  ctx ? ctx->pcm_handle : NULL);
        return -1;
    }
    pcm = (snd_pcm_t *)ctx->pcm_handle;

    if (err == -EPIPE) {
        /*
         * 必须在 prepare() 之前读取状态；prepare() 成功后状态会变成 PREPARED，
         * 再查询就看不到触发本次恢复时的 XRUN 状态了。
         */
        state_before_recover = snd_pcm_state(pcm);
        state_name = snd_pcm_state_name(state_before_recover);
        xrun_count = __atomic_add_fetch(&ctx->xrun_count, 1, __ATOMIC_RELAXED);
        LOG_WARN("audio capture xrun detected: state_before_recover=%s(%d) xrun_count=%llu",
                 state_name ? state_name : "UNKNOWN",
                 (int)state_before_recover,
                 (unsigned long long)xrun_count);
        ret = snd_pcm_prepare(pcm);
        if (ret < 0) {
            LOG_ERROR("audio_capture_recover failed: xrun prepare %s", snd_strerror(ret));
            return -1;
        }
        /* xrun 期间可能丢失未知数量的采样，下一帧重新用单调时钟建立 PTS 锚点。 */
        ctx->pts_initialized = 0;
        ctx->last_capture_done_us = 0;
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
        /* suspend 持续时间不属于连续 PCM 时间线，恢复后不能沿用旧锚点。 */
        ctx->pts_initialized = 0;
        ctx->last_capture_done_us = 0;
        return 0;
    }
    if (err == -EAGAIN) {
        return 0;
    }

    LOG_ERROR("audio_capture_recover failed: unrecoverable read error %s", snd_strerror(err));
    return -1;
}

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
    /*
     * 这里只先保存与数据布局有关的信息。用户态 period_buffer 必须等 ALSA 完成
     * _near 参数协商后再分配，因为驱动返回的实际 period 可能大于请求值。
     */
    ctx->bytes_per_sample = bytes_per_sample;
    ctx->frame_bytes = normalized.channels * bytes_per_sample;
    ctx->config = normalized;

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

        /*
         * 1. 打开阻塞式采集 PCM。
         * 最后一个参数为 0，snd_pcm_readi() 在请求的数据尚未准备好时会阻塞等待。
         */
        ret = snd_pcm_open(&pcm, normalized.device_name, SND_PCM_STREAM_CAPTURE, 0);
        if (ret < 0) {
            LOG_ERROR("audio_capture_init failed: open device=%s err=%s",
                      normalized.device_name,
                      snd_strerror(ret));
            audio_capture_deinit(ctx);
            return -1;
        }
        ctx->pcm_handle = pcm;

        /*
         * 2. 配置 hw_params：定义 PCM 数据格式和 ALSA 环形缓冲区布局。
         * hw_params_any() 先取得设备支持的完整参数空间；后续每设置一项，ALSA
         * 都会继续收窄其余参数的可选范围，最后由 snd_pcm_hw_params() 提交。
         */
        snd_pcm_hw_params_alloca(&hw_params);
        ret = snd_pcm_hw_params_any(pcm, hw_params);
        if (ret < 0) goto alsa_failed;
        /* 交错布局：同一采样时刻的各声道数据连续存放，例如 L,R,L,R...。 */
        ret = snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
        if (ret < 0) goto alsa_failed;
        /* 当前项目只支持有符号 16-bit little-endian PCM。 */
        ret = snd_pcm_hw_params_set_format(pcm, hw_params, alsa_format);
        if (ret < 0) goto alsa_failed;
        /* 一个 PCM frame 同时包含所有声道各一个 sample。 */
        ret = snd_pcm_hw_params_set_channels(pcm, hw_params, (unsigned int)normalized.channels);
        if (ret < 0) goto alsa_failed;
        /* _near 允许驱动选择最接近的采样率，并通过 rate 返回实际值。 */
        ret = snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, &dir);
        if (ret < 0) goto alsa_failed;
        /*
         * 一个 period 包含 period_frames 个 PCM frame。它通常影响驱动中断/搬运
         * 粒度，但不等同于 snd_pcm_readi() 每次必须读取的数量。
         * 例如 48kHz/960 frames 和 8kHz/160 frames 都对应 20ms。
         * _near 会把 period_frames 回写为驱动当时能接受的最接近值。
         */
        ret = snd_pcm_hw_params_set_period_size_near(pcm, hw_params, &period_frames, &dir);
        if (ret < 0) goto alsa_failed;
        /*
         * buffer_frames 是 ALSA 内核/驱动 PCM 环形缓冲区的总容量，不是本文件中
         * malloc 的用户态 period_buffer。请求值为 period_frames * buffer_periods；
         * 驱动持续向该环形缓冲写入，应用通过 readi() 从中消费数据。
         */
        ret = snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params, &buffer_frames);
        if (ret < 0) goto alsa_failed;
        /* 提交唯一的硬件配置；成功后 PCM 已具备进入 PREPARED 状态的条件。 */
        ret = snd_pcm_hw_params(pcm, hw_params);
        if (ret < 0) goto alsa_failed;

        /*
         * 3. 重新读取最终协商值。
         * 后设置的 buffer 约束仍可能影响 period，不能只相信前面 _near 返回的
         * 中间值；后续缓存分配、读取长度和 Opus 帧长都必须使用最终值。
         */
        ret = snd_pcm_hw_params_get_rate(hw_params, &rate, &dir);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_hw_params_get_period_size(hw_params, &period_frames, &dir);
        if (ret < 0) goto alsa_failed;
        ret = snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_frames);
        if (ret < 0) goto alsa_failed;

        normalized.sample_rate = (int)rate;
        normalized.period_frames = (int)period_frames;
        ctx->config = normalized;

        /*
         * 4. 按“实际 period × 声道数 × 每 sample 字节数”分配用户态缓存。
         * read_frame() 会把一个完整 period 填入这里，再把该内存借给上层使用。
         */
        period_buffer_size = (size_t)period_frames * normalized.channels * bytes_per_sample;
        ctx->period_buffer = (uint8_t *)malloc(period_buffer_size);
        if (!ctx->period_buffer) {
            LOG_ERROR("audio_capture_init failed: period buffer alloc size=%zu", period_buffer_size);
            audio_capture_deinit(ctx);
            return -1;
        }
        ctx->period_buffer_size = period_buffer_size;

        /*
         * 5. 配置 sw_params：不改变格式、period 或 buffer 容量，只控制运行策略。
         * current() 以 ALSA 默认软件参数为起点，修改后再由 snd_pcm_sw_params()
         * 一次性提交。
         */
        snd_pcm_sw_params_alloca(&sw_params);
        ret = snd_pcm_sw_params_current(pcm, sw_params);
        if (ret < 0) goto alsa_failed;
        /*
         * capture 的 start_threshold 表示自动启动门槛：readi() 请求的帧数达到
         * 该值时，PCM 可从 PREPARED 自动进入 RUNNING。设为 1 表示任何有效读取
         * 都可启动采集；即使 xrun 恢复后只需补读不足一个 period，也不会因为
         * 请求帧数小于启动门槛而停留在 PREPARED。它不控制正常运行时的唤醒量。
         */
        ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, 1);
        if (ret < 0) goto alsa_failed;
        /*
         * avail_min 表示至少有多少帧可读时，把 PCM 视为 ready 并唤醒等待者。
         * 设成一个 period，使正常唤醒粒度与上层一次编码的 PCM 粒度一致；它
         * 不会强制 readi() 每次读取一个 period，也不会改变硬件 period_size。
         */
        ret = snd_pcm_sw_params_set_avail_min(pcm, sw_params, period_frames);
        if (ret < 0) goto alsa_failed;
        /* 应用软件运行策略。sw_params 不会重新分配前面的 ALSA 环形缓冲区。 */
        ret = snd_pcm_sw_params(pcm, sw_params);
        if (ret < 0) goto alsa_failed;

        /*
         * 6. 将 PCM 置为 PREPARED。第一次满足 start_threshold 的 readi() 会使其
         * 自动进入 RUNNING；发生 capture overrun 后也通过 prepare() 回到该状态。
         */
        ret = snd_pcm_prepare(pcm);
        if (ret < 0) goto alsa_failed;

        ctx->initialized = 1;
        printf("[AUDIO] capture ready device=%s rate=%d channels=%d period_frames=%d buffer_frames=%lu requested_buffer_periods=%d\n",
               normalized.device_name,
               normalized.sample_rate,
               normalized.channels,
               normalized.period_frames,
               (unsigned long)buffer_frames,
               normalized.buffer_periods);
        return 0;

alsa_failed:
        LOG_ERROR("audio_capture_init failed: configure device=%s err=%s",
                  normalized.device_name,
                  snd_strerror(ret));
        audio_capture_deinit(ctx);
        return -1;
    }
}

/*
 * 读取一个完整 period。
 * 即使 ALSA 单次 readi 返回短读，也会循环补齐，保证上层拿到固定采样数的音频帧。
 */
int audio_capture_read_frame(AudioCaptureCtx *ctx, AudioCaptureFrame *frame) {
    snd_pcm_t *pcm;
    snd_pcm_sframes_t got;
    int frames_read = 0;
    uint64_t start_us;
    uint64_t end_us;
    uint64_t frame_duration_us;

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
            /*
             * 负数表示 ALSA 错误码而不是读取帧数。常见情况包括：
             * -EPIPE：采集缓冲区 overrun，PCM 已进入 XRUN 状态；
             * -ESTRPIPE：设备被系统挂起；
             * -EAGAIN：暂时没有可读数据。
             * recover() 会按错误类型恢复或要求重试，无法恢复时才返回失败。
             */
            if (audio_capture_recover(ctx, (int)got) != 0) {
                LOG_ERROR("audio_capture_read_frame failed: recover got=%ld", (long)got);
                return -1;
            }
            continue;
        }
        if (got == 0) {
            /*
             * 0 表示本次没有搬运任何 PCM 帧，不是 EOF。阻塞模式下很少出现，
             * 但仍保持 frames_read 不变并重试，不能把未填满的 period 交给上层。
             */
            continue;
        }
        /* 正数是实际读取帧数；下一轮将据此前移 dst 并缩小 frames_left。 */
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
    frame->capture_done_us = end_us;
    frame->capture_interval_us = ctx->last_capture_done_us > 0 &&
                                 end_us >= ctx->last_capture_done_us
                                     ? end_us - ctx->last_capture_done_us
                                     : 0;
    ctx->last_capture_done_us = end_us;
    /*
     * 第一帧用 read 返回时刻减去本帧时长，建立当前连续采集段的 PTS 锚点。
     * 后续帧必须按“累计采样数 / 实际采样率”推导 PTS，不能再次使用 read
     * 返回时刻：当 ALSA buffer 中积累了多个 period 时，连续 readi() 可能在几
     * 毫秒内返回，从而把两个实际相隔一个 period 的音频帧标成几乎相同的 PTS。
     *
     * 使用累计采样数统一计算，而不是每帧累加截断后的微秒增量，还可以避免
     * 44100Hz 等采样率不能整除 1000000 时产生长期累计误差。
     */
    frame_duration_us = (uint64_t)frames_read * 1000000ULL /
                        (uint64_t)ctx->config.sample_rate;
    if (!ctx->pts_initialized) {
        ctx->pts_anchor_us = end_us >= frame_duration_us
                                 ? end_us - frame_duration_us
                                 : 0;
        ctx->pts_frames = 0;
        ctx->pts_initialized = 1;
    }
    frame->pts_us = ctx->pts_anchor_us +
                    ctx->pts_frames * 1000000ULL /
                        (uint64_t)ctx->config.sample_rate;
    ctx->pts_frames += (uint64_t)frames_read;
    return 0;
}

/* 调试线程可与采集线程并发读取，使用 relaxed 即可保证计数值本身不发生数据竞争。 */
uint64_t audio_capture_get_xrun_count(const AudioCaptureCtx *ctx) {
    if (!ctx) {
        return 0;
    }
    return __atomic_load_n(&ctx->xrun_count, __ATOMIC_RELAXED);
}

/* 释放采集设备和周期缓冲；允许重复调用。 */
void audio_capture_deinit(AudioCaptureCtx *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->pcm_handle) {
        snd_pcm_drop((snd_pcm_t *)ctx->pcm_handle);
        snd_pcm_close((snd_pcm_t *)ctx->pcm_handle);
        ctx->pcm_handle = NULL;
    }
    free(ctx->period_buffer);
    memset(ctx, 0, sizeof(*ctx));
}
