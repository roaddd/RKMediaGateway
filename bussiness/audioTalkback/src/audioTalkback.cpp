#include "audioTalkback.h"

#include "audioDecoder.h"
#include "audioPlayback.h"
#include "logger.h"

#include <alsa/asoundlib.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rkmedia {

namespace {

/** @description: 返回 steady_clock 对应的微秒时间，供说话人超时判断使用。 */
static uint64_t talkbackNowUs()
{
    std::chrono::steady_clock::time_point now = {};
    uint64_t result = 0;

    now = std::chrono::steady_clock::now();
    result = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    return result;
}

/**
 * @description: 将 16 位 RTP 序号扩展到当前序号周期，正确处理 65535 到 0 的回绕。
 */
static uint32_t extendRtpSequence(uint16_t sequence, uint32_t reference)
{
    int64_t candidate = 0;
    int64_t distance = 0;

    candidate = static_cast<int64_t>(reference & 0xffff0000U) |
                static_cast<int64_t>(sequence);
    distance = candidate - static_cast<int64_t>(reference);
    if (distance > 32767) {
        candidate -= 65536;
    } else if (distance < -32768) {
        candidate += 65536;
    }
    if (candidate < 0) {
        candidate = 0;
    }
    return static_cast<uint32_t>(candidate);
}

/**
 * @description: 设置并回读 ALSA 枚举型 Playback 路由，确保 PCM 打开前硬件输出路径已经生效。
 */
static MediaResult configurePlaybackMixer(const AudioTalkbackMixerConfig &config)
{
    snd_mixer_t *mixer = NULL;
    snd_mixer_elem_t *element = NULL;
    snd_mixer_selem_id_t *elementId = NULL;
    const char *operation = "open";
    char itemName[128] = {0};
    int itemCount = 0;
    int itemIndex = 0;
    unsigned int targetIndex = 0;
    unsigned int actualIndex = 0;
    int targetFound = 0;
    int ret = 0;
    int closeRet = 0;
    MediaResult result = MEDIA_ERR;

    if (!config.enabled) {
        return MEDIA_OK;
    }
    if (config.card_name == NULL || config.control_name == NULL || config.value_name == NULL ||
        config.card_name[0] == '\0' || config.control_name[0] == '\0' ||
        config.value_name[0] == '\0') {
        LOG_ERROR("audio talkback mixer config invalid: card=%p control=%p value=%p",
                  static_cast<const void *>(config.card_name),
                  static_cast<const void *>(config.control_name),
                  static_cast<const void *>(config.value_name));
        return MEDIA_ERR_INVALID_CONFIG;
    }

    /* Mixer 句柄只在启动阶段使用；播放线程后续只访问 PCM，不持有控制设备。 */
    operation = "open";
    ret = snd_mixer_open(&mixer, 0);
    if (ret < 0) goto alsa_failed;
    operation = "attach";
    ret = snd_mixer_attach(mixer, config.card_name);
    if (ret < 0) goto alsa_failed;
    operation = "register simple elements";
    ret = snd_mixer_selem_register(mixer, NULL, NULL);
    if (ret < 0) goto alsa_failed;
    operation = "load controls";
    ret = snd_mixer_load(mixer);
    if (ret < 0) goto alsa_failed;

    snd_mixer_selem_id_alloca(&elementId);
    snd_mixer_selem_id_set_index(elementId, 0);
    snd_mixer_selem_id_set_name(elementId, config.control_name);
    element = snd_mixer_find_selem(mixer, elementId);
    if (element == NULL) {
        LOG_ERROR("audio talkback mixer control not found: card=%s control=%s",
                  config.card_name,
                  config.control_name);
        goto cleanup;
    }
    if (!snd_mixer_selem_is_enumerated(element)) {
        LOG_ERROR("audio talkback mixer control is not enumerated: card=%s control=%s",
                  config.card_name,
                  config.control_name);
        goto cleanup;
    }

    /* 按名称查找枚举项，避免依赖 BSP 中可能变化的枚举数字。 */
    itemCount = snd_mixer_selem_get_enum_items(element);
    for (itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
        std::memset(itemName, 0, sizeof(itemName));
        operation = "get enum item name";
        ret = snd_mixer_selem_get_enum_item_name(element,
                                                 static_cast<unsigned int>(itemIndex),
                                                 sizeof(itemName),
                                                 itemName);
        if (ret < 0) goto alsa_failed;
        if (std::strcmp(itemName, config.value_name) == 0) {
            targetIndex = static_cast<unsigned int>(itemIndex);
            targetFound = 1;
            break;
        }
    }
    if (!targetFound) {
        LOG_ERROR("audio talkback mixer value not found: card=%s control=%s value=%s items=%d",
                  config.card_name,
                  config.control_name,
                  config.value_name,
                  itemCount);
        goto cleanup;
    }

    operation = "set enum item";
    ret = snd_mixer_selem_set_enum_item(element, SND_MIXER_SCHN_FRONT_LEFT, targetIndex);
    if (ret < 0) goto alsa_failed;
    operation = "read back enum item";
    ret = snd_mixer_selem_get_enum_item(element, SND_MIXER_SCHN_FRONT_LEFT, &actualIndex);
    if (ret < 0) goto alsa_failed;
    if (actualIndex != targetIndex) {
        LOG_ERROR("audio talkback mixer readback mismatch: expected=%u actual=%u",
                  targetIndex,
                  actualIndex);
        goto cleanup;
    }

    LOG_INFO("audio talkback mixer configured: card=%s control=%s value=%s",
             config.card_name,
             config.control_name,
             config.value_name);
    result = MEDIA_OK;
    goto cleanup;

alsa_failed:
    LOG_ERROR("audio talkback mixer operation failed: operation=%s err=%s",
              operation,
              snd_strerror(ret));

cleanup:
    if (mixer != NULL) {
        closeRet = snd_mixer_close(mixer);
        if (closeRet < 0) {
            LOG_ERROR("audio talkback mixer close failed: err=%s", snd_strerror(closeRet));
            if (result == MEDIA_OK) result = MEDIA_ERR;
        }
    }
    return result;
}

} // namespace

