/**
 * @file mediaGatewayPolicy.c
 * @brief MediaGateway 运行期策略处理。
 *
 * 当前负责两类运行期策略：环境亮度调帧率和 RTCP/输出队列反馈调编码参数。
 * 两类策略分别生成约束后在本模块融合，具体硬件切换由 gateway 主循环
 * 异步投递到资源所属线程执行。
 */
#include "mediaGatewayPolicy.h"

#include "commonDef.h"
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
 * @brief 网络策略生成的每路中间动作。
 *
 * 该结构体只表达 RTCP/队列反馈判断出的网络等级动作系数，不直接代表最终下发值。
 * 最终编码码率需要在融合阶段结合基准码率、目标帧率和 bitrate_percent 计算；
 * 最终 RTP pacing 速率需要在融合阶段结合最终编码码率和 pacing_percent 计算。
 */
typedef struct {
    int bitrate_percent[MEDIA_GATEWAY_MAX_STREAMS]; /* 每路网络状态对应的码率系数，百分比。 */
    int pacing_percent[MEDIA_GATEWAY_MAX_STREAMS];  /* 每路网络状态对应的 pacing 系数，百分比。 */
} MediaAdaptNetworkDecision;

/**
 * @brief 初始化网络策略输出为中性约束。
 */
static void adaptive_init_network_decision(MediaAdaptNetworkDecision *decision)
{
    int stream_idx = 0;

    if (!decision)
        return;
    memset(decision, 0, sizeof(*decision));
    for (stream_idx = 0; stream_idx < MEDIA_GATEWAY_MAX_STREAMS; ++stream_idx)
    {
        decision->bitrate_percent[stream_idx] = 100;
        decision->pacing_percent[stream_idx] = 100;
    }
}

/**
 * @brief 将整数限制在给定范围内。
 */
