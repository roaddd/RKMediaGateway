/**
 * @file audioTalkback.h
 * @brief 双向语音内部 C++ 类声明，仅供 audioTalkback 模块实现使用。
 */

#ifndef __AUDIO_TALKBACK_H__
#define __AUDIO_TALKBACK_H__

#include "audioTalkbackModule.h"

#include "audioDecoder.h"
#include "audioPlayback.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rkmedia {

/**
 * @description: 串联入站 Opus 抖动缓冲、丢包恢复、解码和 ALSA 播放。
 *
 * 该类只在 audioTalkback 模块内部可见。网络接收线程通过 submit() 复制编码包，
 * 耗时的解码和设备写入由内部 worker 串行执行。
 */
class AudioTalkback {
public:
    /**
     * @description: 深拷贝配置字符串，并建立尚未启动的运行状态。
     * @param config 已通过模块入口严格校验的完整配置。
     * @note 构造函数不创建解码器、Playback 设备或 worker，实际启动由 start() 完成。
     */
    explicit AudioTalkback(const AudioTalkbackConfig &config);

    /** @description: 确保 worker 和底层媒体资源在对象销毁前全部停止。 */
    ~AudioTalkback();

    /**
     * @description: 初始化 Mixer、Opus 解码器和 ALSA Playback，并启动 worker。
     * @return MEDIA_OK 启动成功；已经运行时返回 MEDIA_ERR_BUSY；否则返回具体初始化错误。
     */
    MediaResult start();

    /**
     * @description: 复制并提交一包入站 Opus RTP 负载，不在调用线程执行解码。
     * @param packet 入站 RTP 元数据和 Opus 负载视图；函数返回前会完成负载复制。
     * @return MEDIA_OK 已接收或按策略忽略；未启动、缓冲区满或说话人冲突时返回相应错误。
     * @note 调用方可在函数返回后立即释放 packet.payload 指向的原始内存。
     */
    MediaResult submit(const AudioTalkbackPacket &packet);

    /**
     * @description: 停止 worker，并按依赖逆序释放 Playback 和解码器。
     * @return MEDIA_OK 停止成功或当前已经停止；否则返回 Playback 释放错误。
     */
    MediaResult stop();

    /**
     * @description: 获取抖动缓冲、解码、恢复和播放累计统计快照。
     * @param stats 输出统计快照，不能为空。
     * @return MEDIA_OK 获取成功；参数无效时返回 MEDIA_ERR_INVALID_PARAM。
     */
    MediaResult getStats(AudioTalkbackStats *stats) const;

private:
    /** @brief 模块内部持有的一包 Opus 数据及其已扩展 RTP 序号。 */
    struct BufferedTalkbackPacket {
        uint32_t extendedSequence = 0; /* 处理 16 位回绕后的连续 RTP 序号。 */
        uint32_t rtpTimestamp = 0;     /* 原始 48 kHz RTP 媒体时间戳。 */
        uint64_t arrivalTimeUs = 0;   /* 数据包到达设备的单调时钟时间。 */
        std::vector<uint8_t> payload;   /* 模块独占的 Opus 负载副本。 */
    };

    /**
     * @description: 选择新的活动流，清空旧时序并请求 worker 串行重置 Opus 状态。
     * @param session_id 新活动流所属的 WebRTC 会话 ID。
     * @param ssrc 新活动流的音频 RTP SSRC。
     * @param reason 切换原因，可为 NULL；仅用于日志记录。
     * @note 调用方必须已经持有 mutex_。
     */
    void resetStreamLocked(int session_id, uint32_t ssrc, const char *reason);

    /**
     * @description: 清理当前说话人、RTP 时序和播放时钟状态。
     * @note 调用方必须已经持有 mutex_；本函数不清空 jitterBuffer_，也不直接重置解码器。
     */
    void clearStreamStateLocked();

    /**
     * @description: 校验解码输出、适配硬件声道并写入 ALSA。
     * @param output 解码器返回的 PCM 数据视图及音频参数。
     * @return MEDIA_OK 播放帧写入成功；否则返回输出校验或 Playback 写入错误。
     */
    MediaResult playDecodedFrame(const AudioDecoderOutput &output);

    /**
     * @description: 正常解码并播放一个完整 Opus 包。
     * @param packet 已从抖动缓冲区取出的当前序号数据包。
     * @param pts_us 分配给本次解码输出帧的连续微秒 PTS。
     * @return MEDIA_OK 解码并播放成功；否则返回解码或 Playback 错误。
     */
    MediaResult decodePacket(const BufferedTalkbackPacket &packet, uint64_t pts_us);

