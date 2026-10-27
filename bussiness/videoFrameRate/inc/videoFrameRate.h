/**
 * @file videoFrameRate.h
 * @brief 共享视频输入帧率规划和逐码流帧率门控接口。
 *
 * 模块只处理帧率数值，不依赖V4L2或MPP。硬件层负责应用选出的Sensor帧率，视频worker
 * 负责在每次取得输入帧时调用门控接口。
 */

#ifndef __VIDEO_FRAME_RATE_H__
#define __VIDEO_FRAME_RATE_H__

#include "commonDef.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单路码流对共享视频输入的帧率需求。 */
typedef struct
{
    int enabled;    /* 该码流是否参与共享输入帧率规划。 */
    int target_fps; /* 该码流期望编码帧率。 */
} VideoFrameRateRequirement;

/** 单路码流的确定性帧率门控状态。 */
typedef struct
{
    uint32_t input_fps;       /* 共享视频输入实际帧率。 */
    uint32_t target_fps;      /* 当前码流目标编码帧率。 */
    uint64_t phase;           /* 有理数累加器，决定当前输入帧是否通过。 */
    uint64_t input_frames;    /* 已参与门控判断的输入帧数。 */
    uint64_t accepted_frames; /* 允许送入编码器的帧数。 */
    uint64_t dropped_frames;  /* 因目标帧率较低而主动丢弃的帧数。 */
} VideoFrameRateGate;

/**
 * @brief 选择满足全部启用码流所需的共享输入源帧率。
 * @param requirements 各路码流的帧率需求数组。
 * @param requirement_count 数组元素数量。
 * @param source_fps 输出最高目标帧率。
 * @return 成功返回MEDIA_OK；参数非法或没有启用码流时返回错误码。
 */
MediaResult video_frame_rate_select_source_fps(
    const VideoFrameRateRequirement *requirements,
    size_t requirement_count,
    int *source_fps);

/**
 * @brief 初始化或重新配置单路码流帧率门控器。
 * @param gate 门控器。
 * @param input_fps 共享输入帧率。
 * @param target_fps 当前码流目标帧率，必须小于或等于输入帧率。
 * @return 成功返回MEDIA_OK；参数非法返回MEDIA_ERR_INVALID_PARAM。
 */
MediaResult video_frame_rate_gate_init(VideoFrameRateGate *gate,
                                       int input_fps,
                                       int target_fps);

/**
 * @brief 对下一帧输入执行确定性帧率门控。
 * @param gate 已初始化的门控器。
 * @param accepted 输出1表示应编码，输出0表示按帧率计划丢弃。
 * @return 成功返回MEDIA_OK；参数或状态非法返回错误码。
 */
MediaResult video_frame_rate_gate_accept(VideoFrameRateGate *gate,
                                         int *accepted);

#ifdef __cplusplus
}
#endif

#endif