static int clamp_int_policy(int value, int min_value, int max_value)
{
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

/**
 * @brief 返回联合控制场景状态名称，用于日志。
 */
static const char *adaptive_scene_name(MediaGatewaySceneState state)
{
    switch (state)
    {
    case MEDIA_GATEWAY_SCENE_LOW_LIGHT:
        return "LOW_LIGHT";
    case MEDIA_GATEWAY_SCENE_BRIGHT:
        return "BRIGHT";
    case MEDIA_GATEWAY_SCENE_NORMAL:
    default:
        return "NORMAL";
    }
}

/**
 * @brief 返回联合控制网络状态名称，用于日志。
 */
static const char *adaptive_network_name(MediaGatewayNetworkState state)
{
    switch (state)
    {
    case MEDIA_GATEWAY_NETWORK_NORMAL:
        return "NORMAL";
    case MEDIA_GATEWAY_NETWORK_BAD:
        return "BAD";
    case MEDIA_GATEWAY_NETWORK_VERY_BAD:
        return "VERY_BAD";
    case MEDIA_GATEWAY_NETWORK_GOOD:
    default:
        return "GOOD";
    }
}

/**
 * @brief 判断当前 RTCP/队列指标是否达到指定网络等级阈值。
 */
static int adaptive_network_detector_matched(const MediaGatewayNetworkDetectLevelConfig *detector,
                                             uint8_t fraction_lost,
                                             uint32_t rtt_ms,
                                             uint32_t jitter,
                                             int queue_depth)
{
    if (!detector)
        return 0;
    if (detector->min_fraction_lost > 0 && fraction_lost >= detector->min_fraction_lost)
        return 1;
    if (detector->min_rtt_ms > 0 && rtt_ms >= detector->min_rtt_ms)
        return 1;
    if (detector->min_jitter > 0 && jitter >= detector->min_jitter)
        return 1;
    if (detector->min_queue_depth > 0 && queue_depth >= detector->min_queue_depth)
        return 1;
    return 0;
}

/**
 * @brief 计算场景策略关闭时使用的全局帧率上限。
 *
 * 亮度感知调帧率关闭时，场景侧应该作为中性约束参与融合，不能再用某一路
 * stream.fps 限制网络反馈策略的结果。因此这里取配置中所有可能的最高帧率，
 * 作为 scene.max_fps 的兜底上限。
 */
static int adaptive_get_global_max_fps(MediaGatewayCtx *ctx)
{
    int max_fps = 0;
    int stream_idx = 0;

    if (!ctx)
        return 0;

    for (stream_idx = 0; stream_idx < ctx->config.video.stream_count &&
                         stream_idx < MEDIA_GATEWAY_MAX_STREAMS;
         ++stream_idx)
    {
        if (!ctx->config.video.streams[stream_idx].enabled)
            continue;
        if (ctx->config.video.streams[stream_idx].fps > max_fps)
            max_fps = ctx->config.video.streams[stream_idx].fps;
    }

    if (ctx->config.policy.light_fps.targets.bright_fps > max_fps)
        max_fps = ctx->config.policy.light_fps.targets.bright_fps;
    if (ctx->config.policy.network_encode.network.action.good.max_fps > max_fps)
        max_fps = ctx->config.policy.network_encode.network.action.good.max_fps;
    if (ctx->config.policy.network_encode.base_fps > max_fps)
        max_fps = ctx->config.policy.network_encode.base_fps;
    if (max_fps <= 0)
        max_fps = 30;

    return max_fps;
}

/**
 * @brief 根据 AE 已确认目标生成场景侧帧率上限。
 * 该函数只产出约束，不直接修改硬件。
 * @return MEDIA_OK 表示场景侧约束更新成功，错误码表示配置或状态非法。
 */
static int adaptive_update_scene_constraint(MediaGatewayCtx *ctx)
{
    MediaAdaptCtrlState *state = NULL;
    const MediaGatewayLightFpsPolicyConfig *fps_cfg = NULL;
    int scene_fps = 0;
    int global_max_fps = 0;

    if (!ctx)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] adaptive control is disabled or ctx is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    state = &ctx->adaptive_policy_state;
    fps_cfg = &ctx->config.policy.light_fps;
    global_max_fps = adaptive_get_global_max_fps(ctx);

    if (!fps_cfg->enabled)
    {
        /* 亮度调帧率关闭时，场景侧保持中性上限，不影响 RTCP/队列反馈的网络侧约束。 */
        state->scene.state = MEDIA_GATEWAY_SCENE_NORMAL;
        state->scene.max_fps = global_max_fps;
        return MEDIA_OK;
    }

    /* 亮度调帧率开启时，场景侧严格使用 AE 确认后的目标帧率参与融合。 */
    scene_fps = ctx->light_fps_state.target_fps;
    if (scene_fps <= 0)
        scene_fps = fps_cfg->targets.normal_fps;
    if (scene_fps <= 0)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] invalid scene_fps=%d target_fps=%d normal_fps=%d",
                  scene_fps,
                  ctx->light_fps_state.target_fps,
                  fps_cfg->targets.normal_fps);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    state->scene.max_fps = scene_fps;
    if (scene_fps == fps_cfg->targets.low_light_fps)
        state->scene.state = MEDIA_GATEWAY_SCENE_LOW_LIGHT;
    else if (scene_fps == fps_cfg->targets.bright_fps)
        state->scene.state = MEDIA_GATEWAY_SCENE_BRIGHT;
    else
        state->scene.state = MEDIA_GATEWAY_SCENE_NORMAL;
    if (state->scene.max_fps <= 0)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] invalid scene max_fps=%d global_max_fps=%d",
                  state->scene.max_fps,
                  global_max_fps);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    return MEDIA_OK;
}

/**
 * @brief 汇总指定码流所有输出通道的最大队列深度，作为该码流网络压力输入。
 */
static int adaptive_get_stream_output_queue_depth(MediaGatewayCtx *ctx, int stream_idx)
{
    MediaOutputStats output_stats = {0};
    int i = 0;
    int max_depth = 0;

    if (!ctx)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] adaptive control is disabled or ctx is NULL");
        return 0;
    }
    if (stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
        return 0;
    for (i = 0; i < ctx->output_count; ++i)
    {
        if (ctx->output_stream_index[i] != stream_idx)
            continue;
        memset(&output_stats, 0, sizeof(output_stats));
        media_output_get_stats(&ctx->outputs[i], &output_stats);
        if (output_stats.queue_depth > max_depth)
            max_depth = output_stats.queue_depth;
    }
    return max_depth;
}

/**
 * @brief 根据 RTCP RR 和输出队列压力生成网络侧约束。
 * @return 网络侧码率和 pacing 调整结果。
 */
