/**
 * @file ispController.c
 * @brief RKAIQ/ISP 生命周期控制与状态查询。
 *
 * 当前测试模式只依赖状态查询输出 ISP/3A 参数；低照度主动控制由配置关闭，
 * 上层 policy 也不会主动触发 ISP 参数或编码器码率联动。
 */
#include "ispController.h"

#include "logger.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef ENABLE_RKAIQ
#include <stdbool.h>
#include <stdint.h>
#include "rk_aiq_user_api2_ae.h"
#include "rk_aiq_user_api2_awb.h"
#include "rk_aiq_user_api2_imgproc.h"
#include "rk_aiq_user_api2_sysctl.h"
#endif

#ifdef ENABLE_RKAIQ
static IspControllerCtx *g_active_isp_ctx = NULL;
#endif


/**
 * @brief 获取单调时钟时间戳，单位微秒。
 */
static uint64_t get_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}


/**
 * @brief 返回可用字符串，输入为空或空串时返回 fallback。
 */
static const char *safe_config_str(const char *value, const char *fallback)
{
    return (value && value[0] != '\0') ? value : fallback;
}

/**
 * @brief 判断字符串是否带指定后缀。
 */
static int str_has_suffix(const char *value, const char *suffix)
{
    size_t value_len;
    size_t suffix_len;

    if (!value || !suffix)
        return 0;

    value_len = strlen(value);
    suffix_len = strlen(suffix);
    if (value_len < suffix_len)
        return 0;

    return strcmp(value + value_len - suffix_len, suffix) == 0;
}

/**
 * @brief 当前 RKAIQ uAPI2 preInit 只可靠支持 XML force IQ。
 */
static int should_preinit_force_iq_file(const char *force_iq_file)
{
    return force_iq_file &&
           force_iq_file[0] != '\0' &&
           str_has_suffix(force_iq_file, ".xml");
}

/**
 * @brief 判断手动白平衡 gain 四个通道是否都已配置。
 */
static int has_manual_wb_gain(const IspControllerImageControls *controls)
{
    return controls->wb_rgain > 0.0f &&
           controls->wb_grgain > 0.0f &&
           controls->wb_gbgain > 0.0f &&
           controls->wb_bgain > 0.0f;
}


/**
 * @brief 判断 ISP 初始化失败后是否允许降级继续运行。
 */
static int isp_controller_should_fallback(const IspControllerConfig *config)
{
    return !config || config->lifecycle.fallback_on_error;
}

/**
 * @brief 归一化 ISP 配置，补齐默认值并裁剪策略参数范围。
 */
static IspControllerConfig normalize_isp_config(const IspControllerConfig *config)
{
    IspControllerConfig cfg = *config;

    cfg.sensor.iq_dir = safe_config_str(cfg.sensor.iq_dir, "thirdparty/rkaiq");
    cfg.sensor.video_device = safe_config_str(cfg.sensor.video_device, "/dev/video0");
    cfg.sensor.working_mode = (cfg.sensor.working_mode < 0) ? 0 : cfg.sensor.working_mode;
    cfg.health.check_enable = cfg.health.check_enable ? 1 : 0;
    if (cfg.health.meta_timeout_ms <= 0)
        cfg.health.meta_timeout_ms = 2000;
    if (cfg.health.max_error_count <= 0)
        cfg.health.max_error_count = 3;
    cfg.health.restart_on_fault = cfg.health.restart_on_fault ? 1 : 0;
    cfg.low_light.enabled = cfg.low_light.enabled ? 1 : 0;
    if (cfg.low_light.exit_lux <= cfg.low_light.enter_lux)
        cfg.low_light.exit_lux = cfg.low_light.enter_lux * 1.5f;
    if (cfg.low_light.exit_mean_luma <= cfg.low_light.enter_mean_luma)
        cfg.low_light.exit_mean_luma = cfg.low_light.enter_mean_luma + 8.0f;
    if (cfg.low_light.nr_strength < -1)
        cfg.low_light.nr_strength = -1;
    if (cfg.low_light.sharpness < -1)
        cfg.low_light.sharpness = -1;
    if (cfg.low_light.normal_nr_strength < -1)
        cfg.low_light.normal_nr_strength = -1;
    if (cfg.low_light.normal_sharpness < -1)
        cfg.low_light.normal_sharpness = -1;
    if (cfg.low_light.bitrate_boost_percent < 0)
        cfg.low_light.bitrate_boost_percent = 0;
    if (cfg.low_light.qp_delta < -20)
        cfg.low_light.qp_delta = -20;
    if (cfg.low_light.qp_delta > 20)
        cfg.low_light.qp_delta = 20;
    if (cfg.low_light.min_switch_interval_ms <= 0)
        cfg.low_light.min_switch_interval_ms = 5000;
    if (cfg.sensor.width <= 0)
        cfg.sensor.width = 1920;
    if (cfg.sensor.height <= 0)
        cfg.sensor.height = 1080;

    return cfg;
}

/**
 * @brief 将整数限制在指定闭区间内。
 */
static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

/**
 * @brief 写入 ISP 健康状态原因字符串。
 */