/** @description: 深拷贝配置字符串，并建立尚未启动的运行状态。 */
AudioTalkback::AudioTalkback(const AudioTalkbackConfig &config)
    : config_(config),
      playbackDevice_(config.playback_device),
      mixerCard_(config.mixer.card_name != NULL ? config.mixer.card_name : ""),
      mixerControl_(config.mixer.control_name != NULL ? config.mixer.control_name : ""),
      mixerValue_(config.mixer.value_name != NULL ? config.mixer.value_name : "")
{
    config_.playback_device = playbackDevice_.c_str();
    config_.mixer.card_name = mixerCard_.c_str();
    config_.mixer.control_name = mixerControl_.c_str();
    config_.mixer.value_name = mixerValue_.c_str();
    std::memset(&stats_, 0, sizeof(stats_));
}

/** @description: 确保 worker 和底层媒体资源在对象销毁前全部停止。 */
AudioTalkback::~AudioTalkback()
{
    stop();
}

/** @description: 初始化输出链路并启动唯一的解码/播放线程。 */
MediaResult AudioTalkback::start()
{
    AudioDecoderConfig decoderConfig = {};
    AudioPlaybackConfig playbackConfig = {};
    MediaResult result = MEDIA_OK;
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

    /* 已处于运行状态时拒绝重复启动，防止重复创建解码器、PCM 设备和工作线程。 */
    lock.lock();
    if (running_) {
        LOG_ERROR("audio talkback start failed: module is already running");
        return MEDIA_ERR_BUSY;
    }
    lock.unlock();

    /* 打开 PCM 前先选择耳机或扬声器等硬件播放路由。 */
    result = configurePlaybackMixer(config_.mixer);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio talkback start failed: configure playback mixer result=%d", result);
        return result;
    }

    /* 创建有状态的 Opus 解码器，后续普通解码、FEC 和 PLC 共用该实例。 */
    decoderConfig.codec = MEDIA_CODEC_OPUS;
    decoderConfig.sample_rate = config_.sample_rate;
    decoderConfig.output_channels = config_.decoder_channels;
    decoderConfig.max_frame_samples_per_channel = config_.frame_samples_per_channel;
    result = audio_decoder_create(&decoderConfig, &decoder_);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio talkback start failed: create decoder result=%d", result);
        return result;
    }

    /*
     * Playback 的一个 period 与一帧 Opus 解码结果对齐，使 worker 每次写入
     * 一个完整音频帧；硬件缓冲周期数和启动阈值由双向语音配置显式控制。
     */
    playbackConfig.device_name = config_.playback_device;
    playbackConfig.sample_rate = config_.sample_rate;
    playbackConfig.channels = config_.playback_channels;
    playbackConfig.format = AUDIO_PLAYBACK_SAMPLE_FORMAT_S16_LE;
    playbackConfig.period_frames = config_.frame_samples_per_channel;
    playbackConfig.buffer_periods = config_.playback_buffer_periods;
    playbackConfig.start_threshold_periods = config_.playback_start_periods;
    result = playback_.init(&playbackConfig);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio talkback start failed: init playback result=%d", result);
        audio_decoder_destroy(decoder_);
        decoder_ = NULL;
        return result;
    }

    /* 预分配声道适配缓存，避免实时播放过程中反复申请内存。 */
    try {
        stereoBuffer_.resize(static_cast<size_t>(config_.frame_samples_per_channel) *
                             static_cast<size_t>(config_.playback_channels));
    } catch (const std::bad_alloc &error) {
        LOG_ERROR("audio talkback start failed: allocate PCM adapter buffer exception=%s",
                  error.what());
        playback_.deinit();
        audio_decoder_destroy(decoder_);
        decoder_ = NULL;
        return MEDIA_ERR_NO_MEMORY;
    }

    /*
     * 创建线程前先发布 running_，确保 worker 启动后不会立即退出；若线程创建
     * 失败，则撤销运行状态并按初始化的相反顺序释放 Playback 和解码器。
     */
    lock.lock();
    running_ = true;
    resetPending_ = false;
    lock.unlock();
    try {
        worker_ = std::thread(&AudioTalkback::workerMain, this);
    } catch (const std::exception &error) {
        LOG_ERROR("audio talkback start failed: create worker exception=%s", error.what());
        lock.lock();
        running_ = false;
        lock.unlock();
        playback_.deinit();
        audio_decoder_destroy(decoder_);
        decoder_ = NULL;
        return MEDIA_ERR;
    }

    LOG_INFO("audio talkback started: device=%s rate=%d decoder_channels=%d playback_channels=%d frame_samples=%d prebuffer=%d max_packets=%d",
             config_.playback_device,
             config_.sample_rate,
             config_.decoder_channels,
             config_.playback_channels,
             config_.frame_samples_per_channel,
             config_.jitter_prebuffer_packets,
             config_.jitter_max_packets);
    return MEDIA_OK;
}

    /** @description: 执行说话人仲裁、RTP 序号扩展并把包插入有序抖动缓冲。 */