static MediaAdaptNetworkDecision adaptive_update_network_constraint(MediaGatewayCtx *ctx)
{
    MediaAdaptCtrlState *state = NULL;
    const MediaGatewayNetworkAdaptiveConfig *network = NULL;
    MediaAdaptNetworkDecision decision;
    MediaGatewayStreamConfig *stream = NULL;
    MediaAdaptNetworkState *stream_network = NULL;
    uint8_t fraction_lost = 0;
    uint32_t rtt_ms = 0;
    uint32_t jitter = 0;
    int max_queue_depth = 0;
    int stream_idx = 0;
    int active_stream_count = 0;
    int aggregate_max_fps = 0;
    int aggregate_queue_depth = 0;
    uint8_t aggregate_fraction_lost = 0;
    uint32_t aggregate_rtt_ms = 0;
    uint32_t aggregate_jitter = 0;
    MediaGatewayNetworkState aggregate_state = MEDIA_GATEWAY_NETWORK_GOOD;

    adaptive_init_network_decision(&decision);
    if (!ctx)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] adaptive control is disabled or ctx is NULL");
        return decision;
    }

    state = &ctx->adaptive_policy_state;
    network = &ctx->config.policy.network_encode.network;
    aggregate_max_fps = network->action.good.max_fps;
    if (aggregate_max_fps <= 0)
        aggregate_max_fps = ctx->config.policy.network_encode.base_fps;

    for (stream_idx = 0; stream_idx < ctx->config.video.stream_count &&
                         stream_idx < MEDIA_GATEWAY_MAX_STREAMS;
         ++stream_idx)
    {
        stream = &ctx->config.video.streams[stream_idx];
        stream_network = &state->stream_network[stream_idx];
        if (!stream->enabled)
        {
            stream_network->state = MEDIA_GATEWAY_NETWORK_GOOD;
            stream_network->max_fps = aggregate_max_fps;
            stream_network->max_output_queue_depth = 0;
            continue;
        }

        /* 获取该码流对应 RTCP 反馈信息，RTSP 回调已按 session_name 写入 stream_network。 */
        if (ctx->stats_lock_ready)
            pthread_mutex_lock(&ctx->stats_lock);
        fraction_lost = stream_network->rtcp_fraction_lost;
        rtt_ms = stream_network->rtcp_rtt_ms;
        jitter = stream_network->rtcp_jitter;
        if (ctx->stats_lock_ready)
            pthread_mutex_unlock(&ctx->stats_lock);

        max_queue_depth = adaptive_get_stream_output_queue_depth(ctx, stream_idx);
        stream_network->max_output_queue_depth = max_queue_depth;

        /* 根据本码流 RTCP 丢包、RTT、jitter 和输出队列深度粗分网络等级。 */
        if (adaptive_network_detector_matched(&network->detector.very_bad,
                                              fraction_lost,
                                              rtt_ms,
                                              jitter,
                                              max_queue_depth))
        {
            stream_network->state = MEDIA_GATEWAY_NETWORK_VERY_BAD;
            stream_network->max_fps = network->action.very_bad.max_fps;
            decision.bitrate_percent[stream_idx] = network->action.very_bad.bitrate_percent;
            decision.pacing_percent[stream_idx] = network->action.very_bad.pacing_percent;
        }
        else if (adaptive_network_detector_matched(&network->detector.bad,
                                                   fraction_lost,
                                                   rtt_ms,
                                                   jitter,
                                                   max_queue_depth))
        {
            stream_network->state = MEDIA_GATEWAY_NETWORK_BAD;
            stream_network->max_fps = network->action.bad.max_fps;
            decision.bitrate_percent[stream_idx] = network->action.bad.bitrate_percent;
            decision.pacing_percent[stream_idx] = network->action.bad.pacing_percent;
        }
        else if (adaptive_network_detector_matched(&network->detector.normal,
                                                   fraction_lost,
                                                   rtt_ms,
                                                   jitter,
                                                   max_queue_depth))
        {
            stream_network->state = MEDIA_GATEWAY_NETWORK_NORMAL;
            stream_network->max_fps = network->action.normal.max_fps;
            decision.bitrate_percent[stream_idx] = network->action.normal.bitrate_percent;
            decision.pacing_percent[stream_idx] = network->action.normal.pacing_percent;
        }
        else
        {
            stream_network->state = MEDIA_GATEWAY_NETWORK_GOOD;
            stream_network->max_fps = network->action.good.max_fps;
            decision.bitrate_percent[stream_idx] = network->action.good.bitrate_percent;
            decision.pacing_percent[stream_idx] = network->action.good.pacing_percent;
        }

        /* 配置缺失时回退到基准帧率，避免网络侧约束生成不可用的 0 fps。 */
        if (stream_network->max_fps <= 0)
            stream_network->max_fps = ctx->config.policy.network_encode.base_fps;

        if (active_stream_count == 0 || stream_network->max_fps < aggregate_max_fps)
            aggregate_max_fps = stream_network->max_fps;
        if (stream_network->state > aggregate_state)
            aggregate_state = stream_network->state;
        if (stream_network->max_output_queue_depth > aggregate_queue_depth)
            aggregate_queue_depth = stream_network->max_output_queue_depth;
        if (fraction_lost > aggregate_fraction_lost)
            aggregate_fraction_lost = fraction_lost;
        if (rtt_ms > aggregate_rtt_ms)
            aggregate_rtt_ms = rtt_ms;
        if (jitter > aggregate_jitter)
            aggregate_jitter = jitter;
        active_stream_count++;
    }

    state->aggregate_network.state = aggregate_state;
    state->aggregate_network.max_fps = aggregate_max_fps;
    state->aggregate_network.max_output_queue_depth = aggregate_queue_depth;
    state->aggregate_network.rtcp_fraction_lost = aggregate_fraction_lost;
    state->aggregate_network.rtcp_rtt_ms = aggregate_rtt_ms;
    state->aggregate_network.rtcp_jitter = aggregate_jitter;
    if (active_stream_count <= 0)
        state->aggregate_network.max_fps = ctx->config.policy.network_encode.base_fps;
    if (state->aggregate_network.max_fps <= 0)
        state->aggregate_network.max_fps = ctx->config.policy.network_encode.base_fps;
    return decision;
}