static void set_health_reason(IspControllerStatus *status, const char *reason)
{
    snprintf(status->health.reason, sizeof(status->health.reason), "%s", safe_config_str(reason, ""));
}


/**
 * @brief 根据生命周期、回调计数和超时信息计算 ISP 健康状态。
 *
 * 健康诊断只写入 IspControllerStatus，不直接重启 RKAIQ。RKAIQ 重启需要
 * 与 V4L2 STREAMOFF/STREAMON、采集线程退出、编码器输入队列清理协同处理，
 * 不能在状态查询路径里直接做破坏性动作。
 */
static void fill_health_status(IspControllerCtx *ctx, IspControllerStatus *status)
{
    uint64_t now_us = get_now_us();
    uint64_t timeout_us = 0;
    uint64_t base_meta_ts = 0;

    status->health.enabled = ctx->config.health.check_enable ? 1 : 0;

    /* 诊断关闭时显式返回 disabled，避免日志误判为异常。 */
    if (!status->health.enabled)
    {
        status->health.state = ISP_CONTROLLER_HEALTH_DISABLED;
        set_health_reason(status, "health check disabled");
        return;
    }

    if (!ctx->config.lifecycle.enabled)
    {
        status->health.state = ISP_CONTROLLER_HEALTH_DISABLED;
        set_health_reason(status, "ISP disabled by config");
        return;
    }


    /* 配置启用但未 started，通常意味着 RKAIQ 初始化降级或启动失败。 */
    if (!ctx->started)
    {
        status->health.state = ISP_CONTROLLER_HEALTH_NOT_STARTED;
        set_health_reason(status, "RKAIQ is not started");
        return;
    }

    status->lifecycle.uptime_us = (ctx->start_ts_us > 0 && now_us >= ctx->start_ts_us) ? (now_us - ctx->start_ts_us) : 0;
    base_meta_ts = status->callbacks.last_meta_ts_us ? status->callbacks.last_meta_ts_us : ctx->start_ts_us;
    if (base_meta_ts > 0 && now_us >= base_meta_ts)
        status->health.meta_stall_us = now_us - base_meta_ts;


    /* error 回调是 RKAIQ 明确上报的异常，优先级高于 metas 停滞。 */
    if (ctx->config.health.max_error_count > 0 && status->callbacks.error_count >= (uint64_t)ctx->config.health.max_error_count)
    {
        status->health.state = ISP_CONTROLLER_HEALTH_ERROR_LIMIT;
        snprintf(status->health.reason,
                 sizeof(status->health.reason),
                 "RKAIQ error count reached limit: count=%" PRIu64 " limit=%d",
                 status->callbacks.error_count,
                 ctx->config.health.max_error_count);
        return;
    }


    /* metas 长时间不更新通常说明 3A/ISP pipeline 没有持续产出帧级状态。 */
    if (ctx->config.health.meta_timeout_ms > 0)
    {
        timeout_us = (uint64_t)ctx->config.health.meta_timeout_ms * 1000ULL;
        if (status->health.meta_stall_us > timeout_us)
        {
            status->health.state = ISP_CONTROLLER_HEALTH_META_STALLED;
            snprintf(status->health.reason,
                     sizeof(status->health.reason),
                     "RKAIQ metas stalled: stall_ms=%" PRIu64 " timeout_ms=%d",
                     status->health.meta_stall_us / 1000ULL,
                     ctx->config.health.meta_timeout_ms);
            return;
        }
    }

    status->health.state = ISP_CONTROLLER_HEALTH_OK;
    set_health_reason(status, "OK");
}


/**
 * @brief 判断是否需要进入低照度优化模式。
 *
 * 优先使用 RKAIQ AE 给出的 env_lux；当 env_lux 无效时，使用 mean_luma
 * 作为兜底判断。任一维度达到进入阈值即可进入低照度模式。
 */
static int low_light_should_enter(const IspControllerLowLightConfig *cfg, const IspControllerStatus *status)
{
    if (!status->ae.valid)
        return 0;
    if (cfg->enter_lux > 0.0f && status->ae.env_lux > 0.0f && status->ae.env_lux <= cfg->enter_lux)
        return 1;
    if (cfg->enter_mean_luma > 0.0f && status->ae.mean_luma <= cfg->enter_mean_luma)
        return 1;
    return 0;
}


/**
 * @brief 判断是否可以退出低照度优化模式。
 *
 * 退出条件使用回滞阈值，避免光照处于临界点时反复切换。
 */
static int low_light_should_exit(const IspControllerLowLightConfig *cfg, const IspControllerStatus *status)
{
    int lux_ok = 1;
    int luma_ok = 1;

    if (!status->ae.valid)
        return 0;
    if (cfg->exit_lux > 0.0f && status->ae.env_lux > 0.0f)
        lux_ok = (status->ae.env_lux >= cfg->exit_lux);
    if (cfg->exit_mean_luma > 0.0f)
        luma_ok = (status->ae.mean_luma >= cfg->exit_mean_luma);
    return lux_ok && luma_ok;
}


/**
 * @brief 构造低照度进入或退出时需要下发的 RKAIQ 图像控制批次。
 *
 * active=1 时按低照度配置提高曝光/增益、增强降噪、降低锐度；
 * active=0 时恢复自动曝光和普通光照下的降噪/锐度配置。
 */