MediaResult AudioTalkback::submit(const AudioTalkbackPacket &packet)
{
        BufferedTalkbackPacket bufferedPacket = {};
        std::map<uint32_t, BufferedTalkbackPacket>::iterator inserted = {};
        std::map<uint32_t, BufferedTalkbackPacket>::iterator dropIt = {};
        uint64_t nowUs = 0;
        uint32_t extendedSequence = 0;
        bool insertedOk = false;
        bool droppedSubmittedPacket = false;
        std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

        if (packet.codec != MEDIA_CODEC_OPUS || packet.payload == NULL || packet.payload_size == 0) {
            LOG_ERROR("audio talkback submit failed: codec=%d payload=%p size=%zu",
                      packet.codec,
                      static_cast<const void *>(packet.payload),
                      packet.payload_size);
            return MEDIA_ERR_INVALID_PARAM;
        }

        nowUs = packet.arrival_time_us != 0 ? packet.arrival_time_us : talkbackNowUs();
        lock.lock();
        if (!running_) {
            LOG_ERROR("audio talkback submit failed: module is not running");
            return MEDIA_ERR_NOT_READY;
        }

        /* 同一时刻只播放一个浏览器；活动方超时后，新会话才可取得解码器所有权。 */
        if (hasActiveTalker_ && packet.session_id != activeSessionId_) {
            if (nowUs - lastArrivalTimeUs_ < static_cast<uint64_t>(config_.talker_timeout_ms) * 1000ULL) {
                ++stats_.foreign_talker_drops;
                return MEDIA_ERR_BUSY;
            }
            resetStreamLocked(packet.session_id, packet.ssrc, "talker switched");
        } else if (!hasActiveTalker_) {
            resetStreamLocked(packet.session_id, packet.ssrc, "first talker");
        } else if (packet.ssrc != activeSsrc_) {
            resetStreamLocked(packet.session_id, packet.ssrc, "ssrc changed");
        }

        if (!hasHighestSequence_) {
            extendedSequence = packet.sequence_number;
            highestExtendedSequence_ = extendedSequence;
            hasHighestSequence_ = true;
        } else {
            extendedSequence = extendRtpSequence(packet.sequence_number, highestExtendedSequence_);
            if (extendedSequence > highestExtendedSequence_) highestExtendedSequence_ = extendedSequence;
        }
        /* 序号向前跳过整个缓冲窗口时视为发送端重启，避免连续数百次 PLC 追赶旧时序。 */
        if (playoutStarted_ &&
            extendedSequence > expectedExtendedSequence_ +
                                   static_cast<uint32_t>(config_.jitter_max_packets)) {
            resetStreamLocked(packet.session_id, packet.ssrc, "sequence discontinuity");
            extendedSequence = packet.sequence_number;
            highestExtendedSequence_ = extendedSequence;
            hasHighestSequence_ = true;
        }
        if (playoutStarted_ && extendedSequence < expectedExtendedSequence_) {
            ++stats_.late_packets;
            return MEDIA_OK;
        }
        if (jitterBuffer_.find(extendedSequence) != jitterBuffer_.end()) {
            ++stats_.duplicate_packets;
            return MEDIA_OK;
        }

        bufferedPacket.extendedSequence = extendedSequence;
        bufferedPacket.rtpTimestamp = packet.rtp_timestamp;
        bufferedPacket.arrivalTimeUs = nowUs;
        try {
            bufferedPacket.payload.assign(packet.payload, packet.payload + packet.payload_size);
            inserted = jitterBuffer_.emplace(extendedSequence, std::move(bufferedPacket)).first;
        } catch (const std::bad_alloc &error) {
            LOG_ERROR("audio talkback submit failed: copy packet size=%zu exception=%s",
                      packet.payload_size,
                      error.what());
            return MEDIA_ERR_NO_MEMORY;
        }
        insertedOk = true;
        lastArrivalTimeUs_ = nowUs;
        ++stats_.submitted_packets;

        /* 满载时保留最靠近播放点的低序号包，优先丢弃最远的未来包以控制时延。 */
        if (jitterBuffer_.size() > static_cast<size_t>(config_.jitter_max_packets)) {
            dropIt = std::prev(jitterBuffer_.end());
            droppedSubmittedPacket = dropIt == inserted;
            jitterBuffer_.erase(dropIt);
            ++stats_.overflow_drops;
        }
        condition_.notify_one();
        if (insertedOk && droppedSubmittedPacket) {
            return MEDIA_ERR_FULL;
        }
    return MEDIA_OK;
}

    /** @description: 请求线程退出，等待其完成后按依赖逆序释放播放和解码资源。 */
