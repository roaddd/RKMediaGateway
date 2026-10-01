#include "audioPlayback.h"

#include "logger.h"

#include <errno.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <alsa/asoundlib.h>

namespace rkmedia {

/**
 * @description: AudioPlayback 私有实现，隐藏 ALSA 类型和运行状态。
 */
class AudioPlayback::Impl {
public:
    snd_pcm_t *pcm_handle;             /* 当前打开的 ALSA Playback PCM。 */
    AudioPlaybackConfig config;        /* ALSA 协商后的实际配置。 */
    AudioPlaybackStats stats;          /* 播放帧数和 XRUN 统计。 */
    char device_name[128];             /* 对象自有的设备名副本。 */
    size_t frame_bytes;                /* 一个多声道 PCM 帧的字节数。 */
    uint64_t buffer_frames;            /* ALSA 实际缓冲区容量，单位帧。 */
    uint64_t start_threshold_frames;   /* ALSA 实际启动阈值，单位帧。 */
    bool initialized;                  /* true 表示已初始化并可写入。 */

    Impl()
        : pcm_handle(NULL),
          config(),
          stats(),
          device_name(),
          frame_bytes(0),
          buffer_frames(0),
          start_threshold_frames(0),
          initialized(false)
    {
        reset();
    }

    /** @description: 清空运行字段，不执行 ALSA 资源释放。 */
    void reset()
    {
        pcm_handle = NULL;
        memset(&config, 0, sizeof(config));
        memset(&stats, 0, sizeof(stats));
        memset(device_name, 0, sizeof(device_name));
        frame_bytes = 0;
        buffer_frames = 0;
        start_threshold_frames = 0;
        initialized = false;
    }
};

AudioPlayback::AudioPlayback()
    : impl_(new (std::nothrow) Impl())
{
    if (!impl_) {
        LOG_ERROR("AudioPlayback construct failed: allocate implementation");
    }
}

AudioPlayback::~AudioPlayback()
{
    MediaResult result = MEDIA_OK;

    if (impl_ && impl_->pcm_handle != NULL) {
        result = deinit();
        if (result != MEDIA_OK) {
            LOG_ERROR("AudioPlayback destruct failed: deinit result=%d", (int)result);
        }
    }
}

/**
 * @description: 对输入配置应用默认值并检查基本约束。
 */
static MediaResult audio_playback_normalize_config(AudioPlaybackConfig *dst,
                                                   const AudioPlaybackConfig *src) {
    if (dst == NULL) {
        LOG_ERROR("audio_playback_normalize_config failed: dst is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    memset(dst, 0, sizeof(*dst));
    if (src != NULL) {
        *dst = *src;
    }

    if (dst->device_name == NULL || dst->device_name[0] == '\0') {
        dst->device_name = AUDIO_PLAYBACK_DEFAULT_DEVICE;
    }
    if (dst->sample_rate == 0) {
        dst->sample_rate = AUDIO_PLAYBACK_DEFAULT_SAMPLE_RATE;
    }
    if (dst->channels == 0) {
        dst->channels = AUDIO_PLAYBACK_DEFAULT_CHANNELS;
    }
    if (dst->format == AUDIO_PLAYBACK_SAMPLE_FORMAT_INVALID) {
        dst->format = AUDIO_PLAYBACK_SAMPLE_FORMAT_S16_LE;
    }
    if (dst->period_frames == 0) {
        dst->period_frames = AUDIO_PLAYBACK_DEFAULT_PERIOD_FRAMES;
    }
    if (dst->buffer_periods == 0) {
        dst->buffer_periods = AUDIO_PLAYBACK_DEFAULT_BUFFER_PERIODS;
    }
    if (dst->start_threshold_periods == 0) {
        dst->start_threshold_periods = AUDIO_PLAYBACK_DEFAULT_START_THRESHOLD_PERIODS;
    }

    if (dst->sample_rate < 8000 || dst->sample_rate > 192000) {
        LOG_ERROR("audio_playback_normalize_config failed: invalid sample_rate=%d",
                  dst->sample_rate);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (dst->channels < 1 || dst->channels > 8) {
        LOG_ERROR("audio_playback_normalize_config failed: invalid channels=%d",
                  dst->channels);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (dst->format != AUDIO_PLAYBACK_SAMPLE_FORMAT_S16_LE) {
        LOG_ERROR("audio_playback_normalize_config failed: unsupported format=%d",
                  (int)dst->format);
        return MEDIA_ERR_UNSUPPORTED;
    }
    if (dst->period_frames <= 0 || dst->period_frames > dst->sample_rate * 10) {
        LOG_ERROR("audio_playback_normalize_config failed: invalid period_frames=%d",
                  dst->period_frames);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (dst->buffer_periods < 2 || dst->buffer_periods > 1024) {
        LOG_ERROR("audio_playback_normalize_config failed: buffer_periods=%d expected 2..1024",
                  dst->buffer_periods);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (dst->start_threshold_periods < 1 ||
        dst->start_threshold_periods > dst->buffer_periods) {
        LOG_ERROR("audio_playback_normalize_config failed: start_threshold_periods=%d "
                  "buffer_periods=%d",
                  dst->start_threshold_periods,
                  dst->buffer_periods);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    return MEDIA_OK;
}

/**
 * @description: 将模块 PCM 格式映射为 ALSA 格式。
 */
static snd_pcm_format_t audio_playback_to_alsa_format(AudioPlaybackSampleFormat format) {
    snd_pcm_format_t alsa_format = SND_PCM_FORMAT_UNKNOWN;

    if (format == AUDIO_PLAYBACK_SAMPLE_FORMAT_S16_LE) {
        alsa_format = SND_PCM_FORMAT_S16_LE;
    }

    return alsa_format;
}

/**
 * @description: 计算单个样本的字节数。
 */
static int audio_playback_bytes_per_sample(AudioPlaybackSampleFormat format) {
    int bytes_per_sample = 0;

    if (format == AUDIO_PLAYBACK_SAMPLE_FORMAT_S16_LE) {
        bytes_per_sample = 2;
    }

    return bytes_per_sample;
}

/**
 * @description: 将常见 ALSA 错误转换为工程统一错误码。
 */
static MediaResult audio_playback_map_alsa_error(int alsa_error) {
    MediaResult result = MEDIA_ERR;

    if (alsa_error == -EBUSY) {
        result = MEDIA_ERR_BUSY;
    } else if (alsa_error == -EINVAL || alsa_error == -ENOTSUP) {
        result = MEDIA_ERR_UNSUPPORTED;
    }

    return result;
}

/** @brief ALSA 参数协商后供初始化流程使用的实际帧参数。 */
struct AudioPlaybackNegotiatedParams {
    unsigned int sample_rate;                 /* ALSA 实际采用的采样率。 */
    snd_pcm_uframes_t period_frames;          /* ALSA 实际采用的单个 period 帧数。 */
    snd_pcm_uframes_t buffer_frames;          /* ALSA 实际采用的环形缓冲区帧数。 */
    snd_pcm_uframes_t start_threshold_frames; /* 启动 DMA 前需要积累的帧数。 */
};

/**
 * @description: 配置并提交 ALSA Playback 硬件参数，返回驱动实际采用的帧参数。
 */
static MediaResult audio_playback_configure_hw_params(
    snd_pcm_t *pcm,
    const AudioPlaybackConfig *config,
    snd_pcm_format_t alsa_format,
    AudioPlaybackNegotiatedParams *negotiated) {
    snd_pcm_hw_params_t *hw_params = NULL;
    snd_pcm_uframes_t period_frames = 0;
    snd_pcm_uframes_t buffer_frames = 0;
    unsigned int sample_rate = 0;
    int direction = 0;
    int ret = 0;

    if (pcm == NULL || config == NULL || negotiated == NULL) {
        LOG_ERROR("audio_playback_configure_hw_params failed: pcm=%p config=%p negotiated=%p",
                  (void *)pcm,
                  (const void *)config,
                  (void *)negotiated);
        return MEDIA_ERR_INVALID_PARAM;
    }

    period_frames = (snd_pcm_uframes_t)config->period_frames;
    buffer_frames = period_frames * (snd_pcm_uframes_t)config->buffer_periods;
    sample_rate = (unsigned int)config->sample_rate;
    snd_pcm_hw_params_alloca(&hw_params);

    /* 固定交错布局、样本格式和声道数，保证 write() 可直接提交调用方的 PCM。 */
    ret = snd_pcm_hw_params_any(pcm, hw_params);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: hw_params_any err=%s",
                  snd_strerror(ret));
        return audio_playback_map_alsa_error(ret);
    }
    ret = snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: set access=RW_INTERLEAVED err=%s",
                  snd_strerror(ret));
        return MEDIA_ERR_UNSUPPORTED;
    }
    ret = snd_pcm_hw_params_set_format(pcm, hw_params, alsa_format);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: set format=%d err=%s",
                  (int)config->format,
                  snd_strerror(ret));
        return MEDIA_ERR_UNSUPPORTED;
    }
    ret = snd_pcm_hw_params_set_channels(pcm, hw_params, (unsigned int)config->channels);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: set channels=%d err=%s",
                  config->channels,
                  snd_strerror(ret));
        return MEDIA_ERR_UNSUPPORTED;
    }