static void build_low_light_controls(IspControllerCtx *ctx,
                                     int active,
                                     IspControllerImageControls *controls)
{
    memset(controls, 0, sizeof(*controls));

    /* 先把所有字段初始化为“不修改”，后续只打开策略负责的控制项。 */
    controls->enabled = 1;
    controls->brightness = -1;
    controls->contrast = -1;
    controls->saturation = -1;
    controls->hue = -1;
    controls->sharpness = -1;
    controls->exposure_mode = -1;
    controls->exposure_time = -1.0f;
    controls->exposure_gain = -1.0f;
    controls->wb_mode = -1;
    controls->wb_ct = 0;
    controls->wb_rgain = -1.0f;
    controls->wb_grgain = -1.0f;
    controls->wb_gbgain = -1.0f;
    controls->wb_bgain = -1.0f;
    controls->anti_flicker_enable = -1;
    controls->power_line_freq = -1;
    controls->nr_mode = -1;
    controls->anr_strength = -1;
    controls->dehaze_mode = -1;
    controls->dehaze_strength = -1;

    if (active)
    {

        /* 固定曝光时间/增益前先切到手动模式，否则 AE 可能覆盖范围设置。 */
        if (ctx->config.low_light.exposure_gain > 0.0f ||
            ctx->config.low_light.exposure_time > 0.0f)
            controls->exposure_mode = 1;
        controls->exposure_gain = ctx->config.low_light.exposure_gain;
        controls->exposure_time = ctx->config.low_light.exposure_time;
        if (ctx->config.low_light.nr_strength >= 0)
        {

            /* 手动降噪强度要求 NR 进入 manual 模式。 */
            controls->nr_mode = 1;
            controls->anr_strength = ctx->config.low_light.nr_strength;
        }
        controls->sharpness = ctx->config.low_light.sharpness;
        return;
    }


    /* 退出低照度时恢复自动曝光，避免白天仍锁在夜间曝光参数。 */
    controls->exposure_mode = 0;
    if (ctx->config.low_light.normal_nr_strength >= 0)
    {
        controls->nr_mode = 1;
        controls->anr_strength = ctx->config.low_light.normal_nr_strength;
    }
    else
    {

        /* normal_nr_strength 未配置时，直接恢复自动降噪。 */
        controls->nr_mode = 0;
    }
    controls->sharpness = ctx->config.low_light.normal_sharpness;
}


/**
 * @brief 周期性更新低照度策略状态。
 *
 * 该函数只在“进入/退出状态发生变化”时下发 RKAIQ 控制，平稳处于低照度或
 * 普通光照时不重复调用 RKAIQ。min_switch_interval_ms 用于防止临界抖动。
 */
int isp_controller_update_low_light(IspControllerCtx *ctx, IspControllerStatus *status)
{
    IspControllerStatus local_status;
    IspControllerImageControls controls;
    uint64_t now_us = get_now_us();
    uint64_t min_interval_us;
    int target_active;
    const char *reason;

    if (!ctx)
        return -1;


    /* 策略关闭或 RKAIQ 未 started 时不参与判断，保持原有 IQ/3A 行为。 */
    if (!ctx->config.low_light.enabled || !ctx->started)
        return 0;


    /* 调用方未传入状态时，内部查询一次，方便未来控制面直接调用。 */
    if (!status)
    {
        if (isp_controller_query_status(ctx, &local_status) != 0)
            return -1;
        status = &local_status;
    }


    /* 低照度判断依赖 AE 查询结果；AE 无效时不盲目切换。 */
    if (!status->ae.valid)
        return 0;

    target_active = ctx->low_light_active;
    if (!ctx->low_light_active && low_light_should_enter(&ctx->config.low_light, status))
    {
        target_active = 1;
        reason = "enter low light";
    }
    else if (ctx->low_light_active && low_light_should_exit(&ctx->config.low_light, status))
    {
        target_active = 0;
        reason = "exit low light";
    }
    else
    {
        return 0;
    }


    /* 回滞之外再叠加最小切换间隔，减少灯光抖动导致的频繁切换。 */
    min_interval_us = (uint64_t)ctx->config.low_light.min_switch_interval_ms * 1000ULL;
    if (ctx->low_light_last_switch_ts_us > 0 &&
        now_us >= ctx->low_light_last_switch_ts_us &&
        now_us - ctx->low_light_last_switch_ts_us < min_interval_us)
    {
        return 0;
    }


    /* 状态真正变化后再构造控制批次并下发。 */
    build_low_light_controls(ctx, target_active, &controls);
    if (isp_controller_apply_image_controls(ctx, &controls) != 0)
        return -1;


    if (ctx->status_lock_ready)
        pthread_mutex_lock(&ctx->status_lock);
    ctx->low_light_active = target_active;
    ctx->low_light_last_switch_ts_us = now_us;
    snprintf(ctx->low_light_reason,
             sizeof(ctx->low_light_reason),
             "%s env_lux=%.2f mean_luma=%.2f",
             reason,
             status->ae.env_lux,
             status->ae.mean_luma);
    if (ctx->status_lock_ready)
        pthread_mutex_unlock(&ctx->status_lock);

    LOG_WARN("[ISP_LOWLIGHT] active=%d reason=%s env_lux=%.2f mean_luma=%.2f",
             target_active,
             reason,
             status->ae.env_lux,
             status->ae.mean_luma);
    return 0;
}