MediaResult AudioTalkback::stop()
{
        MediaResult playbackResult = MEDIA_OK;
        std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
        AudioTalkbackStats finalStats = {};

        lock.lock();
        if (!running_ && decoder_ == NULL) return MEDIA_OK;
        running_ = false;
        condition_.notify_all();
        lock.unlock();
        if (worker_.joinable()) worker_.join();
        playbackResult = playback_.deinit();
        if (playbackResult != MEDIA_OK) {
            LOG_ERROR("audio talkback stop: playback deinit failed result=%d", playbackResult);
        }
        if (decoder_ != NULL) {
            audio_decoder_destroy(decoder_);
            decoder_ = NULL;
        }
        lock.lock();
        jitterBuffer_.clear();
        clearStreamStateLocked();
        finalStats = stats_;
        lock.unlock();
        LOG_INFO("audio talkback stopped: submitted=%llu decoded=%llu fec=%llu plc=%llu duplicate=%llu late=%llu overflow=%llu foreign=%llu decode_errors=%llu playback_errors=%llu resets=%llu",
                 static_cast<unsigned long long>(finalStats.submitted_packets),
                 static_cast<unsigned long long>(finalStats.decoded_packets),
                 static_cast<unsigned long long>(finalStats.fec_recovered_frames),
                 static_cast<unsigned long long>(finalStats.plc_concealed_frames),
                 static_cast<unsigned long long>(finalStats.duplicate_packets),
                 static_cast<unsigned long long>(finalStats.late_packets),
                 static_cast<unsigned long long>(finalStats.overflow_drops),
                 static_cast<unsigned long long>(finalStats.foreign_talker_drops),
                 static_cast<unsigned long long>(finalStats.decode_errors),
                 static_cast<unsigned long long>(finalStats.playback_errors),
                 static_cast<unsigned long long>(finalStats.stream_resets));
    return playbackResult;
}

    /** @description: 在互斥锁保护下复制累计统计，避免读取到半更新字段。 */