/**
 * @brief 按最终帧率和关键帧时间间隔计算 GOP。
 * @return 大于 0 表示有效 GOP，-1 表示输入参数或关键帧间隔配置非法。
 */
static int adaptive_compute_gop(const MediaGatewayNetworkEncodePolicyConfig *cfg,
                                MediaGatewayNetworkState network_state,
                                int target_fps)
{
    const MediaGatewayNetworkLevelConfig *level = NULL;
    int interval_ms = 0;
    int gop = 0;

    if (!cfg)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] compute gop failed: cfg is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (target_fps <= 0)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] compute gop failed: invalid target_fps=%d", target_fps);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* 按网络状态选择对应的 GOP 时间间隔配置 */
    switch (network_state)
    {
    case MEDIA_GATEWAY_NETWORK_GOOD:
    case MEDIA_GATEWAY_NETWORK_NORMAL:
        level = &cfg->network.action.good;
        break;
    case MEDIA_GATEWAY_NETWORK_BAD:
        level = &cfg->network.action.bad;
        break;
    case MEDIA_GATEWAY_NETWORK_VERY_BAD:
        level = &cfg->network.action.very_bad;
        break;
    default:
        LOG_ERROR("[ADAPTIVE_CONTROL] compute gop failed: invalid network_state=%d", network_state);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* 关键帧间隔属于网络等级对应的编码输出约束 */
    interval_ms = level->keyframe_interval_ms;
    if (interval_ms <= 0)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] compute gop failed: invalid keyframe_interval_ms=%d state=%d",
                  interval_ms,
                  network_state);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    /* 根据目标帧率和关键帧间隔计算 GOP */
    gop = target_fps * interval_ms / 1000;
    if (gop <= 0)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] compute gop failed: target_fps=%d interval_ms=%d gop=%d",
                  target_fps,
                  interval_ms,
                  gop);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    return gop;
}

/**
 * @brief 根据最终 fps 和各路网络码率系数生成每路编码器目标运行参数。
 * @return MEDIA_OK 表示目标编码参数生成成功，错误码表示输入或配置非法。
 */
