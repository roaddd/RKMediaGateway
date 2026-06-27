/**
 * @file mediaGatewayPolicy.c
 * @brief MediaGateway 运行期策略处理。
 *
 * 当前主要负责动态帧率策略：周期性读取 RKISP AE 状态，根据曝光时间、
 * 模拟增益和亮度统计判断亮光、正常、低照场景，并生成 60/30/15fps
 * 目标值。具体硬件切换由 gateway 主循环异步投递到资源所属线程执行。
 */
#include "mediaGatewayPolicy.h"

#include "logger.h"
#include "mediaGatewayClock.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief 获取策略模块使用的单调时钟，单位微秒。
 */
static uint64_t policy_now_us(void)
{
    return media_gateway_get_now_us();
}

/**
 * @brief 返回当前动态帧率场景名称，用于日志和状态说明。
 */
static const char *dynamic_fps_scene_name(const MediaGatewayDynamicFpsState *state)
{
    if (!state)
        return "unknown";
    if (state->low_light_active)
        return "low_light";
    if (state->bright_active)
        return "bright";
    return "normal";
}

/**
 * @brief 将 AE 曝光时间统一转换为微秒。
 *
 * RKAIQ 上报的 integration_time 可能是秒，也可能已经是微秒。
 * 小于 1 的值按秒处理，否则按微秒处理。
 */
static float ae_exposure_us(const IspControllerAeStatus *ae)
{
    if (!ae || ae->integration_time <= 0.0f)
        return 0.0f;

    return (ae->integration_time < 1.0f) ? (ae->integration_time * 1000000.0f) : ae->integration_time;
}

/**
 * @brief 判断 AE 状态是否满足亮光场景。
 *
 * 亮光场景要求曝光时间较短、模拟增益较低，并且平均亮度达到阈值。
 * 满足后状态机可切换到高帧率以降低运动拖影和端到端延迟。
 */
static int dynamic_fps_ae_is_bright(const DynamicFrameRateConfig *cfg, const IspControllerAeStatus *ae)
{
    float exposure_us = 0;

    if (!cfg || !ae || !ae->valid)
    {
        LOG_ERROR("[DYNAMIC_FPS] ae is invalid, skipping bright check");
        return 0;
    }

    exposure_us = ae_exposure_us(ae);

    /* 曝光时间较短、模拟增益较低，并且平均亮度达到阈值 */
    return exposure_us > 0.0f &&
           exposure_us <= cfg->ae.bright_max_exposure_us &&
           ae->analog_gain > 0.0f &&
           ae->analog_gain <= cfg->ae.bright_max_analog_gain &&
           ae->mean_luma >= cfg->ae.bright_min_mean_luma;
}

/**
 * @brief 判断 AE 状态是否满足低照场景。
 *
 * 低照场景要求 AE 接近当前帧周期曝光上限、模拟增益持续偏高，
 * 且平均亮度仍然较低。满足后状态机可切换到 15fps，扩大最大曝光时间。
 */
static int dynamic_fps_ae_is_low_light(const DynamicFrameRateConfig *cfg,
                                       const IspControllerAeStatus *ae,
                                       int current_fps)
{
    float exposure_us = 0;
    float frame_period_us = 0;
    int exposure_near_limit = 0;

    if (!cfg || !ae || !ae->valid || current_fps <= 0)
    {
        LOG_ERROR("[DYNAMIC_FPS] ae is invalid or current_fps <= 0, skipping low_light check");
        return 0;
    }

    exposure_us = ae_exposure_us(ae); /* 将曝光时间转换为微秒 */
    frame_period_us = 1000000.0f / (float)current_fps; /* 帧周期，单位微秒 */
    /* exp_max 为 ISP 明确给出的曝光上限标志；没有该标志时用曝光/帧周期比例兜底判断。 */
    exposure_near_limit = ae->exp_max ||
                          (exposure_us > 0.0f &&
                           frame_period_us > 0.0f &&
                           exposure_us / frame_period_us >= cfg->ae.low_light_min_exposure_ratio);

    /* 低照度状态判断条件: (1) 曝光时间接近上限; (2) 模拟增益较高; (3) 画面平均亮度较低 */
    return exposure_near_limit &&
           ae->analog_gain >= cfg->ae.low_light_min_analog_gain &&
           ae->mean_luma <= cfg->ae.low_light_max_mean_luma;
}

/**
 * @brief 获取目标帧率对应的场景确认时间，单位毫秒。
 */
static int dynamic_fps_confirm_ms(const DynamicFrameRateConfig *cfg, int target_fps)
{
    if (!cfg)
    {
        LOG_ERROR("cfg is null");
        return -1;
    }

    if (target_fps == cfg->targets.bright_fps)
        return cfg->timing.bright_confirm_ms;
    if (target_fps == cfg->targets.low_light_fps)
        return cfg->timing.low_light_confirm_ms;
    return cfg->timing.ae_scene_confirm_ms;
}

