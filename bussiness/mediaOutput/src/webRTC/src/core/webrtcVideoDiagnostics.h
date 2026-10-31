/**
 * @file webrtcVideoDiagnostics.h
 * @brief 定义 WebRTC 编码视频短时间窗口诊断模块。
 *
 * 本模块只接收已经编码完成的 H.264 帧元数据，在内部维护固定窗口的帧数、
 * 字节数、关键帧和峰值帧统计。调用方不需要了解窗口切换与码率计算细节。
 */
#ifndef __WEBRTC_VIDEO_DIAGNOSTICS_H__
#define __WEBRTC_VIDEO_DIAGNOSTICS_H__

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace rkmedia {
namespace webrtc {

/** @brief 一个已经结束的编码视频诊断窗口快照。 */
struct WebRtcVideoWindowStats {
    bool valid = false; /* 是否已经形成完整窗口。 */
    uint64_t durationMs = 0; /* 窗口实际持续时间，单位毫秒。 */
    uint64_t frames = 0; /* 窗口内编码视频帧数。 */
    uint64_t bytes = 0; /* 窗口内编码视频总字节数。 */
    uint64_t bitrateBps = 0; /* 按实际窗口时长计算的瞬时码率。 */
    uint64_t keyFrames = 0; /* 窗口内关键帧数量。 */
    size_t maxFrameBytes = 0; /* 窗口内最大单帧字节数。 */
    size_t maxKeyFrameBytes = 0; /* 窗口内最大关键帧字节数。 */
};

/**
 * @brief 维护编码视频短时间窗口统计。
 *
 * 该类不执行内部加锁。调用方必须在同一把外部锁保护下调用 reset()、
 * recordFrame() 和 snapshot()，从而避免为每一帧额外引入互斥开销。
 */
class WebRtcVideoDiagnostics {
public:
    /** @brief 创建尚未记录帧的诊断对象。 */
    WebRtcVideoDiagnostics();

    /** @brief 清空当前窗口、最近完整窗口及累计峰值。 */
    void reset();

    /**
     * @brief 记录一帧编码视频，并在窗口到期时返回完整窗口快照。
     * @param[in] frameBytes 当前编码帧字节数。
     * @param[in] keyFrame 当前帧是否为关键帧。
     * @param[in] now 当前单调时钟时间点。
     * @param[out] completedWindow 接收刚结束的窗口快照；无完整窗口时 valid=false。
     */
    void recordFrame(size_t frameBytes,
                     bool keyFrame,
                     const std::chrono::steady_clock::time_point &now,
                     WebRtcVideoWindowStats &completedWindow);

    /**
     * @brief 获取最近完整窗口和运行期间累计峰值。
     * @param[out] latestWindow 最近结束的完整窗口；尚未形成时 valid=false。
     * @param[out] maxFrameBytes 运行期间最大单帧字节数。
     * @param[out] maxKeyFrameBytes 运行期间最大关键帧字节数。
     * @param[out] peakBitrateBps 所有完整窗口中的最高瞬时码率。
     */
    void snapshot(WebRtcVideoWindowStats &latestWindow,
                  size_t &maxFrameBytes,
                  size_t &maxKeyFrameBytes,
                  uint64_t &peakBitrateBps) const;

private:
    /** @brief 使用当前帧开始一个新的统计窗口。 */
    void startWindow(size_t frameBytes,
                     bool keyFrame,
                     const std::chrono::steady_clock::time_point &now);

private:
    bool windowStarted_; /* 当前窗口是否已经收到首帧。 */
    std::chrono::steady_clock::time_point windowStart_; /* 当前窗口首帧时间。 */
    uint64_t windowFrames_; /* 当前窗口帧数。 */
    uint64_t windowBytes_; /* 当前窗口总字节数。 */
    uint64_t windowKeyFrames_; /* 当前窗口关键帧数。 */
    size_t windowMaxFrameBytes_; /* 当前窗口最大帧。 */
    size_t windowMaxKeyFrameBytes_; /* 当前窗口最大关键帧。 */

    WebRtcVideoWindowStats latestWindow_; /* 最近结束的完整窗口。 */
    size_t maxFrameBytes_; /* 运行期间最大帧。 */
    size_t maxKeyFrameBytes_; /* 运行期间最大关键帧。 */
    uint64_t peakBitrateBps_; /* 运行期间最高窗口码率。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