    /**
     * @description: 恢复并播放当前缺失的音频帧。
     * @param following_packet 缺失帧的后继 Opus 包，可为 NULL。非 NULL 时优先尝试读取
     *        其中携带的前一帧带内 FEC；包中没有可用 FEC 时由解码器自动回退 PLC。
     *        为 NULL 时表示后继包尚未到达，直接使用解码器 PLC 生成补偿帧。
     * @param pts_us 分配给当前缺失帧的连续微秒 PTS，而不是后继包的 PTS。
     * @return MEDIA_OK 表示 FEC 或 PLC 输出已经成功写入 Playback；否则返回恢复或播放错误。
     * @note following_packet 只用于恢复前一帧，不会在本函数中作为当前帧正常解码。
     */
    MediaResult recoverLostFrame(const BufferedTalkbackPacket *following_packet, uint64_t pts_us);

    /**
     * @description: 在 mutex_ 保护下累计一次解码或恢复错误。
     * @note 调用方不得持有 mutex_，避免对非递归互斥锁重复加锁。
     */
    void recordDecodeError();

    /**
     * @description: 串行执行预缓冲、定时出队、FEC/PLC、Opus 解码及 ALSA 播放。
     * @note 仅作为 worker_ 的线程入口调用；函数在 running_ 被清除后退出。
     */
    void workerMain();

private:
    /* ---------- 启动配置及字符串所有权 ---------- */
    AudioTalkbackConfig config_ = {}; /* 创建时保存的只读配置；字符串指针指向下方自持有字符串。 */
    std::string playbackDevice_;      /* 自持有 ALSA PCM 设备名；构造后不再修改。 */
    std::string mixerCard_;           /* 自持有 ALSA Mixer 控制卡名称；构造后不再修改。 */
    std::string mixerControl_;        /* 自持有 Playback 路由枚举控件名称；构造后不再修改。 */
    std::string mixerValue_;          /* 自持有 Playback 路由枚举值；构造后不再修改。 */

    /* ---------- worker 独占的媒体处理资源 ---------- */
    AudioDecoderHandle *decoder_ = NULL; /* 当前活动语音流独占的有状态 Opus 解码器。 */
    AudioPlayback playback_;             /* ALSA Playback 封装；worker 退出后才允许释放。 */
    std::vector<int16_t> stereoBuffer_;  /* worker 复用的单声道转 RK809 双声道 PCM 缓存。 */

    /* ---------- 工作线程及同步状态 ---------- */
    std::thread worker_; /* 抖动缓冲、恢复、解码及播放工作线程；仅由 start()/stop() 管理。 */

    /**
     * 保护以下共享状态：
     * 1. worker 生命周期标志 running_、resetPending_；
     * 2. 活动说话人及超时状态；
     * 3. RTP 序号、播放时钟和 jitterBuffer_；
     * 4. stats_ 累计统计。
     *
     * decoder_、playback_ 和 stereoBuffer_ 不依赖该锁并发访问：start() 在线程创建前
     * 完成初始化，运行期间仅由 worker 使用，stop() 在 join worker 后才释放它们。
     */
    mutable std::mutex mutex_;
    std::condition_variable condition_; /* 唤醒等待预缓存、流重置或停止的 worker。 */
    bool running_ = false;               /* 是否允许 submit，并要求 worker 继续运行。 */
    bool resetPending_ = false;          /* 是否要求 worker 重置 Opus 解码状态。 */

    /* ---------- 活动说话人选择及超时状态（受 mutex_ 保护） ---------- */
    bool hasActiveTalker_ = false;   /* 当前是否已经选定允许播放的说话人。 */
    int activeSessionId_ = 0;        /* 当前活动说话人所属 WebRTC 会话 ID。 */
    uint32_t activeSsrc_ = 0;        /* 当前活动说话人的音频 RTP SSRC。 */
    uint64_t lastArrivalTimeUs_ = 0; /* 当前流最近一包到达时间，用于超时切换。 */

    /* ---------- RTP 时序及抖动缓冲（受 mutex_ 保护） ---------- */
    bool hasHighestSequence_ = false; /* 最大扩展 RTP 序号是否已有有效基准。 */
    uint32_t highestExtendedSequence_ = 0; /* 当前流已收到的最大扩展 RTP 序号。 */
    bool playoutStarted_ = false; /* 是否已完成预缓存并建立固定间隔播放时钟。 */
    uint32_t expectedExtendedSequence_ = 0; /* 下一播放时刻期望消费或恢复的 RTP 序号。 */
    uint64_t nextPtsUs_ = 0; /* 下一解码输出帧使用的连续微秒 PTS。 */
    std::map<uint32_t, BufferedTalkbackPacket> jitterBuffer_; /* 按扩展序号排序的有界 Opus 包缓冲。 */

    /* ---------- 运行统计（受 mutex_ 保护） ---------- */
    AudioTalkbackStats stats_ = {}; /* 抖动缓冲、解码、恢复及播放累计统计。 */

    AudioTalkback(const AudioTalkback &) = delete;
    AudioTalkback &operator=(const AudioTalkback &) = delete;
};

} // namespace rkmedia

#endif /* __AUDIO_TALKBACK_H__ */