static int adaptive_build_target_params(MediaGatewayCtx *ctx, const MediaAdaptNetworkDecision *decision)
{
    MediaAdaptCtrlState *state = NULL;
    const MediaGatewayNetworkEncodePolicyConfig *cfg = NULL;
    MediaGatewayStreamConfig *stream = NULL;
    MediaVideoEncodeParams *base_params = NULL;
    MediaVideoEncodeParams *params = NULL;
    MediaAdaptNetworkState *stream_network = NULL;
    int stream_idx = 0;
    int base_bitrate = 0;
    int bitrate_percent = 100;
    int64_t bitrate = 0;
    int target_gop = 0;

    if (!ctx || !decision)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] adaptive control is disabled or ctx is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    state = &ctx->adaptive_policy_state;
    cfg = &ctx->config.policy.network_encode;
    if (cfg->base_fps <= 0)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] build target params failed: invalid base_fps=%d", cfg->base_fps);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    for (stream_idx = 0; stream_idx < ctx->config.video.stream_count && stream_idx < MEDIA_GATEWAY_MAX_STREAMS; ++stream_idx)
    {
        stream = &ctx->config.video.streams[stream_idx];
        base_params = &state->encode_params.base[stream_idx]; /* 每路编码器自适应计算基准参数 */
        params = &state->encode_params.target[stream_idx]; /* 根据RTCP网络反馈计算出来的编码器最终目标参数 */
        memset(params, 0, sizeof(*params));
        if (!stream->enabled) /* 跳过未启用的码流 */
            continue;
        stream_network = &state->stream_network[stream_idx]; /* 每路码流独立的网络反馈和网络侧约束状态 */
        /* 获取码率调整系数，这个系数是根据网络反馈决定的 */
        bitrate_percent = decision->bitrate_percent[stream_idx] > 0 ?  decision->bitrate_percent[stream_idx] : 100;
        /* 调整码率，先根据基准帧率对应的码率算出目标帧率对应的码率，然后再乘以码率调整系数，得到最终目标码率 */
        base_bitrate = base_params->bitrate > 0 ? base_params->bitrate : stream->bitrate;
        if (base_bitrate <= 0)
            base_bitrate = cfg->max_bitrate;
        if (base_bitrate <= 0)
        {
            LOG_ERROR("[ADAPTIVE_CONTROL] build target params failed: stream=%d invalid base_bitrate=%d max_bitrate=%d",
                      stream_idx,
                      base_bitrate,
                      cfg->max_bitrate);
            return MEDIA_ERR_INVALID_CONFIG;
        }
        bitrate = (int64_t)base_bitrate * state->output.target_fps / cfg->base_fps;
        bitrate = bitrate * bitrate_percent / 100;
        /* 调整帧率 */
        params->fps = state->output.target_fps;
        /* 调整码率 */
        params->bitrate = clamp_int_policy((int)bitrate, cfg->min_bitrate, cfg->max_bitrate);
        /* 调整 GOP */
        target_gop = adaptive_compute_gop(cfg, stream_network->state, state->output.target_fps);
        if (target_gop <= 0)
        {
            LOG_ERROR("[ADAPTIVE_CONTROL] build target params failed: stream=%d target_fps=%d network_state=%d",
                      stream_idx,
                      state->output.target_fps,
                      stream_network->state);
            return target_gop;
        }
        params->gop = target_gop;

        params->rc_mode = base_params->rc_mode;
        params->qp_init = base_params->qp_init;
        params->qp_min = base_params->qp_min;
        params->qp_max = base_params->qp_max;
        params->qp_min_i = base_params->qp_min_i;
        params->qp_max_i = base_params->qp_max_i;
        params->qp_max_step = base_params->qp_max_step;
    }
    return MEDIA_OK;
}

/**
 * @brief 更新 RTCP/输出队列反馈驱动的网络侧策略输出。
 *
 * 网络自适应关闭时生成中性网络约束，保证最终融合只受场景侧影响。
 */
static MediaAdaptNetworkDecision media_gateway_update_network_policy(MediaGatewayCtx *ctx)
{
    MediaAdaptCtrlState *state = NULL;
    MediaAdaptNetworkDecision network_decision;
    int stream_idx = 0;
    int global_max_fps = 0;

    adaptive_init_network_decision(&network_decision);
    if (!ctx)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] update network policy failed: ctx is NULL");
        return network_decision;
    }

    state = &ctx->adaptive_policy_state;
    global_max_fps = adaptive_get_global_max_fps(ctx);

    /* 根据是否开启了RTCP/队列反馈驱动的网络自适应编码参数控制来更新编码参数 */
    if (ctx->config.policy.network_encode.enabled)
    {
        network_decision = adaptive_update_network_constraint(ctx);
    }
    else
    {
        /* 未启用网络自适应时，网络侧保持全局帧率上限，不影响场景侧帧率结果。 */
        state->aggregate_network.state = MEDIA_GATEWAY_NETWORK_GOOD;
        state->aggregate_network.max_fps = global_max_fps;
        state->aggregate_network.max_output_queue_depth = 0;
        for (stream_idx = 0; stream_idx < MEDIA_GATEWAY_MAX_STREAMS; ++stream_idx)
        {
            state->stream_network[stream_idx].state = MEDIA_GATEWAY_NETWORK_GOOD;
            state->stream_network[stream_idx].max_fps = global_max_fps;
            state->stream_network[stream_idx].max_output_queue_depth = 0;
        }
    }
    return network_decision;
}