    /* _near 接口允许驱动调整请求值；采样率必须精确匹配，帧参数则在提交后回读。 */
    ret = snd_pcm_hw_params_set_rate_near(pcm, hw_params, &sample_rate, &direction);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: set sample_rate=%d err=%s",
                  config->sample_rate,
                  snd_strerror(ret));
        return MEDIA_ERR_UNSUPPORTED;
    }
    if (sample_rate != (unsigned int)config->sample_rate) {
        LOG_ERROR("audio_playback_configure_hw_params failed: requested sample_rate=%d actual=%u",
                  config->sample_rate,
                  sample_rate);
        return MEDIA_ERR_UNSUPPORTED;
    }
    ret = snd_pcm_hw_params_set_period_size_near(pcm, hw_params, &period_frames, &direction);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: set period_frames=%d err=%s",
                  config->period_frames,
                  snd_strerror(ret));
        return MEDIA_ERR_UNSUPPORTED;
    }
    ret = snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params, &buffer_frames);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: set buffer_frames=%llu err=%s",
                  (unsigned long long)((uint64_t)config->period_frames *
                                      (uint64_t)config->buffer_periods),
                  snd_strerror(ret));
        return MEDIA_ERR_UNSUPPORTED;
    }
    ret = snd_pcm_hw_params(pcm, hw_params);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: commit hw_params err=%s",
                  snd_strerror(ret));
        return audio_playback_map_alsa_error(ret);
    }

    /* 提交后回读最终值，后续软件阈值和对外实际配置均以协商结果为准。 */
    ret = snd_pcm_hw_params_get_rate(hw_params, &sample_rate, &direction);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: get actual sample_rate err=%s",
                  snd_strerror(ret));
        return MEDIA_ERR;
    }
    ret = snd_pcm_hw_params_get_period_size(hw_params, &period_frames, &direction);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: get actual period_size err=%s",
                  snd_strerror(ret));
        return MEDIA_ERR;
    }
    ret = snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_frames);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_hw_params failed: get actual buffer_size err=%s",
                  snd_strerror(ret));
        return MEDIA_ERR;
    }
    if (period_frames == 0 || buffer_frames / period_frames < 2) {
        LOG_ERROR("audio_playback_configure_hw_params failed: invalid period=%llu buffer=%llu",
                  (unsigned long long)period_frames,
                  (unsigned long long)buffer_frames);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    negotiated->sample_rate = sample_rate;
    negotiated->period_frames = period_frames;
    negotiated->buffer_frames = buffer_frames;
    negotiated->start_threshold_frames =
        period_frames * (snd_pcm_uframes_t)config->start_threshold_periods;
    if (negotiated->start_threshold_frames > buffer_frames) {
        negotiated->start_threshold_frames = buffer_frames;
    }
    return MEDIA_OK;
}