MediaResult AudioTalkback::getStats(AudioTalkbackStats *stats) const
{
        std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

        if (stats == NULL) {
            LOG_ERROR("audio talkback get stats failed: stats is NULL");
            return MEDIA_ERR_INVALID_PARAM;
        }
        lock.lock();
        *stats = stats_;
    return MEDIA_OK;
}

    /** @description: 记录新的活动流，并通知 worker 在串行上下文中重置 Opus 状态。 */
void AudioTalkback::resetStreamLocked(int sessionId, uint32_t ssrc, const char *reason)
{
        bool wasActive = false;

        wasActive = hasActiveTalker_;
        jitterBuffer_.clear();
        playoutStarted_ = false;
        hasHighestSequence_ = false;
        expectedExtendedSequence_ = 0;
        highestExtendedSequence_ = 0;
        activeSessionId_ = sessionId;
        activeSsrc_ = ssrc;
        hasActiveTalker_ = true;
        resetPending_ = true;
        if (wasActive) ++stats_.stream_resets;
        LOG_INFO("audio talkback stream selected: session=%d ssrc=%u reason=%s",
                 sessionId,
                 static_cast<unsigned int>(ssrc),
                 reason != NULL ? reason : "unknown");
}

    /** @description: 清理仅与当前 RTP 流有关的状态；调用方必须持有 mutex_。 */
void AudioTalkback::clearStreamStateLocked()
{
        hasActiveTalker_ = false;
        activeSessionId_ = 0;
        activeSsrc_ = 0;
        lastArrivalTimeUs_ = 0;
        hasHighestSequence_ = false;
        highestExtendedSequence_ = 0;
        playoutStarted_ = false;
        expectedExtendedSequence_ = 0;
        nextPtsUs_ = 0;
        resetPending_ = false;
}

    /** @description: 校验解码输出并按硬件声道数完成 PCM 适配后阻塞写入 ALSA。 */