/**
 * @brief 联合控制输出：合并场景约束和网络约束，生成最终 fps、码率、GOP 和 pacing 目标。
 * @return MEDIA_OK 表示融合成功，错误码表示融合过程发现非法参数。
 */
static int media_gateway_fuse_runtime_policy_outputs(MediaGatewayCtx *ctx,
                                                     MediaAdaptNetworkDecision network_decision)
{
    MediaAdaptCtrlState *state = NULL;
    int old_target_fps = 0; /* 保存旧的帧率值 */
    int stream_idx = 0;
    int max_pacing_rate_bps = 0;
    int ret = MEDIA_OK;

    if (!ctx)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] fuse runtime policy failed: ctx is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* 如果场景和网络自适应都未启用，直接返回 */
    if (!ctx->config.policy.light_fps.enabled && !ctx->config.policy.network_encode.enabled)
        return MEDIA_OK;

    state = &ctx->adaptive_policy_state;
    old_target_fps = state->output.target_fps;

    /* 取网络反馈决定的帧率和亮度决定的帧率最小值作为最终帧率,此时肯定有一个是有效的 */
    state->output.target_fps = state->scene.max_fps < state->aggregate_network.max_fps ? state->scene.max_fps : state->aggregate_network.max_fps;
    if (state->output.target_fps <= 0)
    {
        /* 融合后的帧率小于等于 0 属于配置或策略计算错误，直接返回错误。 */
        LOG_ERROR("[ADAPTIVE_CONTROL] invalid fused target_fps=%d scene_fps=%d network_fps=%d base_fps=%d",
                  state->output.target_fps,
                  state->scene.max_fps,
                  state->aggregate_network.max_fps,
                  ctx->config.policy.network_encode.base_fps);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    /* 网络自适应开启时才生成并下发完整编码运行参数；否则保留原有 set_fps 路径。 */
    if (ctx->config.policy.network_encode.enabled)
    {
        ret = adaptive_build_target_params(ctx, &network_decision);
        if (ret != MEDIA_OK)
        {
            LOG_ERROR("[ADAPTIVE_CONTROL] fuse runtime policy failed: build target params ret=%d", ret);
            return ret;
        }
    }

    for (stream_idx = 0; stream_idx < MEDIA_GATEWAY_MAX_STREAMS; ++stream_idx)
        state->output.pacing_rate_bps[stream_idx] = 0;

    /*
     * pacing 是网络自适应的独立执行项。
     * 只有网络自适应和 pacing 开关同时开启时，最终输出状态才生成 pacing rate；
     * 任一开关关闭时保持 0，表示后续输出层需要显式关闭 RTP pacer。
     */
    if (ctx->config.policy.network_encode.enabled &&
        ctx->config.policy.network_encode.pacing_enabled)
    {
        for (stream_idx = 0; stream_idx < ctx->config.video.stream_count && stream_idx < MEDIA_GATEWAY_MAX_STREAMS; ++stream_idx)
        {
            if (!ctx->config.video.streams[stream_idx].enabled)
                continue;
            /* pacing rate 等于目标码率乘以 pacing_percent。 */
            state->output.pacing_rate_bps[stream_idx] =
                state->encode_params.target[stream_idx].bitrate *
                network_decision.pacing_percent[stream_idx] / 100;
            if (state->output.pacing_rate_bps[stream_idx] > max_pacing_rate_bps)
                max_pacing_rate_bps = state->output.pacing_rate_bps[stream_idx];
        }
    }

    snprintf(state->output.reason,
            sizeof(state->output.reason),
            "scene=%s network=%s scene_fps=%d network_fps=%d queue=%d loss=%u rtt=%u jitter=%u",
            adaptive_scene_name(state->scene.state),
            adaptive_network_name(state->aggregate_network.state),
            state->scene.max_fps,
            state->aggregate_network.max_fps,
            state->aggregate_network.max_output_queue_depth,
            state->aggregate_network.rtcp_fraction_lost,
            state->aggregate_network.rtcp_rtt_ms,
            state->aggregate_network.rtcp_jitter);

    state->output.last_decision_ts_us = policy_now_us();
    if (old_target_fps != state->output.target_fps)
    {
        LOG_WARN("[ADAPTIVE_CONTROL] target_fps %d->%d %s pacing=%d",
                 old_target_fps,
                 state->output.target_fps,
                 state->output.reason,
                 max_pacing_rate_bps); /* TODO；这里是不是要打印出所有路的pacing rate bps */
    }
    return MEDIA_OK;
}