/**
 * @description: 根据硬件协商结果配置 ALSA Playback 唤醒、启动和停止阈值。
 */
static MediaResult audio_playback_configure_sw_params(
    snd_pcm_t *pcm,
    const AudioPlaybackNegotiatedParams *negotiated) {
    snd_pcm_sw_params_t *sw_params = NULL;
    int ret = 0;

    if (pcm == NULL || negotiated == NULL) {
        LOG_ERROR("audio_playback_configure_sw_params failed: pcm=%p negotiated=%p",
                  (void *)pcm,
                  (const void *)negotiated);
        return MEDIA_ERR_INVALID_PARAM;
    }

    snd_pcm_sw_params_alloca(&sw_params);
    ret = snd_pcm_sw_params_current(pcm, sw_params);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_sw_params failed: sw_params_current err=%s",
                  snd_strerror(ret));
        return MEDIA_ERR;
    }

    /*
     * avail_min 以一个 period 为应用层唤醒粒度；start_threshold 预填若干
     * period 后再启动 DMA；数据耗尽达到 stop_threshold 时进入 XRUN 并显式恢复。
     */
    ret = snd_pcm_sw_params_set_avail_min(pcm, sw_params, negotiated->period_frames);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_sw_params failed: set avail_min=%llu err=%s",
                  (unsigned long long)negotiated->period_frames,
                  snd_strerror(ret));
        return MEDIA_ERR;
    }
    ret = snd_pcm_sw_params_set_start_threshold(
        pcm,
        sw_params,
        negotiated->start_threshold_frames);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_sw_params failed: set start_threshold=%llu err=%s",
                  (unsigned long long)negotiated->start_threshold_frames,
                  snd_strerror(ret));
        return MEDIA_ERR;
    }
    ret = snd_pcm_sw_params_set_stop_threshold(pcm, sw_params, negotiated->buffer_frames);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_sw_params failed: set stop_threshold=%llu err=%s",
                  (unsigned long long)negotiated->buffer_frames,
                  snd_strerror(ret));
        return MEDIA_ERR;
    }
    ret = snd_pcm_sw_params(pcm, sw_params);
    if (ret < 0) {
        LOG_ERROR("audio_playback_configure_sw_params failed: commit sw_params err=%s",
                  snd_strerror(ret));
        return MEDIA_ERR;
    }
    return MEDIA_OK;
}

