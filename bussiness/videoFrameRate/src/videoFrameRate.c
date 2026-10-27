/**
 * @file videoFrameRate.c
 * @brief 共享视频输入帧率规划和逐码流帧率门控实现。
 */

#include "videoFrameRate.h"

#include "logger.h"

#include <string.h>

/**
 * @brief 从全部启用码流中选择共享输入源所需的最高帧率。
 */
MediaResult video_frame_rate_select_source_fps(
    const VideoFrameRateRequirement *requirements,
    size_t requirement_count,
    int *source_fps)
{
    size_t index = 0;
    int selected_fps = 0;

    if (!requirements || requirement_count == 0U || !source_fps)
    {
        LOG_ERROR("video_frame_rate_select_source_fps failed: requirements=%p count=%zu source_fps=%p",
                  (const void *)requirements,
                  requirement_count,
                  (void *)source_fps);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* 共享输入必须覆盖最高帧率需求，低帧率码流随后通过各自门控器降帧。 */
    for (index = 0; index < requirement_count; ++index)
    {
        if (!requirements[index].enabled)
            continue;
        if (requirements[index].target_fps <= 0)
        {
            LOG_ERROR("video_frame_rate_select_source_fps failed: invalid target index=%zu fps=%d",
                      index,
                      requirements[index].target_fps);
            return MEDIA_ERR_INVALID_PARAM;
        }
        if (requirements[index].target_fps > selected_fps)
            selected_fps = requirements[index].target_fps;
    }
    if (selected_fps <= 0)
    {
        LOG_ERROR("video_frame_rate_select_source_fps failed: no enabled stream");
        return MEDIA_ERR_NOT_READY;
    }

    *source_fps = selected_fps;
    return MEDIA_OK;
}

/**
 * @brief 初始化单路确定性帧率门控状态。
 */
MediaResult video_frame_rate_gate_init(VideoFrameRateGate *gate,
                                       int input_fps,
                                       int target_fps)
{
    if (!gate || input_fps <= 0 || target_fps <= 0 || target_fps > input_fps)
    {
        LOG_ERROR("video_frame_rate_gate_init failed: gate=%p input=%d target=%d",
                  (void *)gate,
                  input_fps,
                  target_fps);
        return MEDIA_ERR_INVALID_PARAM;
    }

    memset(gate, 0, sizeof(*gate));
    gate->input_fps = (uint32_t)input_fps;
    gate->target_fps = (uint32_t)target_fps;
    /* 预置一个相位差，使新流的第一帧立即通过，缩短首帧等待。 */
    gate->phase = (uint64_t)(input_fps - target_fps);
    return MEDIA_OK;
}

/**
 * @brief 使用有理数相位累加器判断当前输入帧是否通过。
 */
MediaResult video_frame_rate_gate_accept(VideoFrameRateGate *gate,
                                         int *accepted)
{
    if (!gate || !accepted)
    {
        LOG_ERROR("video_frame_rate_gate_accept failed: gate=%p accepted=%p",
                  (void *)gate,
                  (void *)accepted);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (gate->input_fps == 0U || gate->target_fps == 0U ||
        gate->target_fps > gate->input_fps)
    {
        LOG_ERROR("video_frame_rate_gate_accept failed: invalid state input=%u target=%u",
                  gate->input_fps,
                  gate->target_fps);
        return MEDIA_ERR_NOT_READY;
    }

    /*
     * 每个输入帧累加目标帧率；累加值跨过输入帧率时放行一帧并扣除一个周期。
     * 该算法也支持60→25等非整数倍转换，长期输出帧率不会因整数除法产生漂移。
     */
    gate->input_frames++;
    gate->phase += gate->target_fps;
    if (gate->phase >= gate->input_fps)
    {
        gate->phase -= gate->input_fps;
        gate->accepted_frames++;
        *accepted = 1;
    }
    else
    {
        gate->dropped_frames++;
        *accepted = 0;
    }
    return MEDIA_OK;
}