/**
 * @brief 返回当前动态帧率场景名称，用于日志和状态说明。
 */
static const char *light_fps_scene_name(const MediaLightFpsState *state)
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
static int light_fps_ae_is_bright(const MediaGatewayLightFpsPolicyConfig *cfg, const IspControllerAeStatus *ae)
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
static int light_fps_ae_is_low_light(const MediaGatewayLightFpsPolicyConfig *cfg,
                                       const IspControllerAeStatus *ae,
                                       int reference_fps)
{
    float exposure_us = 0;
    float frame_period_us = 0;
    int exposure_near_limit = 0;

    if (!cfg || !ae || !ae->valid || reference_fps <= 0)
    {
        LOG_ERROR("[DYNAMIC_FPS] ae is invalid or reference_fps <= 0, skipping low_light check");
        return 0;
    }

    exposure_us = ae_exposure_us(ae); /* 将曝光时间转换为微秒 */
    frame_period_us = 1000000.0f / (float)reference_fps; /* 帧周期，单位微秒 */
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
 * @return 大于 0 表示确认时间，错误码表示输入参数非法。
 */
static int light_fps_confirm_ms(const MediaGatewayLightFpsPolicyConfig *cfg, int target_fps)
{
    if (!cfg)
    {
        LOG_ERROR("cfg is null");
        return MEDIA_ERR_INVALID_PARAM;
    }

    if (target_fps == cfg->targets.bright_fps)
        return cfg->timing.bright_confirm_ms;
    if (target_fps == cfg->targets.low_light_fps)
        return cfg->timing.low_light_confirm_ms;
    return cfg->timing.ae_scene_confirm_ms;
}

/**
 * @brief 更新环境亮度感知的帧率策略输出。
 *
 * 本函数只推进 AE/亮度侧状态机，并刷新 scene 约束，不读取 RTCP，也不做最终融合。
 * @return MEDIA_OK 表示亮度侧策略更新成功，错误码表示配置或状态非法。
 */
static int media_gateway_update_light_fps_policy(MediaGatewayCtx *ctx)
{
    const MediaGatewayLightFpsPolicyConfig *cfg = NULL;
    MediaLightFpsState *state = NULL;
    IspControllerStatus isp_status = {0};
    uint64_t now_us = 0;
    uint64_t confirm_us = 0;
    uint64_t elapsed_us = 0;
    int confirm_ms = 0;
    int target_fps = 0;
    int low_light_active = 0;
    int bright_active = 0;
    int ae_valid = 0;
    const char *reason = "normal";

    if (!ctx)
    {
        LOG_ERROR("[DYNAMIC_FPS] update light fps policy failed: ctx is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!ctx->config.policy.light_fps.enabled)
    {
        /* 环境亮度调帧率未启用时，场景侧只刷新中性上限，不推进 AE 状态机。 */
        goto out_update_scene;
    }

    cfg = &ctx->config.policy.light_fps;
    state = &ctx->light_fps_state;
    if (state->manual_override) /* TODO：当前是通过light_fps_state结构体中的manual_override字段控制的，可以直接改为ctx->config.policy.light_fps.enabled */
    {
        LOG_DEBUG("[DYNAMIC_FPS] manual override enabled, skipping AE policy update target=%d current=%d",
                  state->target_fps,
                  state->current_fps);
        goto out_update_scene;
    }

    now_us = policy_now_us();
    /* 限制策略评估频率，避免每帧查询 ISP 和重复触发切换判断。 */
    if (state->last_evaluate_ts_us > 0 &&
        now_us - state->last_evaluate_ts_us < (uint64_t)cfg->timing.evaluate_interval_ms * 1000ULL)
    {
        goto out_update_scene;
    }
    state->last_evaluate_ts_us = now_us;

    memset(&isp_status, 0, sizeof(isp_status));
    if (ctx->isp_ready && isp_controller_query_status(&ctx->isp, &isp_status) == 0)
    {
        ae_valid = isp_status.ae.valid;
        if (ae_valid)
        {
            /* 判断是否为低照度状态。 */
            /*
             * 低照判断使用 normal_fps 对应的帧周期作为参考基线。
             * 如果使用 current_fps，30fps 切到 15fps 后帧周期变长，同样的曝光时间会被误判为
             * 未接近曝光上限，导致 min_switch_interval_ms 到期后又切回 30fps。
             */
            low_light_active = light_fps_ae_is_low_light(cfg, &isp_status.ae, cfg->targets.normal_fps);
            /* 判断是否为亮光状态。 */
            bright_active = light_fps_ae_is_bright(cfg, &isp_status.ae);
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
        /* 场景仍处于确认期时，不提前提交新的场景 fps。 */
        goto out_update_scene;
    }
    else if (target_fps != state->target_fps)
    {
        /* 进入此分支时target_fps等于pending_fps，判断pending的fps等待时间是否满足时间 */
        confirm_ms = light_fps_confirm_ms(cfg, target_fps);
        if (confirm_ms <= 0)
        {
            LOG_ERROR("[DYNAMIC_FPS] invalid scene confirm_ms=%d target_fps=%d", confirm_ms, target_fps);
            return MEDIA_ERR_INVALID_CONFIG;
        }
        confirm_us = (uint64_t)confirm_ms * 1000ULL;
        if (now_us >= state->pending_since_ts_us &&
            now_us - state->pending_since_ts_us < confirm_us)
        {
            /* 等待 AE 场景确认期间，亮度侧继续沿用上一次已确认目标。 */
            goto out_update_scene;
        }
    }

    if (target_fps != state->target_fps)
    {
        elapsed_us = (state->last_switch_ts_us > 0 && now_us >= state->last_switch_ts_us)
                         ? (now_us - state->last_switch_ts_us)
                         : UINT64_MAX;
        /* 确认时间通过后，还要满足全局最小切换间隔，保护 pipeline 稳定性。 */
        if (elapsed_us < (uint64_t)cfg->timing.min_switch_interval_ms * 1000ULL)
        {
            /* 最小切换间隔内不接受新场景 fps，亮度侧继续沿用上一次已确认目标。 */
            goto out_update_scene;
        }

        {
            state->target_fps = target_fps;
            snprintf(state->reason,
                     sizeof(state->reason),
                     "%s scene=%s ae_valid=%d exp_us=%.2f analog_gain=%.2f mean_luma=%.2f exp_max=%d",
                     reason,
                     light_fps_scene_name(state),
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

out_update_scene:
    return adaptive_update_scene_constraint(ctx);
}

/**
 * @brief 按评估周期更新运行期策略。
 *
 * 运行期策略主流程：
 * 1. 调用亮度感知过程，得到场景侧帧率约束；
 * 2. 调用 RTCP/输出队列反馈过程，得到网络侧帧率、码率和 pacing 约束；
 * 3. 统一融合两侧结果，生成最终帧率和编码运行参数；
 * 4. 具体硬件切换交给 gateway 主循环异步事务处理。
 */
void media_gateway_refresh_adaptive_policy_targets_if_due(MediaGatewayCtx *ctx)
{
    MediaAdaptNetworkDecision network_decision;

    adaptive_init_network_decision(&network_decision);
    if (!ctx)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] ctx is null, skipping runtime policy update");
        return;
    }
    if (!ctx->config.policy.light_fps.enabled && !ctx->config.policy.network_encode.enabled)
        return;

    /* 第一步：亮度感知过程，只更新场景侧约束。场景侧无效时停止本轮融合。 */
    if (media_gateway_update_light_fps_policy(ctx) != MEDIA_OK)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] update scene constraint failed, skipping runtime policy fusion");
        return;
    }

    /* 第二步：RTCP/队列反馈过程，只更新网络侧约束。 */
    network_decision = media_gateway_update_network_policy(ctx);

    /* 第三步：融合两侧控制输出，硬件切换仍由 gateway 主循环异步投递。 */
    if (media_gateway_fuse_runtime_policy_outputs(ctx, network_decision) != MEDIA_OK)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] fuse runtime policy failed, skipping this round");
        return;
    }
}