/**
 * @description: 释放初始化过程中已获取的 ALSA 资源。
 */
void AudioPlayback::releaseAfterInitFailure() {
    Impl *ctx = impl_.get();
    snd_pcm_t *pcm = NULL;
    int ret = 0;

    if (ctx == NULL) {
        LOG_ERROR("AudioPlayback releaseAfterInitFailure failed: implementation is NULL");
        return;
    }

    pcm = (snd_pcm_t *)ctx->pcm_handle;
    if (pcm != NULL) {
        ret = snd_pcm_close(pcm);
        if (ret < 0) {
            LOG_ERROR("audio_playback_release_after_init_failure: close failed err=%s",
                      snd_strerror(ret));
        }
    }
    ctx->reset();
}

/**
 * @description: 从 Playback underrun 或设备 suspend 状态恢复。
 */
MediaResult AudioPlayback::recover(int error_code) {
    Impl *ctx = impl_.get();
    snd_pcm_t *pcm = NULL;
    snd_pcm_state_t state_before_recover = SND_PCM_STATE_DISCONNECTED;
    const char *state_name = NULL;
    struct timespec retry_interval = {0, 1000000L};
    uint64_t xrun_count = 0;
    int resume_attempts = 0;
    int ret = 0;

    if (ctx == NULL || ctx->pcm_handle == NULL) {
        LOG_ERROR("audio_playback_recover failed: invalid ctx=%p pcm=%p",
                  (void *)ctx,
                  ctx != NULL ? ctx->pcm_handle : NULL);
        return MEDIA_ERR_INVALID_PARAM;
    }

    pcm = (snd_pcm_t *)ctx->pcm_handle;
    if (error_code == -EPIPE) {
        /*
         * Playback 缓冲区被耗尽时 ALSA 进入 XRUN。先记录恢复前状态和次数，
         * 再 prepare PCM；上层当前尚未写完的数据会在恢复后继续提交。
         */
        state_before_recover = snd_pcm_state(pcm);
        state_name = snd_pcm_state_name(state_before_recover);
        xrun_count = __atomic_add_fetch(&ctx->stats.xrun_count, 1, __ATOMIC_RELAXED);
        LOG_WARN("audio playback xrun detected: state_before_recover=%s(%d) xrun_count=%llu",
                 state_name != NULL ? state_name : "UNKNOWN",
                 (int)state_before_recover,
                 (unsigned long long)xrun_count);

        ret = snd_pcm_prepare(pcm);
        if (ret < 0) {
            LOG_ERROR("audio_playback_recover failed: xrun prepare err=%s",
                      snd_strerror(ret));
            return MEDIA_ERR;
        }
        return MEDIA_OK;
    }

    if (error_code == -ESTRPIPE) {
        /*
         * suspend 恢复可能短暂返回 EAGAIN。有限重试避免永久占用调用线程，
         * resume 仍失败时再通过 prepare 重建 PCM 运行状态。
         */
        ret = -EAGAIN;
        while (ret == -EAGAIN && resume_attempts < 1000) {
            ret = snd_pcm_resume(pcm);
            if (ret == -EAGAIN) {
                nanosleep(&retry_interval, NULL);
            }
            resume_attempts += 1;
        }
        if (ret < 0) {
            ret = snd_pcm_prepare(pcm);
            if (ret < 0) {
                LOG_ERROR("audio_playback_recover failed: suspend prepare err=%s",
                          snd_strerror(ret));
                return MEDIA_ERR;
            }
        }
        return MEDIA_OK;
    }

    if (error_code == -EINTR || error_code == -EAGAIN) {
        /* 临时中断或暂无空间不改变 PCM 状态，由写入循环直接重试。 */
        return MEDIA_OK;
    }

    LOG_ERROR("audio_playback_recover failed: unrecoverable write error=%s(%d)",
              snd_strerror(error_code),
              error_code);
    return audio_playback_map_alsa_error(error_code);
}