/**
 * @brief 向已启动的 RKAIQ 上下文下发运行时图像控制参数。
 *
 * 该函数按控制类别逐项检查哨兵值，只对明确配置的字段调用 RKAIQ API。
 * 某个控制项失败时继续尝试后续项，最后统一返回失败，便于一次性暴露
 * 当前 SDK/算法版本不支持的控制项。
 */
int isp_controller_apply_image_controls(IspControllerCtx *ctx, const IspControllerImageControls *controls)
{
    IspControllerImageControls applied;


    /* 参数校验只检查上下文和控制批次本身，字段是否生效由哨兵值决定。 */
    if (!ctx || !controls)
    {
        LOG_ERROR("[ISP_CTRL] apply failed: invalid argument");
        return -1;
    }


    /* 控制功能关闭时保持 IQ/3A 默认行为，作为无操作成功返回。 */
    if (!controls->enabled)
        return 0;

#ifndef ENABLE_RKAIQ

    LOG_WARN("[ISP_CTRL] RKAIQ support is not compiled in; skip runtime image controls");
    return 0;
#else
    {
        rk_aiq_sys_ctx_t *sys_ctx = (rk_aiq_sys_ctx_t *)ctx->sys_ctx;
        int failures = 0;
        XCamReturn ret;

        if (!sys_ctx || !ctx->started)
        {
            LOG_ERROR("[ISP_CTRL] apply failed: RKAIQ is not started");
            return -1;
        }

        applied = *controls;


        /* 图像基础属性使用 RKAIQ 0-255/0-100 范围，超出配置会钳位。 */
        if (controls->brightness >= 0)
        {
            unsigned int level = (unsigned int)clamp_int(controls->brightness, 0, 255);
            ret = rk_aiq_uapi_setBrightness(sys_ctx, level);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set brightness failed ret=%d level=%u", ret, level);
                failures++;
            }
        }

        if (controls->contrast >= 0)
        {
            unsigned int level = (unsigned int)clamp_int(controls->contrast, 0, 255);
            ret = rk_aiq_uapi_setContrast(sys_ctx, level);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set contrast failed ret=%d level=%u", ret, level);
                failures++;
            }
        }

        if (controls->saturation >= 0)
        {
            unsigned int level = (unsigned int)clamp_int(controls->saturation, 0, 255);
            ret = rk_aiq_uapi_setSaturation(sys_ctx, level);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set saturation failed ret=%d level=%u", ret, level);
                failures++;
            }
        }

        if (controls->hue >= 0)
        {
            unsigned int level = (unsigned int)clamp_int(controls->hue, 0, 255);
            ret = rk_aiq_uapi_setHue(sys_ctx, level);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set hue failed ret=%d level=%u", ret, level);
                failures++;
            }
        }

        if (controls->sharpness >= 0)
        {
            unsigned int level = (unsigned int)clamp_int(controls->sharpness, 0, 100);
            ret = rk_aiq_uapi_setSharpness(sys_ctx, level);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set sharpness failed ret=%d level=%u", ret, level);
                failures++;
            }
        }


        /* 曝光相关控制：模式、曝光时间范围和增益范围。 */
        if (controls->exposure_mode >= 0)
        {
            ret = rk_aiq_uapi2_setExpMode(sys_ctx, (opMode_t)controls->exposure_mode);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set exposure mode failed ret=%d mode=%d",
                          ret,
                          controls->exposure_mode);
                failures++;
            }
        }

        if (controls->exposure_time > 0.0f)
        {
            paRange_t range;
            range.min = controls->exposure_time;
            range.max = controls->exposure_time;
            ret = rk_aiq_uapi2_setExpTimeRange(sys_ctx, &range);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set exposure time failed ret=%d value=%.6f",
                          ret,
                          controls->exposure_time);
                failures++;
            }
        }

        if (controls->exposure_gain > 0.0f)
        {
            paRange_t range;
            range.min = controls->exposure_gain;
            range.max = controls->exposure_gain;
            ret = rk_aiq_uapi2_setExpGainRange(sys_ctx, &range);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set exposure gain failed ret=%d value=%.3f",
                          ret,
                          controls->exposure_gain);
                failures++;
            }
        }


        /* 白平衡相关控制：模式、色温和手动四通道 gain。 */
        if (controls->wb_mode >= 0)
        {
            ret = rk_aiq_uapi2_setWBMode(sys_ctx, (opMode_t)controls->wb_mode);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set white balance mode failed ret=%d mode=%d",
                          ret,
                          controls->wb_mode);
                failures++;
            }
        }

        if (controls->wb_ct > 0)
        {
            ret = rk_aiq_uapi2_setMWBCT(sys_ctx, (unsigned int)controls->wb_ct);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set white balance CT failed ret=%d ct=%d",
                          ret,
                          controls->wb_ct);
                failures++;
            }
        }

        if (has_manual_wb_gain(controls))
        {
            rk_aiq_wb_gain_t gain;
            gain.rgain = controls->wb_rgain;
            gain.grgain = controls->wb_grgain;
            gain.gbgain = controls->wb_gbgain;
            gain.bgain = controls->wb_bgain;
            ret = rk_aiq_uapi2_setMWBGain(sys_ctx, &gain);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set white balance gain failed ret=%d gain=(%.3f,%.3f,%.3f,%.3f)",
                          ret,
                          gain.rgain,
                          gain.grgain,
                          gain.gbgain,
                          gain.bgain);
                failures++;
            }
        }


        /* 抗频闪和电源频率用于抑制 50Hz/60Hz 照明环境下的闪烁。 */
        if (controls->anti_flicker_enable >= 0)
        {
            ret = rk_aiq_uapi2_setAntiFlickerEn(sys_ctx, controls->anti_flicker_enable ? true : false);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set anti-flicker enable failed ret=%d enable=%d",
                          ret,
                          controls->anti_flicker_enable ? 1 : 0);
                failures++;
            }
        }

        if (controls->power_line_freq >= 0)
        {
            int freq = clamp_int(controls->power_line_freq, 0, 2);
            ret = rk_aiq_uapi2_setExpPwrLineFreqMode(sys_ctx, (expPwrLineFreq_t)freq);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set power line frequency failed ret=%d freq=%d", ret, freq);
                failures++;
            }
        }


        /* 降噪和去雾属于画质增强类控制。 */
        if (controls->nr_mode >= 0)
        {
            ret = rk_aiq_uapi2_setNRMode(sys_ctx, (opMode_t)controls->nr_mode);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set NR mode failed ret=%d mode=%d", ret, controls->nr_mode);
                failures++;
            }
        }

        if (controls->anr_strength >= 0)
        {
            unsigned int level = (unsigned int)clamp_int(controls->anr_strength, 0, 100);
            ret = rk_aiq_uapi2_setANRStrth(sys_ctx, level);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set ANR strength failed ret=%d level=%u", ret, level);
                failures++;
            }
        }

        if (controls->dehaze_mode >= 0)
        {
            ret = rk_aiq_uapi2_setDhzMode(sys_ctx, (opMode_t)controls->dehaze_mode);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set dehaze mode failed ret=%d mode=%d",
                          ret,
                          controls->dehaze_mode);
                failures++;
            }
        }

        if (controls->dehaze_strength >= 0)
        {
            unsigned int level = (unsigned int)clamp_int(controls->dehaze_strength, 0, 10);
            ret = rk_aiq_uapi2_setMDhzStrth(sys_ctx, level);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP_CTRL] set dehaze strength failed ret=%d level=%u", ret, level);
                failures++;
            }
        }

        if (failures > 0)
        {
            LOG_ERROR("[ISP_CTRL] apply finished with %d failed controls", failures);
            return -1;
        }


        /* 成功后缓存本次批次，供状态查询和日志判断控制是否已生效。 */
        if (ctx->status_lock_ready)
            pthread_mutex_lock(&ctx->status_lock);
        ctx->last_controls = applied;
        ctx->controls_applied = 1;
        if (ctx->status_lock_ready)
            pthread_mutex_unlock(&ctx->status_lock);

        LOG_WARN("[ISP_CTRL] runtime image controls applied");
        return 0;
    }
