/**
 * @file audioPlayback.h
 * @brief ALSA PCM 音频播放类对外接口。
 */

#ifndef __AUDIO_PLAYBACK_H__
#define __AUDIO_PLAYBACK_H__

#include "commonDef.h"

#include <cstddef>
#include <cstdint>
#include <memory>

#define AUDIO_PLAYBACK_DEFAULT_DEVICE "hw:0,0"
#define AUDIO_PLAYBACK_DEFAULT_SAMPLE_RATE 48000
#define AUDIO_PLAYBACK_DEFAULT_CHANNELS 2
#define AUDIO_PLAYBACK_DEFAULT_PERIOD_FRAMES 960
#define AUDIO_PLAYBACK_DEFAULT_BUFFER_PERIODS 4
#define AUDIO_PLAYBACK_DEFAULT_START_THRESHOLD_PERIODS 2

namespace rkmedia {

/** @brief AudioPlayback 当前支持的 PCM 样本格式。 */
enum AudioPlaybackSampleFormat {
    AUDIO_PLAYBACK_SAMPLE_FORMAT_INVALID = 0, /* 无效格式，传入时将使用默认格式。 */
    AUDIO_PLAYBACK_SAMPLE_FORMAT_S16_LE = 1  /* 16-bit 有符号小端 PCM。 */
};

/** @brief ALSA Playback 初始化配置。 */
struct AudioPlaybackConfig {
    const char *device_name;          /* ALSA PCM 设备名，例如 hw:0,0 或 default。 */
    int sample_rate;                  /* 每秒每声道的采样数，对讲默认为 48000 Hz。 */
    int channels;                     /* 写入 ALSA 硬件的声道数，RK809 数字侧使用双声道。 */
    AudioPlaybackSampleFormat format; /* PCM 样本格式，当前只支持 S16_LE。 */
    int period_frames;                /* 期望的单个 ALSA period 帧数，48000 Hz 下 960 帧为 20 ms。 */
    int buffer_periods;               /* 期望的 ALSA 环形缓冲区 period 数，默认为 4。 */
    int start_threshold_periods;      /* 启动播放前需要积累的 period 数，默认为 2。 */
};

/** @brief AudioPlayback 运行统计。 */
struct AudioPlaybackStats {
    uint64_t written_frames; /* 已成功交付给 ALSA 的 PCM 帧总数。 */
    uint64_t xrun_count;     /* 累计检测并恢复的 Playback underrun 次数。 */
};

/**
 * @description: ALSA PCM 播放类。
 *
 * 该类将 ALSA 参数协商、部分写入、XRUN/suspend 恢复和资源生命周期
 * 隐藏在私有实现中。构造函数不打开设备，调用者必须通过 init() 获取
 * 可判断的 MediaResult；同一对象重新初始化前必须先调用 deinit()。
 */
class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    /**
     * @description: 打开 ALSA Playback 设备并配置硬件、软件参数。
     * @param config 期望配置，为 NULL 时使用默认值。
     */
    MediaResult init(const AudioPlaybackConfig *config);

    /**
     * @description: 阻塞写入指定数量的交错 PCM 帧，内部补齐部分写入并恢复 XRUN。
     * @param data PCM 数据首地址，格式和声道数必须与初始化配置一致。
     * @param frame_count PCM 帧数，不是样本数或字节数。
     */
    MediaResult write(const void *data, size_t frame_count);

    /** @description: 等待已提交的 PCM 全部播放完毕。 */
    MediaResult drain();

    /**
     * @description: 返回 ALSA 协商后的实际配置。
     * @param config 输出配置，其 device_name 在本对象 deinit() 前有效。
     */
    MediaResult getConfig(AudioPlaybackConfig *config) const;

    /** @description: 获取播放帧数和 XRUN 运行统计快照。 */
    MediaResult getStats(AudioPlaybackStats *stats) const;

    /** @description: 立即停止播放并释放 ALSA 资源，尚未播放数据会被丢弃。 */
    MediaResult deinit();

private:
    class Impl;
    std::unique_ptr<Impl> impl_; /* 私有实现，用于隐藏 ALSA 类型和运行状态。 */

    /** @description: 释放初始化过程中已获取的资源并重置状态。 */
    void releaseAfterInitFailure();

    /** @description: 从 Playback underrun 或设备 suspend 状态恢复。 */
    MediaResult recover(int error_code);

    AudioPlayback(const AudioPlayback &) = delete;
    AudioPlayback &operator=(const AudioPlayback &) = delete;
};

} // namespace rkmedia

#endif /* __AUDIO_PLAYBACK_H__ */