/**
 * @description: 初始化 ALSA Playback 设备。
 */
MediaResult AudioPlayback::init(const AudioPlaybackConfig *config) {
    Impl *ctx = impl_.get();
    AudioPlaybackConfig normalized = {};
    AudioPlaybackNegotiatedParams negotiated = {};
    MediaResult result = MEDIA_OK;
    snd_pcm_t *pcm = NULL;
    snd_pcm_format_t alsa_format = SND_PCM_FORMAT_UNKNOWN;
    int bytes_per_sample = 0;
    int name_length = 0;
    int ret = 0;

    /* 先校验对象生命周期，避免覆盖仍在使用的 PCM 和运行统计。 */
    if (ctx == NULL) {
        LOG_ERROR("AudioPlayback init failed: implementation is NULL");
        return MEDIA_ERR_NO_MEMORY;
    }
    if (ctx->initialized || ctx->pcm_handle != NULL) {
        LOG_ERROR("AudioPlayback init failed: playback is already initialized");
        return MEDIA_ERR_BUSY;
    }
    ctx->reset();

    /* 准备有效配置，并把设备名复制到对象自有存储中。 */
    result = audio_playback_normalize_config(&normalized, config);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio_playback_init failed: normalize config result=%d", (int)result);
        return result;
    }

    name_length = snprintf(ctx->device_name,
                           sizeof(ctx->device_name),
                           "%s",
                           normalized.device_name);
    if (name_length < 0 || (size_t)name_length >= sizeof(ctx->device_name)) {
        LOG_ERROR("audio_playback_init failed: device name is too long");
        ctx->reset();
        return MEDIA_ERR_INVALID_CONFIG;
    }
    normalized.device_name = ctx->device_name;

    alsa_format = audio_playback_to_alsa_format(normalized.format);
    bytes_per_sample = audio_playback_bytes_per_sample(normalized.format);
    if (alsa_format == SND_PCM_FORMAT_UNKNOWN || bytes_per_sample <= 0) {
        LOG_ERROR("audio_playback_init failed: unsupported format=%d",
                  (int)normalized.format);
        ctx->reset();
        return MEDIA_ERR_UNSUPPORTED;
    }

    /*
     * 以阻塞模式打开 Playback PCM。当环形缓冲区暂无空间时，
     * snd_pcm_writei() 会等待硬件消费数据，不需要上层轮询。
     */
    ret = snd_pcm_open(&pcm, normalized.device_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        LOG_ERROR("audio_playback_init failed: open device=%s err=%s",
                  normalized.device_name,
                  snd_strerror(ret));
        ctx->reset();
        return audio_playback_map_alsa_error(ret);
    }
    ctx->pcm_handle = pcm;

    /*
     * 硬件参数辅助函数完成格式、采样率、period 和 buffer 协商；软件参数
     * 辅助函数再以协商结果设置应用层唤醒粒度与 PCM 启停阈值。
     */
    result = audio_playback_configure_hw_params(pcm, &normalized, alsa_format, &negotiated);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio_playback_init failed: configure hw_params result=%d", (int)result);
        goto init_failed;
    }
    normalized.sample_rate = (int)negotiated.sample_rate;
    normalized.period_frames = (int)negotiated.period_frames;
    normalized.buffer_periods =
        (int)(negotiated.buffer_frames / negotiated.period_frames);

    result = audio_playback_configure_sw_params(pcm, &negotiated);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio_playback_init failed: configure sw_params result=%d", (int)result);
        goto init_failed;
    }

    /* 显式 prepare，使 PCM 从 SETUP 进入 PREPARED，后续可直接写入。 */
    ret = snd_pcm_prepare(pcm);
    if (ret < 0) {
        LOG_ERROR("audio_playback_init failed: prepare err=%s", snd_strerror(ret));
        result = MEDIA_ERR;
        goto init_failed;
    }

    ctx->config = normalized;
    ctx->config.device_name = ctx->device_name;
    ctx->frame_bytes = (size_t)bytes_per_sample * (size_t)normalized.channels;
    ctx->buffer_frames = (uint64_t)negotiated.buffer_frames;
    ctx->start_threshold_frames = (uint64_t)negotiated.start_threshold_frames;
    ctx->initialized = 1;

    LOG_INFO("audio playback init success: device=%s rate=%d channels=%d format=%d "
             "period_frames=%d buffer_frames=%llu start_threshold=%llu",
             ctx->config.device_name,
             ctx->config.sample_rate,
             ctx->config.channels,
             (int)ctx->config.format,
             ctx->config.period_frames,
             (unsigned long long)ctx->buffer_frames,
             (unsigned long long)ctx->start_threshold_frames);
    return MEDIA_OK;