#endif
}

#ifdef ENABLE_RKAIQ

/**
 * @brief RKAIQ 系统错误回调。
 *
 * 当前阶段只记录错误码，不主动重启 ISP。后续故障诊断可以在这里接入
 * 错误分类、自恢复和诊断上报。
 */
static XCamReturn isp_error_cb(rk_aiq_err_msg_t *err_msg)
{
    if (err_msg)
    {
        LOG_ERROR("[ISP] rkaiq error code=%d", err_msg->err_code);
        if (g_active_isp_ctx && g_active_isp_ctx->status_lock_ready)
        {
            pthread_mutex_lock(&g_active_isp_ctx->status_lock);
            g_active_isp_ctx->error_count++;
            g_active_isp_ctx->last_error_code = err_msg->err_code;
            g_active_isp_ctx->last_error_ts_us = get_now_us();
            pthread_mutex_unlock(&g_active_isp_ctx->status_lock);
        }
    }
    return XCAM_RETURN_NO_ERROR;
}


/**
 * @brief RKAIQ 元数据回调。
 *
 * 当前只记录回调计数、最近帧号和时间戳，用于健康检查判断 3A/ISP
 * pipeline 是否持续产出帧级状态。
 */
static XCamReturn isp_metas_cb(rk_aiq_metas_t *metas)
{
    if (g_active_isp_ctx && g_active_isp_ctx->status_lock_ready)
    {
        pthread_mutex_lock(&g_active_isp_ctx->status_lock);
        g_active_isp_ctx->meta_callback_count++;
        g_active_isp_ctx->last_meta_ts_us = get_now_us();
        if (metas)
            g_active_isp_ctx->meta_frame_id = metas->frame_id;
        pthread_mutex_unlock(&g_active_isp_ctx->status_lock);
    }
    return XCAM_RETURN_NO_ERROR;
}