/**
 * @brief 按评估周期更新运行期策略。
 *
 * 动态帧率状态机流程：
 * 1. 到达评估周期后读取 ISP AE 状态；
 * 2. 根据 AE 判定亮光、正常或低照目标帧率；
 * 3. 目标需稳定达到确认时间，且满足最小切换间隔后才真正切换；
 * 4. 确认后的目标交给 gateway 异步切换事务处理。
 */
void media_gateway_update_runtime_policies_if_due(MediaGatewayCtx *ctx)
{
    const DynamicFrameRateConfig *cfg = NULL;
    MediaGatewayDynamicFpsState *state = NULL;
    IspControllerStatus isp_status = {0};
    uint64_t now_us = 0;
    uint64_t confirm_us = 0;
    uint64_t elapsed_us = 0;
    int target_fps = 0;
    int low_light_active = 0;
    int bright_active = 0;
    int ae_valid = 0;
    const char *reason = "normal";

    if (!ctx || !ctx->config.dynamic_fps.enabled)
    {
        LOG_DEBUG("[DYNAMIC_FPS] ctx is null or dynamic_fps disabled, skipping policy update");
        return;
    }

    cfg = &ctx->config.dynamic_fps;
    state = &ctx->dynamic_fps_state;
    now_us = policy_now_us();
    /* 限制策略评估频率，避免每帧查询 ISP 和重复触发切换判断。 */
    if (state->last_evaluate_ts_us > 0 &&
        now_us - state->last_evaluate_ts_us < (uint64_t)cfg->timing.evaluate_interval_ms * 1000ULL)
    {
        return;
    }
    state->last_evaluate_ts_us = now_us;

    memset(&isp_status, 0, sizeof(isp_status));
    if (ctx->isp_ready && isp_controller_query_status(&ctx->isp, &isp_status) == 0)
    {
        ae_valid = isp_status.ae.valid;
        if (ae_valid)
        {
            /* 判断是否为低照度状态。 */
            low_light_active = dynamic_fps_ae_is_low_light(cfg, &isp_status.ae, state->current_fps);
            /* 判断是否为亮光状态。 */
            bright_active = dynamic_fps_ae_is_bright(cfg, &isp_status.ae);
        }
    }
    state->low_light_active = low_light_active;
    state->bright_active = bright_active;

    /* AE 不可用时退回正常帧率；低照优先级高于亮光，避免暗场误判到高帧率。 */
    if (!ae_valid)
    {
        target_fps = cfg->targets.normal_fps;
        reason = "ae_unavailable";
    }
    else if (low_light_active)
    {
        target_fps = cfg->targets.low_light_fps;
        reason = "low_light";
    }
    else if (bright_active)
    {
        target_fps = cfg->targets.bright_fps;
        reason = "bright";
    }
    else
    {
        target_fps = cfg->targets.normal_fps;
        reason = "normal";
    }

    /*
     * 先进入 pending 状态，只有同一目标持续达到确认时间才允许成为正式 target。
     * 这样可以过滤短时亮度波动，减少 15/30/60fps 来回抖动。
     */
    if (target_fps != state->pending_fps)
    {
        state->pending_fps = target_fps;
        state->pending_since_ts_us = now_us;
        return;
    }
    else if (target_fps != state->target_fps)
    {
        /* 进入此分支时target_fps等于pending_fps，判断pending的fps等待时间是否满足时间 */
        confirm_us = (uint64_t)dynamic_fps_confirm_ms(cfg, target_fps) * 1000ULL;
        if (now_us >= state->pending_since_ts_us &&
            now_us - state->pending_since_ts_us < confirm_us)
        {
            return;
        }
    }

    if (target_fps != state->target_fps)
    {
        elapsed_us = (state->last_switch_ts_us > 0 && now_us >= state->last_switch_ts_us)
                         ? (now_us - state->last_switch_ts_us)
                         : UINT64_MAX;
        /* 确认时间通过后，还要满足全局最小切换间隔，保护 pipeline 稳定性。 */
        if (elapsed_us >= (uint64_t)cfg->timing.min_switch_interval_ms * 1000ULL)
        {
            state->target_fps = target_fps;
            snprintf(state->reason,
                     sizeof(state->reason),
                     "%s scene=%s ae_valid=%d exp_us=%.2f analog_gain=%.2f mean_luma=%.2f exp_max=%d",
                     reason,
                     dynamic_fps_scene_name(state),
                     ae_valid,
                     ae_exposure_us(&isp_status.ae),
                     isp_status.ae.analog_gain,
                     isp_status.ae.mean_luma,
                     isp_status.ae.exp_max);
        }
    }

    if (state->target_fps != state->last_logged_target_fps)
    {
        LOG_WARN("[DYNAMIC_FPS] target_fps=%d current_fps=%d reason=[%s] action=try_apply",
                 state->target_fps,
                 state->current_fps,
                 state->reason[0] ? state->reason : reason);
        state->last_logged_target_fps = state->target_fps;
    }

    /* 此处不直接操作硬件，gateway 主循环会异步投递目标帧率。 */
}