init_failed:
    releaseAfterInitFailure();
    LOG_ERROR("audio_playback_init failed: result=%d", (int)result);
    return result;
}

/**
 * @description: 将一批 PCM 帧完整写入 ALSA Playback 缓冲区。
 */
MediaResult AudioPlayback::write(const void *data, size_t frame_count) {
    Impl *ctx = impl_.get();
    const uint8_t *source = NULL;
    const uint8_t *current = NULL;
    snd_pcm_t *pcm = NULL;
    snd_pcm_sframes_t written = 0;
    size_t frames_written = 0;
    size_t frames_remaining = 0;
    MediaResult result = MEDIA_OK;

    if (ctx == NULL || data == NULL || frame_count == 0) {
        LOG_ERROR("AudioPlayback write failed: impl=%p data=%p frame_count=%zu",
                  (void *)ctx,
                  data,
                  frame_count);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!ctx->initialized || ctx->pcm_handle == NULL || ctx->frame_bytes == 0) {
        LOG_ERROR("audio_playback_write failed: playback is not ready initialized=%d pcm=%p "
                  "frame_bytes=%zu",
                  ctx->initialized,
                  ctx->pcm_handle,
                  ctx->frame_bytes);
        return MEDIA_ERR_NOT_READY;
    }
    if (frame_count > SIZE_MAX / ctx->frame_bytes) {
        LOG_ERROR("audio_playback_write failed: frame byte size overflow frame_count=%zu "
                  "frame_bytes=%zu",
                  frame_count,
                  ctx->frame_bytes);
        return MEDIA_ERR_INVALID_PARAM;
    }

    source = (const uint8_t *)data;
    pcm = (snd_pcm_t *)ctx->pcm_handle;
    frames_remaining = frame_count;

    /*
     * snd_pcm_writei() 可能只接收请求的一部分帧。每次成功后同时推进
     * 数据指针和剩余帧数，直到当前输入全部交付；错误恢复成功后
     * 不移动指针，重试尚未写入的同一段数据。
     */
    while (frames_remaining > 0) {
        current = source + frames_written * ctx->frame_bytes;
        written = snd_pcm_writei(pcm, current, (snd_pcm_uframes_t)frames_remaining);
        if (written > 0) {
            frames_written += (size_t)written;
            frames_remaining -= (size_t)written;
            __atomic_add_fetch(&ctx->stats.written_frames,
                               (uint64_t)written,
                               __ATOMIC_RELAXED);
            continue;
        }
        if (written == 0) {
            LOG_ERROR("audio_playback_write failed: ALSA write made no progress "
                      "frames_remaining=%zu",
                      frames_remaining);
            return MEDIA_ERR;
        }

        result = recover((int)written);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio_playback_write failed: recover write error=%s result=%d",
                      snd_strerror((int)written),
                      (int)result);
            return result;
        }
    }

    return MEDIA_OK;
}