/**
 * @brief 解析最终使用的 sensor media entity 名称。
 *
 * 优先使用配置中的 sensor_name；为空时通过 RKAIQ 根据 video_device
 * 反查绑定的 sensor entity。
 */
static const char *resolve_sensor_name(IspControllerCtx *ctx)
{
    const char *sensor_name = safe_config_str(ctx->config.sensor.sensor_name, "");
    const char *video_device = safe_config_str(ctx->config.sensor.video_device, "");
    const char *bound_sensor = NULL;

    LOG_WARN("[ISP] resolve sensor: config_sensor='%s' video_device='%s'",
             sensor_name,
             video_device);

    if (sensor_name[0] != '\0')
    {
        LOG_WARN("[ISP] resolve sensor: use configured sensor='%s'", sensor_name);
        return sensor_name;
    }

    if (video_device[0] == '\0')
    {
        LOG_ERROR("[ISP] resolve sensor failed: video_device is empty");
        return "";
    }


    /* 根据 video_device 查找绑定的 sensor entity，例如 m00_b_ov13850。 */
    bound_sensor = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(video_device);
    LOG_WARN("[ISP] resolve sensor: binded sensor by video='%s' is '%s'",
             video_device,
             bound_sensor ? bound_sensor : "NULL");
    if (!bound_sensor || bound_sensor[0] == '\0')
    {
        LOG_ERROR("[ISP] resolve sensor failed: no sensor bound to video_device='%s'", video_device);
        return "";
    }

    snprintf(ctx->resolved_sensor_name, sizeof(ctx->resolved_sensor_name), "%s", bound_sensor);
    LOG_WARN("[ISP] resolve sensor success: sensor='%s'", ctx->resolved_sensor_name);
    return ctx->resolved_sensor_name;
}
#endif


/**
 * @brief 初始化并启动 RKAIQ/ISP 控制链路。
 *
 * 真实 RKAIQ 路径依次执行 sensor 解析、可选 preInit、sysctl_init、
 * prepare、start。未编译 RKAIQ 时，根据 fallback_on_error 决定返回成功
 * 还是失败。
 */