MediaResult AudioTalkback::playDecodedFrame(const AudioDecoderOutput &output)
{
        const int16_t *playbackData = NULL;
        size_t sampleIndex = 0;
        MediaResult result = MEDIA_OK;
        std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

        if (output.data == NULL || output.sample_rate != config_.sample_rate ||
            output.channels != config_.decoder_channels ||
            output.samples_per_channel != config_.frame_samples_per_channel) {
            LOG_ERROR("audio talkback decoded frame invalid: data=%p rate=%d channels=%d samples=%d",
                      static_cast<const void *>(output.data),
                      output.sample_rate,
                      output.channels,
                      output.samples_per_channel);
            lock.lock();
            ++stats_.decode_errors;
            lock.unlock();
            return MEDIA_ERR;
        }

        playbackData = output.data;
        if (config_.decoder_channels == 1 && config_.playback_channels == 2) {
            if (stereoBuffer_.size() < static_cast<size_t>(output.samples_per_channel) * 2U) {
                LOG_ERROR("audio talkback PCM adapter buffer too small: capacity=%zu required=%zu",
                          stereoBuffer_.size(),
                          static_cast<size_t>(output.samples_per_channel) * 2U);
                lock.lock();
                ++stats_.decode_errors;
                lock.unlock();
                return MEDIA_ERR;
            }
            /* RK809 数字侧要求双声道：把浏览器单声道样本复制到左右两个 I2S 时隙。 */
            for (sampleIndex = 0;
                 sampleIndex < static_cast<size_t>(output.samples_per_channel);
                 ++sampleIndex) {
                stereoBuffer_[sampleIndex * 2U] = output.data[sampleIndex];
                stereoBuffer_[sampleIndex * 2U + 1U] = output.data[sampleIndex];
            }
            playbackData = stereoBuffer_.data();
        }

        result = playback_.write(playbackData, static_cast<size_t>(output.samples_per_channel));
        if (result != MEDIA_OK) {
            LOG_ERROR("audio talkback playback write failed: frames=%d result=%d",
                      output.samples_per_channel,
                      result);
            lock.lock();
            ++stats_.playback_errors;
            lock.unlock();
        }
    return result;
}

    /** @description: 正常解码当前序号对应的 Opus 包并立即播放。 */
MediaResult AudioTalkback::decodePacket(const BufferedTalkbackPacket &packet, uint64_t ptsUs)
{
        AudioDecoderInput input = {};
        AudioDecoderOutput output = {};
        MediaResult result = MEDIA_OK;

        input.data = packet.payload.data();
        input.size = packet.payload.size();
        input.pts_us = ptsUs;
        /* 解码一帧Opus包 */
        result = audio_decoder_decode(decoder_, &input, &output);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio talkback decode failed: sequence=%u size=%zu result=%d",
                      packet.extendedSequence,
                      packet.payload.size(),
                      result);
            recordDecodeError();
            return result;
        }
    /* 播放一帧PCM */
    return playDecodedFrame(output);
}

    /** @description: 用下一包尝试带内 FEC；不存在冗余时 AudioDecoder 自动回退 PLC。 */
