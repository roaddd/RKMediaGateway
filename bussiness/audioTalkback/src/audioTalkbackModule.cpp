/**
 * @file audioTalkbackModule.cpp
 * @brief 进程内唯一 AudioTalkback 实例的纯 C 接口适配实现。
 */

#include "audioTalkbackModule.h"

#include "audioTalkback.h"
#include "logger.h"

#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

static std::mutex g_talkback_mutex; /* 串行保护唯一实例的创建、访问和销毁。 */
static std::unique_ptr<rkmedia::AudioTalkback> g_talkback; /* 进程内唯一双向语音实例。 */

/**
 * @description: 严格校验双向语音配置；不修改配置，也不填充任何缺省值。
 */
static MediaResult audio_talkback_validate_config(const AudioTalkbackConfig *config)
{
    if (config == NULL || config->playback_device == NULL ||
        config->playback_device[0] == '\0') {
        LOG_ERROR("audio talkback config invalid: config=%p playback_device=%p",
                  static_cast<const void *>(config),
                  config != NULL ? static_cast<const void *>(config->playback_device) : NULL);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (config->sample_rate != 48000 || config->decoder_channels != 1 ||
        config->playback_channels != 2 || config->frame_samples_per_channel != 960) {
        LOG_ERROR("audio talkback config invalid: rate=%d decoder_channels=%d playback_channels=%d frame_samples=%d",
                  config->sample_rate,
                  config->decoder_channels,
                  config->playback_channels,
                  config->frame_samples_per_channel);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (config->playback_buffer_periods < 2 || config->playback_start_periods <= 0 ||
        config->playback_start_periods > config->playback_buffer_periods ||
        config->jitter_prebuffer_packets < 2 ||
        config->jitter_max_packets < config->jitter_prebuffer_packets ||
        config->talker_timeout_ms < 100) {
        LOG_ERROR("audio talkback config invalid: buffer_periods=%d start_periods=%d prebuffer=%d max_packets=%d timeout_ms=%d",
                  config->playback_buffer_periods,
                  config->playback_start_periods,
                  config->jitter_prebuffer_packets,
                  config->jitter_max_packets,
                  config->talker_timeout_ms);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (config->mixer.enabled &&
        (config->mixer.card_name == NULL || config->mixer.control_name == NULL ||
         config->mixer.value_name == NULL || config->mixer.card_name[0] == '\0' ||
         config->mixer.control_name[0] == '\0' || config->mixer.value_name[0] == '\0')) {
        LOG_ERROR("audio talkback mixer config invalid: enabled=%d card=%p control=%p value=%p",
                  config->mixer.enabled,
                  static_cast<const void *>(config->mixer.card_name),
                  static_cast<const void *>(config->mixer.control_name),
                  static_cast<const void *>(config->mixer.value_name));
        return MEDIA_ERR_INVALID_CONFIG;
    }
    return MEDIA_OK;
}

/**
 * @description: 创建并启动进程内唯一双向语音实例。
 *
 * 全局锁覆盖创建和 start()，保证并发初始化只能有一个成功；start() 失败时局部
 * unique_ptr 会自动销毁半初始化实例，不会把不可用对象发布到全局状态。
 */
MediaResult audio_talkback_init(const AudioTalkbackConfig *config)
{
    std::unique_ptr<rkmedia::AudioTalkback> instance;
    std::unique_lock<std::mutex> lock(g_talkback_mutex);
    MediaResult result = MEDIA_OK;

    result = audio_talkback_validate_config(config);
    if (result != MEDIA_OK) return result;
    if (g_talkback) {
        LOG_ERROR("audio talkback init failed: singleton is already initialized");
        return MEDIA_ERR_BUSY;
    }

    try {
        instance.reset(new rkmedia::AudioTalkback(*config));
    } catch (const std::bad_alloc &error) {
        LOG_ERROR("audio talkback init failed: construct instance exception=%s", error.what());
        return MEDIA_ERR_NO_MEMORY;
    } catch (const std::exception &error) {
        LOG_ERROR("audio talkback init failed: construct instance exception=%s", error.what());
        return MEDIA_ERR;
    }
    result = instance->start();
    if (result != MEDIA_OK) {
        LOG_ERROR("audio talkback init failed: start instance result=%d", result);
        return result;
    }

    g_talkback = std::move(instance);
    return MEDIA_OK;
}

/**
 * @description: 在单例生命周期锁保护下提交入站 Opus 包。
 *
 * AudioTalkback::submit() 只复制负载并插入抖动缓冲，持有全局锁的时间很短；
 * 该锁保证 deinit() 无法在提交过程中销毁实例。
 */
MediaResult audio_talkback_submit(const AudioTalkbackPacket *packet)
{
    std::unique_lock<std::mutex> lock(g_talkback_mutex);

    if (packet == NULL) {
        LOG_ERROR("audio talkback submit failed: packet is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!g_talkback) {
        LOG_ERROR("audio talkback submit failed: singleton is not initialized");
        return MEDIA_ERR_NOT_READY;
    }
    return g_talkback->submit(*packet);
}

/** @description: 在单例生命周期锁保护下读取累计统计快照。 */
MediaResult audio_talkback_get_stats(AudioTalkbackStats *stats)
{
    std::unique_lock<std::mutex> lock(g_talkback_mutex);

    if (stats == NULL) {
        LOG_ERROR("audio talkback get stats failed: stats is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!g_talkback) {
        return MEDIA_ERR_NOT_READY;
    }
    return g_talkback->getStats(stats);
}

/**
 * @description: 停止并销毁进程内唯一双向语音实例。
 *
 * 全局锁阻止新的 submit/get_stats 进入；stop() 等待 worker 退出后再清空指针，
 * 因而函数返回时不存在仍引用该实例的外部调用。
 */
MediaResult audio_talkback_deinit(void)
{
    std::unique_lock<std::mutex> lock(g_talkback_mutex);
    MediaResult result = MEDIA_OK;

    if (!g_talkback) return MEDIA_OK;
    result = g_talkback->stop();
    if (result != MEDIA_OK) {
        LOG_ERROR("audio talkback deinit: stop instance failed result=%d", result);
    }
    g_talkback.reset();
    return result;
}