/**
 * @description: 等待 ALSA 中已提交的 PCM 播放完毕。
 */
MediaResult AudioPlayback::drain() {
    Impl *ctx = impl_.get();
    snd_pcm_t *pcm = NULL;
    snd_pcm_state_t state_before_drain = SND_PCM_STATE_DISCONNECTED;
    const char *state_name = NULL;
    uint64_t xrun_count = 0;
    int ret = 0;

    if (ctx == NULL) {
        LOG_ERROR("AudioPlayback drain failed: implementation is NULL");
        return MEDIA_ERR_NO_MEMORY;
    }
    if (!ctx->initialized || ctx->pcm_handle == NULL) {
        LOG_ERROR("audio_playback_drain failed: playback is not ready initialized=%d pcm=%p",
                  ctx->initialized,
                  ctx->pcm_handle);
        return MEDIA_ERR_NOT_READY;
    }

    pcm = (snd_pcm_t *)ctx->pcm_handle;
    state_before_drain = snd_pcm_state(pcm);
    ret = snd_pcm_drain(pcm);
    if (ret < 0) {
        state_name = snd_pcm_state_name(state_before_drain);
        if (ret == -EPIPE) {
            xrun_count = __atomic_add_fetch(&ctx->stats.xrun_count, 1, __ATOMIC_RELAXED);
            LOG_WARN("audio playback xrun detected while draining: "
                     "state_before_drain=%s(%d) xrun_count=%llu",
                     state_name != NULL ? state_name : "UNKNOWN",
                     (int)state_before_drain,
                     (unsigned long long)xrun_count);
        }
        LOG_ERROR("audio_playback_drain failed: state_before_drain=%s(%d) err=%s",
                  state_name != NULL ? state_name : "UNKNOWN",
                  (int)state_before_drain,
                  snd_strerror(ret));
        return audio_playback_map_alsa_error(ret);
    }

    return MEDIA_OK;
}