MediaResult AudioTalkback::recoverLostFrame(const BufferedTalkbackPacket *followingPacket, uint64_t ptsUs)
{
        AudioDecoderLossInput input = {};
        AudioDecoderOutput output = {};
        AudioDecoderStats beforeStats = {};
        AudioDecoderStats afterStats = {};
        MediaResult result = MEDIA_OK;
        std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

        input.following_packet_data = followingPacket != NULL ? followingPacket->payload.data() : NULL;
        input.following_packet_size = followingPacket != NULL ? followingPacket->payload.size() : 0;
        input.lost_samples_per_channel = config_.frame_samples_per_channel;
        input.pts_us = ptsUs;
        result = audio_decoder_get_stats(decoder_, &beforeStats);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio talkback recover failed: get decoder stats before result=%d", result);
            recordDecodeError();
            return result;
        }
        result = audio_decoder_recover_loss(decoder_, &input, &output);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio talkback recover failed: following_size=%zu result=%d",
                      input.following_packet_size,
                      result);
            recordDecodeError();
            return result;
        }
        result = audio_decoder_get_stats(decoder_, &afterStats);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio talkback recover failed: get decoder stats after result=%d", result);
            recordDecodeError();
            return result;
        }
        result = playDecodedFrame(output);
        if (result != MEDIA_OK) return result;

        lock.lock();
        if (afterStats.inband_recovered_frames > beforeStats.inband_recovered_frames) {
            ++stats_.fec_recovered_frames;
        } else {
            ++stats_.plc_concealed_frames;
        }
    return MEDIA_OK;
}

    /** @description: 在线程安全前提下累计一次解码或恢复错误。 */
void AudioTalkback::recordDecodeError()
{
        std::unique_lock<std::mutex> lock(mutex_);

        ++stats_.decode_errors;
}

    /**
     * @description: 串行执行抖动缓冲出队、丢包恢复、Opus 解码和 ALSA 播放。
     *
     * 启动时先积累 jitter_prebuffer_packets；之后每 20 ms 消费一个 RTP 序号。
     * 期望包缺失但下一包已到时用下一包尝试 FEC，下一时刻仍会正常解码该下一包；
     * 下一包也未到时立即使用 PLC，避免网络抖动阻塞硬件播放时钟。
     */
