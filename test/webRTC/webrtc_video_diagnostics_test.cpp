/**
 * @file webrtc_video_diagnostics_test.cpp
 * @brief 验证 WebRTC 编码视频短窗口统计和累计峰值计算。
 */
#include "webrtcVideoDiagnostics.h"

#include <chrono>
#include <cstdio>

using rkmedia::webrtc::WebRtcVideoDiagnostics;
using rkmedia::webrtc::WebRtcVideoWindowStats;

#define CHECK_TRUE(condition)                                                        \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "check failed: %s line=%d\n", #condition, __LINE__); \
            return 1;                                                                \
        }                                                                            \
    } while (0)

/** 验证窗口到期时旧窗口被封存，触发窗口切换的当前帧进入新窗口。 */
static int testWindowCompletion()
{
    WebRtcVideoDiagnostics diagnostics;
    WebRtcVideoWindowStats window;
    std::chrono::steady_clock::time_point start;

    start = std::chrono::steady_clock::time_point(std::chrono::milliseconds(1000));
    diagnostics.recordFrame(1000, false, start, window);
    CHECK_TRUE(!window.valid);
    diagnostics.recordFrame(2000, true, start + std::chrono::milliseconds(100), window);
    CHECK_TRUE(!window.valid);
    diagnostics.recordFrame(3000, false, start + std::chrono::milliseconds(500), window);

    CHECK_TRUE(window.valid);
    CHECK_TRUE(window.durationMs == 500);
    CHECK_TRUE(window.frames == 2);
    CHECK_TRUE(window.bytes == 3000);
    CHECK_TRUE(window.bitrateBps == 48000);
    CHECK_TRUE(window.keyFrames == 1);
    CHECK_TRUE(window.maxFrameBytes == 2000);
    CHECK_TRUE(window.maxKeyFrameBytes == 2000);
    return 0;
}

/** 验证累计最大帧、最大关键帧和窗口峰值码率不会在窗口切换时丢失。 */
static int testPeakSnapshot()
{
    WebRtcVideoDiagnostics diagnostics;
    WebRtcVideoWindowStats completed;
    WebRtcVideoWindowStats latest;
    std::chrono::steady_clock::time_point start;
    size_t maxFrameBytes;
    size_t maxKeyFrameBytes;
    uint64_t peakBitrateBps;

    maxFrameBytes = 0;
    maxKeyFrameBytes = 0;
    peakBitrateBps = 0;
    start = std::chrono::steady_clock::time_point(std::chrono::milliseconds(2000));
    diagnostics.recordFrame(4000, true, start, completed);
    diagnostics.recordFrame(6000, false, start + std::chrono::milliseconds(100), completed);
    diagnostics.recordFrame(1000, false, start + std::chrono::milliseconds(500), completed);
    diagnostics.snapshot(latest, maxFrameBytes, maxKeyFrameBytes, peakBitrateBps);

    CHECK_TRUE(latest.valid);
    CHECK_TRUE(latest.bytes == 10000);
    CHECK_TRUE(peakBitrateBps == 160000);
    CHECK_TRUE(maxFrameBytes == 6000);
    CHECK_TRUE(maxKeyFrameBytes == 4000);
    return 0;
}

int main()
{
    CHECK_TRUE(testWindowCompletion() == 0);
    CHECK_TRUE(testPeakSnapshot() == 0);
    std::printf("webrtc video diagnostics tests passed\n");
    return 0;
}