/**
 * @description: 读取 audioPlayback 运行统计快照。
 */
MediaResult AudioPlayback::getConfig(AudioPlaybackConfig *config) const {
    const Impl *ctx = impl_.get();

    if (ctx == NULL || config == NULL) {
        LOG_ERROR("AudioPlayback getConfig failed: impl=%p config=%p",
                  (const void *)ctx,
                  (void *)config);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!ctx->initialized) {
        LOG_ERROR("AudioPlayback getConfig failed: playback is not initialized");
        return MEDIA_ERR_NOT_READY;
    }

    *config = ctx->config;
    return MEDIA_OK;
}

MediaResult AudioPlayback::getStats(AudioPlaybackStats *stats) const {
    const Impl *ctx = impl_.get();
    if (ctx == NULL || stats == NULL) {
        LOG_ERROR("audio_playback_get_stats failed: ctx=%p stats=%p",
                  (const void *)ctx,
                  (void *)stats);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!ctx->initialized) {
        LOG_ERROR("audio_playback_get_stats failed: playback is not initialized");
        return MEDIA_ERR_NOT_READY;
    }

    stats->written_frames = __atomic_load_n(&ctx->stats.written_frames, __ATOMIC_RELAXED);
    stats->xrun_count = __atomic_load_n(&ctx->stats.xrun_count, __ATOMIC_RELAXED);
    return MEDIA_OK;
}

/**
 * @description: 立即丢弃待播放数据并关闭 ALSA Playback 设备。
 */
MediaResult AudioPlayback::deinit() {
    Impl *ctx = impl_.get();
    snd_pcm_t *pcm = NULL;
    snd_pcm_state_t state = SND_PCM_STATE_DISCONNECTED;
    const char *state_name = NULL;
    MediaResult result = MEDIA_OK;
    int ret = 0;

    if (ctx == NULL) {
        LOG_ERROR("AudioPlayback deinit failed: implementation is NULL");
        return MEDIA_ERR_NO_MEMORY;
    }

    pcm = (snd_pcm_t *)ctx->pcm_handle;
    if (pcm != NULL) {
        state = snd_pcm_state(pcm);
        state_name = snd_pcm_state_name(state);
        if (state == SND_PCM_STATE_RUNNING ||
            state == SND_PCM_STATE_XRUN ||
            state == SND_PCM_STATE_SUSPENDED ||
            state == SND_PCM_STATE_PAUSED ||
            state == SND_PCM_STATE_DRAINING) {
            ret = snd_pcm_drop(pcm);
            if (ret < 0) {
                LOG_ERROR("audio_playback_deinit failed: drop state=%s(%d) err=%s",
                          state_name != NULL ? state_name : "UNKNOWN",
                          (int)state,
                          snd_strerror(ret));
                result = MEDIA_ERR;
            }
        }

        ret = snd_pcm_close(pcm);
        if (ret < 0) {
            LOG_ERROR("audio_playback_deinit failed: close err=%s", snd_strerror(ret));
            result = MEDIA_ERR;
        }
    }

    ctx->reset();
    return result;
}

} // namespace rkmedia