int isp_controller_init(IspControllerCtx *ctx, const IspControllerConfig *config)
{
    IspControllerConfig cfg;

    if (!ctx)
    {
        LOG_ERROR("isp_controller_init failed: ctx is NULL");
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    if (pthread_mutex_init(&ctx->status_lock, NULL) != 0)
    {
        LOG_ERROR("isp_controller_init failed: pthread_mutex_init status_lock");
        return -1;
    }
    ctx->status_lock_ready = 1;

    if (!config || !config->lifecycle.enabled)
    {
        LOG_INFO("ISP disabled by config");
        return -1;
    }

    cfg = normalize_isp_config(config);
    ctx->config = cfg;

#ifndef ENABLE_RKAIQ
    LOG_WARN("[ISP] RKAIQ support is not compiled in; enable with -DENABLE_RKAIQ=ON");
    return isp_controller_should_fallback(&ctx->config) ? 0 : -1;
#else
    {
        const char *sensor_name = resolve_sensor_name(ctx);
        const char *raw_force_iq_file = safe_config_str(ctx->config.sensor.force_iq_file, "");
        XCamReturn ret;

        if (!sensor_name || sensor_name[0] == '\0')
        {
            LOG_ERROR("[ISP] failed to resolve sensor name from config sensor='%s' video='%s'",
                      safe_config_str(ctx->config.sensor.sensor_name, ""),
                      ctx->config.sensor.video_device);
            return isp_controller_should_fallback(&ctx->config) ? 0 : -1;
        }
        if (sensor_name != ctx->resolved_sensor_name)
            snprintf(ctx->resolved_sensor_name, sizeof(ctx->resolved_sensor_name), "%s", sensor_name);
        if (ctx->resolved_sensor_name[0] == '\0')
        {
            LOG_ERROR("[ISP] resolved sensor is empty after copy; abort RKAIQ init video='%s'",
                      ctx->config.sensor.video_device);
            return isp_controller_should_fallback(&ctx->config) ? 0 : -1;
        }

        LOG_WARN("[ISP] init RKAIQ sensor='%s' iq_dir='%s' size=%dx%d mode=%d force_iq='%s'",
                 ctx->resolved_sensor_name,
                 ctx->config.sensor.iq_dir,
                 ctx->config.sensor.width,
                 ctx->config.sensor.height,
                 ctx->config.sensor.working_mode,
                 raw_force_iq_file[0] ? raw_force_iq_file : "none");

        if (should_preinit_force_iq_file(raw_force_iq_file))
        {
            ret = rk_aiq_uapi2_sysctl_preInit(ctx->resolved_sensor_name,
                                              (rk_aiq_working_mode_t)ctx->config.sensor.working_mode,
                                              raw_force_iq_file);
            if (ret != XCAM_RETURN_NO_ERROR)
            {
                LOG_ERROR("[ISP] rk_aiq_uapi2_sysctl_preInit failed ret=%d sensor=%s iq=%s",
                          ret,
                          ctx->resolved_sensor_name,
                          raw_force_iq_file);
                return isp_controller_should_fallback(&ctx->config) ? 0 : -1;
            }
        }
        else if (raw_force_iq_file[0] != '\0')
        {
            LOG_WARN("[ISP] skip RKAIQ preInit force IQ '%s'; this SDK preInit expects XML, JSON IQ will be auto matched from iq_dir",
                     raw_force_iq_file);
        }

        ctx->sys_ctx = rk_aiq_uapi2_sysctl_init(ctx->resolved_sensor_name,
                                                ctx->config.sensor.iq_dir,
                                                isp_error_cb,
                                                isp_metas_cb);
        if (!ctx->sys_ctx)
        {
            LOG_ERROR("[ISP] rk_aiq_uapi2_sysctl_init failed sensor=%s iq_dir=%s",
                      ctx->resolved_sensor_name,
                      ctx->config.sensor.iq_dir);
            return isp_controller_should_fallback(&ctx->config) ? 0 : -1;
        }
        ctx->initialized = 1;
        g_active_isp_ctx = ctx;

        ret = rk_aiq_uapi2_sysctl_prepare((rk_aiq_sys_ctx_t *)ctx->sys_ctx,
                                          (uint32_t)ctx->config.sensor.width,
                                          (uint32_t)ctx->config.sensor.height,
                                          (rk_aiq_working_mode_t)ctx->config.sensor.working_mode);
        if (ret != XCAM_RETURN_NO_ERROR)
        {
            LOG_ERROR("[ISP] rk_aiq_uapi2_sysctl_prepare failed ret=%d sensor=%s size=%dx%d mode=%d",
                      ret,
                      ctx->resolved_sensor_name,
                      ctx->config.sensor.width,
                      ctx->config.sensor.height,
                      ctx->config.sensor.working_mode);
            isp_controller_deinit(ctx);
            return isp_controller_should_fallback(&cfg) ? 0 : -1;
        }
        ctx->prepared = 1;

        ret = rk_aiq_uapi2_sysctl_start((rk_aiq_sys_ctx_t *)ctx->sys_ctx);
        if (ret != XCAM_RETURN_NO_ERROR)
        {
            LOG_ERROR("[ISP] rk_aiq_uapi2_sysctl_start failed ret=%d sensor=%s",
                      ret,
                      ctx->resolved_sensor_name);
            isp_controller_deinit(ctx);
            return isp_controller_should_fallback(&cfg) ? 0 : -1;
        }
        ctx->started = 1;
        ctx->start_ts_us = get_now_us();
        LOG_WARN("[ISP] RKAIQ started sensor=%s", ctx->resolved_sensor_name);

        if (ctx->config.controls.enabled &&
            isp_controller_apply_image_controls(ctx, &ctx->config.controls) != 0)
        {
            LOG_ERROR("[ISP_CTRL] failed to apply initial runtime image controls");
            isp_controller_deinit(ctx);
            return isp_controller_should_fallback(&cfg) ? 0 : -1;
        }
    }
    return 0;
#endif
}


/**
 * @brief 停止并释放 RKAIQ/ISP 控制链路。
 *
 * 若 RKAIQ 已 started，先 stop；若已 initialized，再 deinit。
 * 函数末尾清空上下文，便于上层重复初始化。
 */
void isp_controller_deinit(IspControllerCtx *ctx)
{
    if (!ctx)
        return;

#ifdef ENABLE_RKAIQ
    if (g_active_isp_ctx == ctx)
        g_active_isp_ctx = NULL;
    if (ctx->started && ctx->sys_ctx)
    {
        XCamReturn ret = rk_aiq_uapi2_sysctl_stop((rk_aiq_sys_ctx_t *)ctx->sys_ctx,
                                                  ctx->config.lifecycle.keep_external_hw_state ? true : false);
        if (ret != XCAM_RETURN_NO_ERROR)
            LOG_WARN("[ISP] rk_aiq_uapi2_sysctl_stop failed ret=%d sensor=%s",
                     ret,
                     ctx->resolved_sensor_name);
        ctx->started = 0;
    }
    if (ctx->initialized && ctx->sys_ctx)
    {
        rk_aiq_uapi2_sysctl_deinit((rk_aiq_sys_ctx_t *)ctx->sys_ctx);
        ctx->sys_ctx = NULL;
        ctx->initialized = 0;
        ctx->prepared = 0;
        LOG_WARN("[ISP] RKAIQ deinit success sensor=%s", ctx->resolved_sensor_name);
    }
#endif
    if (ctx->status_lock_ready)
    {
        pthread_mutex_destroy(&ctx->status_lock);
        ctx->status_lock_ready = 0;
    }
    memset(ctx, 0, sizeof(*ctx));
}


/**
 * @brief 查询 RKAIQ/ISP 控制链路是否处于 started 状态。
 */
int isp_controller_is_started(const IspControllerCtx *ctx)
{
    return (ctx && ctx->started) ? 1 : 0;
}

#ifdef ENABLE_RKAIQ

/**
 * @brief 查询 AE 曝光状态并填充到统一状态结构。
 */
static void fill_ae_status(IspControllerCtx *ctx, IspControllerStatus *status)
{
    Uapi_ExpQueryInfo_t exp_info;
    XCamReturn ret;

    if (!ctx->sys_ctx || !ctx->started)
        return;

    memset(&exp_info, 0, sizeof(exp_info));
    ret = rk_aiq_user_api2_ae_queryExpResInfo((rk_aiq_sys_ctx_t *)ctx->sys_ctx, &exp_info);
    if (ret != XCAM_RETURN_NO_ERROR)
    {
        LOG_WARN("[ISP] query AE status failed ret=%d", ret);
        return;
    }

    status->ae.valid = 1;
    status->ae.converged = exp_info.IsConverged ? 1 : 0;
    status->ae.exp_max = exp_info.IsExpMax ? 1 : 0;
    status->ae.mean_luma = exp_info.MeanLuma;
    status->ae.luma_deviation = exp_info.LumaDeviation;
    status->ae.env_lux = exp_info.GlobalEnvLux;
    status->ae.integration_time = exp_info.CurExpInfo.LinearExp.exp_real_params.integration_time;
    status->ae.analog_gain = exp_info.CurExpInfo.LinearExp.exp_real_params.analog_gain;
    status->ae.digital_gain = exp_info.CurExpInfo.LinearExp.exp_real_params.digital_gain;
    status->ae.isp_dgain = exp_info.CurExpInfo.LinearExp.exp_real_params.isp_dgain;
    status->ae.iso = exp_info.CurExpInfo.LinearExp.exp_real_params.iso;
}


/**
 * @brief 查询 AWB 白平衡状态并填充到统一状态结构。
 */
static void fill_awb_status(IspControllerCtx *ctx, IspControllerStatus *status)
{
    rk_aiq_wb_querry_info_t wb_info;
    XCamReturn ret;

    if (!ctx->sys_ctx || !ctx->started)
        return;

    memset(&wb_info, 0, sizeof(wb_info));
    ret = rk_aiq_user_api2_awb_QueryWBInfo((rk_aiq_sys_ctx_t *)ctx->sys_ctx, &wb_info);
    if (ret != XCAM_RETURN_NO_ERROR)
    {
        LOG_WARN("[ISP] query AWB status failed ret=%d", ret);
        return;
    }

    status->awb.valid = 1;
    status->awb.converged = wb_info.awbConverged ? 1 : 0;
    status->awb.cct = wb_info.cctGloabl.CCT;
    status->awb.ccri = wb_info.cctGloabl.CCRI;
    status->awb.wb_rgain = wb_info.gain.rgain;
    status->awb.wb_grgain = wb_info.gain.grgain;
    status->awb.wb_gbgain = wb_info.gain.gbgain;
    status->awb.wb_bgain = wb_info.gain.bgain;
    status->awb.lv_value = wb_info.LVValue;
}
#endif

/**
 * @brief 查询当前 ISP/RKAIQ 运行状态。
 *
 * 返回生命周期状态、回调计数、健康诊断、低照度状态以及最近一次控制批次。
 * 启用 RKAIQ 编译时，还会尝试查询 AE/AWB 当前状态。
 */
int isp_controller_query_status(IspControllerCtx *ctx, IspControllerStatus *status)
{
    if (!ctx || !status)
        return -1;

    memset(status, 0, sizeof(*status));
    status->lifecycle.enabled = ctx->config.lifecycle.enabled ? 1 : 0;
    status->lifecycle.started = ctx->started ? 1 : 0;
    status->lifecycle.initialized = ctx->initialized ? 1 : 0;
    status->lifecycle.prepared = ctx->prepared ? 1 : 0;
    status->control.enabled = ctx->config.controls.enabled ? 1 : 0;
    snprintf(status->lifecycle.sensor_name, sizeof(status->lifecycle.sensor_name), "%s", ctx->resolved_sensor_name);

    if (ctx->status_lock_ready)
        pthread_mutex_lock(&ctx->status_lock);
    status->lifecycle.start_ts_us = ctx->start_ts_us;
    status->callbacks.meta_frame_id = ctx->meta_frame_id;
    status->callbacks.meta_callback_count = ctx->meta_callback_count;
    status->callbacks.error_count = ctx->error_count;
    status->callbacks.last_error_code = ctx->last_error_code;
    status->callbacks.last_meta_ts_us = ctx->last_meta_ts_us;
    status->callbacks.last_error_ts_us = ctx->last_error_ts_us;
    status->control.applied = ctx->controls_applied;
    status->control.controls = ctx->last_controls;
    status->low_light.active = ctx->low_light_active;
    snprintf(status->low_light.reason, sizeof(status->low_light.reason), "%s", ctx->low_light_reason);
    if (ctx->status_lock_ready)
        pthread_mutex_unlock(&ctx->status_lock);

    fill_health_status(ctx, status);
    status->low_light.enabled = ctx->config.low_light.enabled ? 1 : 0;
    status->low_light.bitrate_boost_percent = status->low_light.active
                                                ? ctx->config.low_light.bitrate_boost_percent
                                                : 0;
    status->low_light.qp_delta = status->low_light.active ? ctx->config.low_light.qp_delta : 0;

#ifdef ENABLE_RKAIQ
    fill_ae_status(ctx, status);
    fill_awb_status(ctx, status);
#endif
    return 0;
}