void AudioTalkback::workerMain()
{
    BufferedTalkbackPacket currentPacket = {};
    BufferedTalkbackPacket followingPacket = {};
    std::map<uint32_t, BufferedTalkbackPacket>::iterator packetIt = {};
    std::map<uint32_t, BufferedTalkbackPacket>::iterator followingIt = {};
    std::chrono::steady_clock::time_point deadline = {};
    std::chrono::microseconds frameDuration(0);
    uint64_t currentPtsUs = 0;
    uint64_t nowUs = 0;
    uint32_t expectedSequence = 0;
    bool haveCurrentPacket = false;
    bool haveFollowingPacket = false;
    MediaResult result = MEDIA_OK;
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

    /* 根据每帧采样数和采样率生成播放节拍，例如 960 / 48000 对应 20 ms。 */
    frameDuration = std::chrono::microseconds(
        static_cast<int64_t>(config_.frame_samples_per_channel) * 1000000LL /
        static_cast<int64_t>(config_.sample_rate));
    lock.lock();
    while (running_) {
        if (resetPending_) {
            /*
             * 切换会话或 SSRC 后必须清除 Opus 的历史解码状态。解码器始终
             * 由 worker 串行访问，因此先消费复位标志，再释放模块锁执行重置，
             * 避免重置过程阻塞网络接收线程提交新包。
             */
            resetPending_ = false;
            lock.unlock();
            result = audio_decoder_reset(decoder_);
            if (result != MEDIA_OK) {
                LOG_ERROR("audio talkback worker reset decoder failed: result=%d", result);
            }
            lock.lock();
            if (!running_) break;
        }

        if (!playoutStarted_) {
            /*
             * 新流先积累指定数量的数据包，用初始缓存抵消网络到达抖动；停止
             * 或再次切流也会唤醒这里，使 worker 不会永久阻塞。
             */
            condition_.wait(lock, [this]() {
                return !running_ || resetPending_ ||
                       jitterBuffer_.size() >= static_cast<size_t>(config_.jitter_prebuffer_packets);
            });
            if (!running_) break;
            if (resetPending_) continue;

            /* 以当前最小扩展序号作为播放起点，同时建立连续 PTS 和本地播放时钟。 */
            expectedExtendedSequence_ = jitterBuffer_.begin()->first;
            nextPtsUs_ = jitterBuffer_.begin()->second.arrivalTimeUs;
            playoutStarted_ = true;
            deadline = std::chrono::steady_clock::now();
            LOG_INFO("audio talkback playout started: session=%d ssrc=%u first_sequence=%u buffered=%zu",
                     activeSessionId_,
                     static_cast<unsigned int>(activeSsrc_),
                     expectedExtendedSequence_,
                     jitterBuffer_.size());
        }

        /*
         * 到达固定播放时刻才消费一个 RTP 序号。新包到达只更新抖动缓冲，
         * 不会提前唤醒播放；停止和流重置可以中断定时等待。
         */
        condition_.wait_until(lock, deadline, [this]() {
            return !running_ || resetPending_;
        });
        if (!running_) break;
        if (resetPending_) continue;

        /*
         * 缓冲已空且长时间没有新包时结束当前说话人，清除流时序并重置
         * 解码器；后续新包将重新经历说话人选择和预缓冲过程。
         */
        nowUs = talkbackNowUs();
        if (jitterBuffer_.empty() && lastArrivalTimeUs_ != 0 &&
            nowUs - lastArrivalTimeUs_ >= static_cast<uint64_t>(config_.talker_timeout_ms) * 1000ULL) {
            LOG_WARN("audio talkback stream timed out: session=%d ssrc=%u timeout_ms=%d",
                     activeSessionId_,
                     static_cast<unsigned int>(activeSsrc_),
                     config_.talker_timeout_ms);
            clearStreamStateLocked();
            ++stats_.stream_resets;
            lock.unlock();
            result = audio_decoder_reset(decoder_);
            if (result != MEDIA_OK) {
                LOG_ERROR("audio talkback timeout reset decoder failed: result=%d", result);
            }
            lock.lock();
            continue;
        }

        expectedSequence = expectedExtendedSequence_;
        currentPtsUs = nextPtsUs_;
        haveCurrentPacket = false;
        haveFollowingPacket = false;

        /*
         * 优先取出期望包做普通解码。期望包缺失时只复制下一包用于尝试
         * 带内 FEC，不从缓冲删除它，以便下一个播放时刻再正常解码该包。
         */
        packetIt = jitterBuffer_.find(expectedSequence);
        if (packetIt != jitterBuffer_.end()) {
            currentPacket = std::move(packetIt->second);
            jitterBuffer_.erase(packetIt);
            haveCurrentPacket = true;
        } else {
            followingIt = jitterBuffer_.find(expectedSequence + 1U);
            if (followingIt != jitterBuffer_.end()) {
                try {
                    followingPacket = followingIt->second;
                    haveFollowingPacket = true;
                } catch (const std::bad_alloc &error) {
                    LOG_ERROR("audio talkback worker failed to copy FEC packet: sequence=%u exception=%s",
                              followingIt->first,
                              error.what());
                    haveFollowingPacket = false;
                }
            }
        }

        /* 无论包是否存在，播放时钟都前进一帧；缺失位置由 FEC 或 PLC 填补。 */
        ++expectedExtendedSequence_;
        nextPtsUs_ += static_cast<uint64_t>(frameDuration.count());
        deadline += frameDuration;
        lock.unlock();

        /* 解码和阻塞式 ALSA 写入不占用抖动缓冲锁，避免阻塞网络接收线程提交新包。 */
        if (haveCurrentPacket) {
            result = decodePacket(currentPacket, currentPtsUs);
        } else {
            result = recoverLostFrame(haveFollowingPacket ? &followingPacket : NULL, currentPtsUs);
        }

        lock.lock();
        if (result == MEDIA_OK && haveCurrentPacket) {
            ++stats_.decoded_packets;
        }

        /* 解码或设备写入耗时过长时从当前时刻重新定基准，禁止补写形成突发播放。 */
        if (deadline + frameDuration < std::chrono::steady_clock::now()) {
            deadline = std::chrono::steady_clock::now() + frameDuration;
        }
    }
}

} // namespace rkmedia
