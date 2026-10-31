/**
 * @file webrtcVideoDiagnostics.cpp
 * @brief 实现 WebRTC 编码视频短时间窗口诊断模块。
 */
#include "webrtcVideoDiagnostics.h"

#include <algorithm>

namespace rkmedia {
namespace webrtc {

/* 500 ms 可观察瞬时突发，同时避免逐帧日志对实时线程造成明显干扰。 */
static const std::chrono::milliseconds WEBRTC_VIDEO_DIAGNOSTIC_WINDOW(500);

WebRtcVideoDiagnostics::WebRtcVideoDiagnostics()
    : windowStarted_(false),
      windowFrames_(0),
      windowBytes_(0),
      windowKeyFrames_(0),
      windowMaxFrameBytes_(0),
      windowMaxKeyFrameBytes_(0),
      maxFrameBytes_(0),
      maxKeyFrameBytes_(0),
      peakBitrateBps_(0)
{
}

/** 清空所有窗口状态，确保端点重新启动后不混入上一次运行的数据。 */
void WebRtcVideoDiagnostics::reset()
{
    windowStarted_ = false;
    windowStart_ = std::chrono::steady_clock::time_point();
    windowFrames_ = 0;
    windowBytes_ = 0;
    windowKeyFrames_ = 0;
    windowMaxFrameBytes_ = 0;
    windowMaxKeyFrameBytes_ = 0;
    latestWindow_ = WebRtcVideoWindowStats();
    maxFrameBytes_ = 0;
    maxKeyFrameBytes_ = 0;
    peakBitrateBps_ = 0;
}

/**
 * 记录编码帧的主要逻辑：
 * 1. 首帧建立窗口；
 * 2. 窗口未满时累加当前帧；
 * 3. 窗口到期时先封存旧窗口，再用当前帧启动下一窗口。
 */
void WebRtcVideoDiagnostics::recordFrame(
    size_t frameBytes,
    bool keyFrame,
    const std::chrono::steady_clock::time_point &now,
    WebRtcVideoWindowStats &completedWindow)
{
    uint64_t durationMs;

    completedWindow = WebRtcVideoWindowStats();
    maxFrameBytes_ = std::max(maxFrameBytes_, frameBytes);
    if (keyFrame) {
        maxKeyFrameBytes_ = std::max(maxKeyFrameBytes_, frameBytes);
    }

    if (!windowStarted_) {
        startWindow(frameBytes, keyFrame, now);
        return;
    }

    if (now - windowStart_ < WEBRTC_VIDEO_DIAGNOSTIC_WINDOW) {
        ++windowFrames_;
        windowBytes_ += static_cast<uint64_t>(frameBytes);
        windowMaxFrameBytes_ = std::max(windowMaxFrameBytes_, frameBytes);
        if (keyFrame) {
            ++windowKeyFrames_;
            windowMaxKeyFrameBytes_ = std::max(windowMaxKeyFrameBytes_, frameBytes);
        }
        return;
    }

    durationMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - windowStart_).count());
    if (durationMs == 0) {
        durationMs = 1;
    }

    completedWindow.valid = true;
    completedWindow.durationMs = durationMs;
    completedWindow.frames = windowFrames_;
    completedWindow.bytes = windowBytes_;
    completedWindow.bitrateBps = windowBytes_ * 8ULL * 1000ULL / durationMs;
    completedWindow.keyFrames = windowKeyFrames_;
    completedWindow.maxFrameBytes = windowMaxFrameBytes_;
    completedWindow.maxKeyFrameBytes = windowMaxKeyFrameBytes_;

    latestWindow_ = completedWindow;
    peakBitrateBps_ = std::max(peakBitrateBps_, completedWindow.bitrateBps);
    startWindow(frameBytes, keyFrame, now);
}

void WebRtcVideoDiagnostics::snapshot(WebRtcVideoWindowStats &latestWindow,
                                      size_t &maxFrameBytes,
                                      size_t &maxKeyFrameBytes,
                                      uint64_t &peakBitrateBps) const
{
    latestWindow = latestWindow_;
    maxFrameBytes = maxFrameBytes_;
    maxKeyFrameBytes = maxKeyFrameBytes_;
    peakBitrateBps = peakBitrateBps_;
}

void WebRtcVideoDiagnostics::startWindow(
    size_t frameBytes,
    bool keyFrame,
    const std::chrono::steady_clock::time_point &now)
{
    windowStarted_ = true;
    windowStart_ = now;
    windowFrames_ = 1;
    windowBytes_ = static_cast<uint64_t>(frameBytes);
    windowKeyFrames_ = keyFrame ? 1 : 0;
    windowMaxFrameBytes_ = frameBytes;
    windowMaxKeyFrameBytes_ = keyFrame ? frameBytes : 0;
}

} // namespace webrtc
} // namespace rkmedia
