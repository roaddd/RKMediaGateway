/**
 * @file mediaGatewayDebug.c
 * @brief MediaGateway shell 调试命令实现。
 */

#include "mediaGatewayDebug.h"

#include "logger.h"
#include "shellCommandServer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief 返回场景状态名称，便于 shell 输出阅读。
 */
static const char *gateway_debug_scene_name(MediaGatewaySceneState state)
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
 * @brief 返回网络状态名称，便于 shell 输出阅读。
 */
static const char *gateway_debug_network_name(MediaGatewayNetworkState state)
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
 * @brief 解析 shell 输入里的 0/1/on/off/enable/disable 开关值。
 */
static int gateway_debug_parse_enable_value(const char *value, int *enabled)
{
    if (!value || !enabled)
        return -1;
    if (strcmp(value, "1") == 0 ||
        strcmp(value, "on") == 0 ||
        strcmp(value, "enable") == 0 ||
        strcmp(value, "enabled") == 0)
    {
        *enabled = 1;
        return 0;
    }
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "disable") == 0 ||
        strcmp(value, "disabled") == 0)
    {
        *enabled = 0;
        return 0;
    }
    return -1;
}

/**
 * @brief 获取 debug 命令使用的单调时钟，单位微秒。
 */
static uint64_t gateway_debug_now_us(void)
{
    struct timespec ts = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 处理 getStatus 调试命令，输出 gateway 主状态。
 *
 * 该命令用于快速确认主循环、采集源、编码器、输出链路和吞吐窗口的当前值。
 * 读取过程不改变业务状态，码流统计通过 media_gateway_get_stats_snapshot 内部锁保护。
 */
static int gateway_shell_get_status(void *user_data, const char *input, char *output)
{
    MediaGatewayCtx *ctx = NULL;
    MediaGatewayStatsSnapshot stats_snapshot = {0};
    char *reply = NULL;
    size_t offset = 0;
    int i = 0;

    (void)input;
    if (!user_data || !output)
    {
        LOG_ERROR("gateway_shell_get_status failed: invalid argument user_data=%p output=%p",
                  user_data,
                  (void *)output);
        return -1;
    }

    ctx = (MediaGatewayCtx *)user_data;
    reply = output;
    reply[0] = '\0';
    media_gateway_get_stats_snapshot(ctx, &stats_snapshot);

    shell_command_reply_append(reply, &offset, "cmd=getStatus\n");
    shell_command_reply_append(reply, &offset, "running=%d\n", ctx->running);
    shell_command_reply_append(reply, &offset, "isp_ready=%d\n", ctx->isp_ready);
    shell_command_reply_append(reply, &offset, "capture_source_count=%d\n", ctx->config.input.capture_source_count);
    shell_command_reply_append(reply, &offset, "stream_count=%d\n", ctx->config.video.stream_count);
    shell_command_reply_append(reply, &offset, "output_count=%d\n", ctx->output_count);
    shell_command_reply_append(reply, &offset, "audio_enabled=%d\n", ctx->config.audio.source.enabled);
    shell_command_reply_append(reply, &offset, "audio_capture_ready=%d\n", ctx->audio_capture_ready);
    shell_command_reply_append(reply, &offset, "audio_encoder_ready=%d\n", ctx->audio_encoder_ready);
    for (i = 0; i < MEDIA_GATEWAY_MAX_CAPTURE_SOURCES; ++i)
    {
        shell_command_reply_append(reply, &offset, "capture%d_ready=%d\n", i, ctx->capture_ready[i]);
    }
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        shell_command_reply_append(reply, &offset, "stream%d_enabled=%d\n", i, ctx->stream_enabled[i]);
        shell_command_reply_append(reply, &offset, "encoder%d_ready=%d\n", i, ctx->encoder_ready[i]);
        shell_command_reply_append(reply, &offset, "stream%d_fps=%.2f\n", i, stats_snapshot.streams[i].fps);
        shell_command_reply_append(reply, &offset, "stream%d_bytes=%" PRIu64 "\n", i, stats_snapshot.streams[i].bytes);
    }

    return 0;
}

/**
 * @description: 处理 getFps 调试命令，输出动态帧率策略状态。
 *
 * 该命令重点观察“目标帧率、当前已提交成功帧率、候选帧率、AE 场景状态”
 * 是否符合预期，便于排查异步帧率切换是否卡在等待 worker 回包阶段。
 */
static int gateway_shell_get_fps(void *user_data, const char *input, char *output)
{
    MediaGatewayCtx *ctx = NULL;
    MediaLightFpsState *state = NULL;
    char *reply = NULL;
    size_t offset = 0;

    (void)input;
    if (!user_data || !output)
    {
        LOG_ERROR("gateway_shell_get_fps failed: invalid argument user_data=%p output=%p",
                  user_data,
                  (void *)output);
        return -1;
    }

    ctx = (MediaGatewayCtx *)user_data;
    state = &ctx->light_fps_state;
    reply = output;
    reply[0] = '\0';

    shell_command_reply_append(reply, &offset, "cmd=getFps\n");
    shell_command_reply_append(reply, &offset, "light_fps_enabled=%d\n", ctx->config.policy.light_fps.enabled);
    shell_command_reply_append(reply, &offset, "manual_override=%d\n", state->manual_override);
    shell_command_reply_append(reply, &offset, "current_fps=%d\n", state->current_fps);
    shell_command_reply_append(reply, &offset, "target_fps=%d\n", state->target_fps);
    shell_command_reply_append(reply, &offset, "pending_fps=%d\n", state->pending_fps);
    shell_command_reply_append(reply, &offset, "low_light_active=%d\n", state->low_light_active);
    shell_command_reply_append(reply, &offset, "bright_active=%d\n", state->bright_active);
    shell_command_reply_append(reply, &offset, "last_evaluate_ms=%" PRIu64 "\n", state->last_evaluate_ts_us / 1000ULL);
    shell_command_reply_append(reply, &offset, "last_switch_ms=%" PRIu64 "\n", state->last_switch_ts_us / 1000ULL);
    shell_command_reply_append(reply, &offset, "reason=%s\n", state->reason);

    return 0;
}

/**
 * @description: 处理 setFps 调试命令，手动固定或恢复自动动态帧率。
 *
 * 用法：
 * setFps 15/30/60  手动固定目标帧率，复用 gateway 的 Sensor -> Encoder 异步切换链路。
 * setFps auto      取消手动覆盖，恢复 AE 自动动态帧率策略。
 */
static int gateway_shell_set_fps(void *user_data, const char *input, char *output)
{
    MediaGatewayCtx *ctx = NULL;
    MediaLightFpsState *state = NULL;
    char *reply = NULL;
    char *end = NULL;
    size_t offset = 0;
    long fps_value = 0;
    int target_fps = 0;

    if (!user_data || !output)
    {
        LOG_ERROR("gateway_shell_set_fps failed: invalid argument user_data=%p output=%p",
                  user_data,
                  (void *)output);
        return -1;
    }

    ctx = (MediaGatewayCtx *)user_data;
    state = &ctx->light_fps_state;
    reply = output;
    reply[0] = '\0';
    shell_command_reply_append(reply, &offset, "cmd=setFps\n");

    if (!input || input[0] == '\0')
    {
        LOG_ERROR("gateway_shell_set_fps failed: missing input");
        shell_command_reply_append(reply, &offset, "ret=-1\n");
        shell_command_reply_append(reply, &offset, "error=missing fps input\n");
        shell_command_reply_append(reply, &offset, "usage=setFps <15|30|60|auto>\n");
        return -1;
    }

    if (strcmp(input, "auto") == 0)
    {
        if (!state->manual_override)
        {
            LOG_WARN("[DYNAMIC_FPS] manual override already disabled by shell current=%d target=%d",
                     state->current_fps,
                     state->target_fps);
        }
        state->manual_override = 0;
        state->pending_fps = 0;
        state->pending_since_ts_us = 0;
        snprintf(state->reason, sizeof(state->reason), "%s", "manual override disabled");
        LOG_WARN("[DYNAMIC_FPS] manual override disabled by shell current=%d target=%d",
                 state->current_fps,
                 state->target_fps);
        shell_command_reply_append(reply, &offset, "ret=0\n");
        shell_command_reply_append(reply, &offset, "manual_override=0\n");
        shell_command_reply_append(reply, &offset, "current_fps=%d\n", state->current_fps);
        shell_command_reply_append(reply, &offset, "target_fps=%d\n", state->target_fps);
        return 0;
    }

    errno = 0;
    fps_value = strtol(input, &end, 10);
    if (errno != 0 || end == input || (end && end[0] != '\0'))
    {
        LOG_ERROR("gateway_shell_set_fps failed: invalid fps input='%s' errno=%d",
                  input,
                  errno);
        shell_command_reply_append(reply, &offset, "ret=-1\n");
        shell_command_reply_append(reply, &offset, "error=invalid fps input\n");
        shell_command_reply_append(reply, &offset, "usage=setFps <15|30|60|auto>\n");
        return -1;
    }

    if (fps_value != 15 && fps_value != 30 && fps_value != 60)
    {
        LOG_ERROR("gateway_shell_set_fps failed: unsupported fps=%ld", fps_value);
        shell_command_reply_append(reply, &offset, "ret=-1\n");
        shell_command_reply_append(reply, &offset, "error=unsupported fps %ld\n", fps_value);
        shell_command_reply_append(reply, &offset, "supported=15,30,60\n");
        return -1;
    }

    target_fps = (int)fps_value;
    if (!ctx->running)
    {
        LOG_ERROR("gateway_shell_set_fps failed: gateway not running target=%d current=%d",
                  target_fps,
                  state->current_fps);
        shell_command_reply_append(reply, &offset, "ret=-1\n");
        shell_command_reply_append(reply, &offset, "error=gateway not running\n");
        return -1;
    }
    if (!ctx->capture_ready[0])
    {
        LOG_ERROR("gateway_shell_set_fps failed: capture source 0 not ready target=%d", target_fps);
        shell_command_reply_append(reply, &offset, "ret=-1\n");
        shell_command_reply_append(reply, &offset, "error=capture source 0 not ready\n");
        return -1;
    }
    if (!ctx->config.policy.light_fps.enabled)
    {
        LOG_ERROR("gateway_shell_set_fps failed: light_fps disabled, async fps transition is not driven target=%d",
                  target_fps);
        shell_command_reply_append(reply, &offset, "ret=-1\n");
        shell_command_reply_append(reply, &offset, "error=light_fps disabled\n");
        shell_command_reply_append(reply, &offset, "hint=enable light_fps or extend transition driver for manual mode\n");
        return -1;
    }

    if (target_fps == state->current_fps)
    {
        LOG_WARN("[DYNAMIC_FPS] manual override target equals current fps current=%d target=%d",
                 state->current_fps,
                 target_fps);
    }

    state->manual_override = 1;
    state->target_fps = target_fps;
    state->pending_fps = target_fps;
    state->pending_since_ts_us = 0;
    snprintf(state->reason, sizeof(state->reason), "manual shell target fps=%d", target_fps);
    LOG_WARN("[DYNAMIC_FPS] manual override target set by shell current=%d target=%d",
             state->current_fps,
             state->target_fps);

    shell_command_reply_append(reply, &offset, "ret=0\n");
    shell_command_reply_append(reply, &offset, "manual_override=1\n");
    shell_command_reply_append(reply, &offset, "current_fps=%d\n", state->current_fps);
    shell_command_reply_append(reply, &offset, "target_fps=%d\n", state->target_fps);
    shell_command_reply_append(reply, &offset, "note=async transition will be executed by gateway main loop\n");
    return 0;
}

/**
 * @description: 处理 getIsp 调试命令，输出 ISP/RKAIQ 聚合状态。
 *
 * 该命令通过 isp_controller_query_status 获取 ISP 模块内部快照，避免直接读
 * ISP 控制器的线程共享字段。若 ISP 未启用或未初始化，则只返回 gateway 标志位。
 */
static int gateway_shell_get_isp(void *user_data, const char *input, char *output)
{
    MediaGatewayCtx *ctx = NULL;
    IspControllerStatus status = {0};
    char *reply = NULL;
    size_t offset = 0;
    int ret = 0;

    (void)input;
    if (!user_data || !output)
    {
        LOG_ERROR("gateway_shell_get_isp failed: invalid argument user_data=%p output=%p",
                  user_data,
                  (void *)output);
        return -1;
    }

    ctx = (MediaGatewayCtx *)user_data;
    reply = output;
    reply[0] = '\0';
    shell_command_reply_append(reply, &offset, "cmd=getIsp\n");
    shell_command_reply_append(reply, &offset, "isp_config_enabled=%d\n", ctx->config.input.isp.enabled);
    shell_command_reply_append(reply, &offset, "isp_ready=%d\n", ctx->isp_ready);

    if (!ctx->isp_ready && !ctx->isp.initialized && !ctx->isp.status_lock_ready)
        return 0;

    ret = isp_controller_query_status(&ctx->isp, &status);
    if (ret != 0)
    {
        LOG_ERROR("gateway_shell_get_isp failed: isp_controller_query_status ret=%d", ret);
        shell_command_reply_append(reply, &offset, "query_status_ret=%d\n", ret);
        return -1;
    }

    shell_command_reply_append(reply, &offset, "started=%d\n", status.lifecycle.started);
    shell_command_reply_append(reply, &offset, "initialized=%d\n", status.lifecycle.initialized);
    shell_command_reply_append(reply, &offset, "prepared=%d\n", status.lifecycle.prepared);
    shell_command_reply_append(reply, &offset, "sensor_name=%s\n", status.lifecycle.sensor_name);
    shell_command_reply_append(reply, &offset, "uptime_ms=%" PRIu64 "\n", status.lifecycle.uptime_us / 1000ULL);
    shell_command_reply_append(reply, &offset, "health_enabled=%d\n", status.health.enabled);
    shell_command_reply_append(reply, &offset, "health_state=%d\n", status.health.state);
    shell_command_reply_append(reply, &offset, "health_reason=%s\n", status.health.reason);
    shell_command_reply_append(reply, &offset, "meta_callback_count=%" PRIu64 "\n", status.callbacks.meta_callback_count);
    shell_command_reply_append(reply, &offset, "meta_frame_id=%" PRIu64 "\n", status.callbacks.meta_frame_id);
    shell_command_reply_append(reply, &offset, "error_count=%" PRIu64 "\n", status.callbacks.error_count);
    shell_command_reply_append(reply, &offset, "last_error_code=%d\n", status.callbacks.last_error_code);
    shell_command_reply_append(reply, &offset, "ae_valid=%d\n", status.ae.valid);
    shell_command_reply_append(reply, &offset, "ae_mean_luma=%.2f\n", status.ae.mean_luma);
    shell_command_reply_append(reply, &offset, "ae_env_lux=%.2f\n", status.ae.env_lux);
    shell_command_reply_append(reply, &offset, "ae_integration_time=%.2f\n", status.ae.integration_time);
    shell_command_reply_append(reply, &offset, "ae_analog_gain=%.2f\n", status.ae.analog_gain);
    shell_command_reply_append(reply, &offset, "low_light_enabled=%d\n", status.low_light.enabled);
    shell_command_reply_append(reply, &offset, "low_light_active=%d\n", status.low_light.active);
    shell_command_reply_append(reply, &offset, "low_light_reason=%s\n", status.low_light.reason);

    return 0;
}

/**
 * @description: 处理 getAdapt 调试命令，输出亮度策略、网络策略和融合输出状态。
 *
 * 该命令用于确认 RTCP/队列反馈是否进入对应码流、每路编码参数目标值是否生成、
 * aggregate_network 是否按多路码流取到最保守的全局 fps 约束。
 */
static int gateway_shell_get_adapt(void *user_data, const char *input, char *output)
{
    MediaGatewayCtx *ctx = NULL;
    MediaAdaptCtrlState state = {0};
    MediaLightFpsState light_state = {0};
    MediaVideoEncodeParams *params = NULL;
    char *reply = NULL;
    size_t offset = 0;
    int stream_idx = 0;

    (void)input;
    if (!user_data || !output)
    {
        LOG_ERROR("gateway_shell_get_adapt failed: invalid argument user_data=%p output=%p",
                  user_data,
                  (void *)output);
        return -1;
    }

    ctx = (MediaGatewayCtx *)user_data;
    reply = output;
    reply[0] = '\0';
    if (ctx->stats_lock_ready)
        pthread_mutex_lock(&ctx->stats_lock);
    state = ctx->adaptive_policy_state;
    light_state = ctx->light_fps_state;
    if (ctx->stats_lock_ready)
        pthread_mutex_unlock(&ctx->stats_lock);

    shell_command_reply_append(reply, &offset, "cmd=getAdapt\n");
    shell_command_reply_append(reply, &offset, "light_fps_enabled=%d\n", ctx->config.policy.light_fps.enabled);
    shell_command_reply_append(reply, &offset, "network_encode_enabled=%d\n", ctx->config.policy.network_encode.enabled);
    shell_command_reply_append(reply, &offset, "manual_override=%d\n", light_state.manual_override);
    shell_command_reply_append(reply, &offset, "light_current_fps=%d\n", light_state.current_fps);
    shell_command_reply_append(reply, &offset, "light_target_fps=%d\n", light_state.target_fps);
    shell_command_reply_append(reply, &offset, "scene_state=%s\n", gateway_debug_scene_name(state.scene.state));
    shell_command_reply_append(reply, &offset, "scene_max_fps=%d\n", state.scene.max_fps);
    shell_command_reply_append(reply, &offset, "aggregate_network_state=%s\n", gateway_debug_network_name(state.aggregate_network.state));
    shell_command_reply_append(reply, &offset, "aggregate_network_max_fps=%d\n", state.aggregate_network.max_fps);
    shell_command_reply_append(reply, &offset, "aggregate_network_queue_depth=%d\n", state.aggregate_network.max_output_queue_depth);
    shell_command_reply_append(reply, &offset, "aggregate_network_loss=%u\n", state.aggregate_network.rtcp_fraction_lost);
    shell_command_reply_append(reply, &offset, "aggregate_network_rtt_ms=%u\n", state.aggregate_network.rtcp_rtt_ms);
    shell_command_reply_append(reply, &offset, "aggregate_network_jitter=%u\n", state.aggregate_network.rtcp_jitter);
    shell_command_reply_append(reply, &offset, "output_target_fps=%d\n", state.output.target_fps);
    shell_command_reply_append(reply, &offset, "output_last_decision_ms=%" PRIu64 "\n", state.output.last_decision_ts_us / 1000ULL);
    shell_command_reply_append(reply, &offset, "output_reason=%s\n", state.output.reason);

    for (stream_idx = 0; stream_idx < ctx->config.video.stream_count &&
                         stream_idx < MEDIA_GATEWAY_MAX_STREAMS;
         ++stream_idx)
    {
        params = &state.encode_params.target[stream_idx];
        shell_command_reply_append(reply, &offset, "stream%d_enabled=%d\n", stream_idx, ctx->config.video.streams[stream_idx].enabled);
        shell_command_reply_append(reply, &offset, "stream%d_network_state=%s\n", stream_idx, gateway_debug_network_name(state.stream_network[stream_idx].state));
        shell_command_reply_append(reply, &offset, "stream%d_network_max_fps=%d\n", stream_idx, state.stream_network[stream_idx].max_fps);
        shell_command_reply_append(reply, &offset, "stream%d_queue_depth=%d\n", stream_idx, state.stream_network[stream_idx].max_output_queue_depth);
        shell_command_reply_append(reply, &offset, "stream%d_rtcp_loss=%u\n", stream_idx, state.stream_network[stream_idx].rtcp_fraction_lost);
        shell_command_reply_append(reply, &offset, "stream%d_rtcp_rtt_ms=%u\n", stream_idx, state.stream_network[stream_idx].rtcp_rtt_ms);
        shell_command_reply_append(reply, &offset, "stream%d_rtcp_jitter=%u\n", stream_idx, state.stream_network[stream_idx].rtcp_jitter);
        shell_command_reply_append(reply, &offset, "stream%d_rtcp_last_ms=%" PRIu64 "\n", stream_idx, state.stream_network[stream_idx].last_rtcp_feedback_ts_us / 1000ULL);
        shell_command_reply_append(reply, &offset, "stream%d_target_fps=%d\n", stream_idx, params->fps);
        shell_command_reply_append(reply, &offset, "stream%d_target_bitrate=%d\n", stream_idx, params->bitrate);
        shell_command_reply_append(reply, &offset, "stream%d_target_gop=%d\n", stream_idx, params->gop);
        shell_command_reply_append(reply, &offset, "stream%d_pacing_rate_bps=%d\n", stream_idx, state.output.pacing_rate_bps[stream_idx]);
    }

    return 0;
}

/**
 * @description: 处理 setAdaptPolicy 调试命令，运行期打开或关闭亮度/网络两个子策略。
 *
 * 用法：
 * setAdaptPolicy light 0/1      开关环境亮度调帧率策略。
 * setAdaptPolicy network 0/1    开关 RTCP/队列反馈调编码参数策略。
 */
static int gateway_shell_set_adapt_policy(void *user_data, const char *input, char *output)
{
    MediaGatewayCtx *ctx = NULL;
    char policy[32] = {0};
    char value[32] = {0};
    char *reply = NULL;
    size_t offset = 0;
    int enabled = 0;
    int parsed = 0;

    if (!user_data || !output)
    {
        LOG_ERROR("gateway_shell_set_adapt_policy failed: invalid argument user_data=%p output=%p",
                  user_data,
                  (void *)output);
        return -1;
    }

    ctx = (MediaGatewayCtx *)user_data;
    reply = output;
    reply[0] = '\0';
    shell_command_reply_append(reply, &offset, "cmd=setAdaptPolicy\n");

    parsed = input ? sscanf(input, "%31s %31s", policy, value) : 0;
    if (parsed != 2 || gateway_debug_parse_enable_value(value, &enabled) != 0)
    {
        shell_command_reply_append(reply, &offset, "ret=-1\n");
        shell_command_reply_append(reply, &offset, "error=invalid input\n");
        shell_command_reply_append(reply, &offset, "usage=setAdaptPolicy <light|network> <0|1|on|off>\n");
        return -1;
    }

    if (strcmp(policy, "light") == 0 || strcmp(policy, "light_fps") == 0 || strcmp(policy, "scene") == 0)
    {
        ctx->config.policy.light_fps.enabled = enabled;
        shell_command_reply_append(reply, &offset, "ret=0\n");
        shell_command_reply_append(reply, &offset, "light_fps_enabled=%d\n", ctx->config.policy.light_fps.enabled);
        shell_command_reply_append(reply, &offset, "network_encode_enabled=%d\n", ctx->config.policy.network_encode.enabled);
        LOG_WARN("[ADAPTIVE_CONTROL] shell set light_fps enabled=%d", enabled);
        return 0;
    }
    if (strcmp(policy, "network") == 0 || strcmp(policy, "network_encode") == 0 || strcmp(policy, "rtcp") == 0)
    {
        ctx->config.policy.network_encode.enabled = enabled;
        shell_command_reply_append(reply, &offset, "ret=0\n");
        shell_command_reply_append(reply, &offset, "light_fps_enabled=%d\n", ctx->config.policy.light_fps.enabled);
        shell_command_reply_append(reply, &offset, "network_encode_enabled=%d\n", ctx->config.policy.network_encode.enabled);
        LOG_WARN("[ADAPTIVE_CONTROL] shell set network_encode enabled=%d", enabled);
        return 0;
    }

    shell_command_reply_append(reply, &offset, "ret=-1\n");
    shell_command_reply_append(reply, &offset, "error=unknown policy %s\n", policy);
    shell_command_reply_append(reply, &offset, "supported=light,network\n");
    return -1;
}

/**
 * @description: 处理 injectRtcp 调试命令，向指定码流注入模拟 RTCP 网络反馈。
 *
 * 用法：
 * injectRtcp <stream_idx> <fraction_lost> <rtt_ms> <jitter>
 */
static int gateway_shell_inject_rtcp(void *user_data, const char *input, char *output)
{
    MediaGatewayCtx *ctx = NULL;
    MediaAdaptNetworkState *network = NULL;
    char *reply = NULL;
    size_t offset = 0;
    int parsed = 0;
    int stream_idx = 0;
    int fraction_lost = 0;
    int rtt_ms = 0;
    int jitter = 0;

    if (!user_data || !output)
    {
        LOG_ERROR("gateway_shell_inject_rtcp failed: invalid argument user_data=%p output=%p",
                  user_data,
                  (void *)output);
        return -1;
    }

    ctx = (MediaGatewayCtx *)user_data;
    reply = output;
    reply[0] = '\0';
    shell_command_reply_append(reply, &offset, "cmd=injectRtcp\n");

    parsed = input ? sscanf(input, "%d %d %d %d", &stream_idx, &fraction_lost, &rtt_ms, &jitter) : 0;
    if (parsed != 4 ||
        stream_idx < 0 || stream_idx >= ctx->config.video.stream_count || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS ||
        fraction_lost < 0 || fraction_lost > 255 ||
        rtt_ms < 0 ||
        jitter < 0)
    {
        shell_command_reply_append(reply, &offset, "ret=-1\n");
        shell_command_reply_append(reply, &offset, "error=invalid input\n");
        shell_command_reply_append(reply, &offset, "usage=injectRtcp <stream_idx> <fraction_lost 0-255> <rtt_ms> <jitter>\n");
        return -1;
    }

    network = &ctx->adaptive_policy_state.stream_network[stream_idx];
    if (ctx->stats_lock_ready)
        pthread_mutex_lock(&ctx->stats_lock);
    network->rtcp_fraction_lost = (uint8_t)fraction_lost;
    network->rtcp_rtt_ms = (uint32_t)rtt_ms;
    network->rtcp_jitter = (uint32_t)jitter;
    network->last_rtcp_feedback_ts_us = gateway_debug_now_us();
    if (ctx->stats_lock_ready)
        pthread_mutex_unlock(&ctx->stats_lock);

    shell_command_reply_append(reply, &offset, "ret=0\n");
    shell_command_reply_append(reply, &offset, "stream_idx=%d\n", stream_idx);
    shell_command_reply_append(reply, &offset, "rtcp_fraction_lost=%d\n", fraction_lost);
    shell_command_reply_append(reply, &offset, "rtcp_rtt_ms=%d\n", rtt_ms);
    shell_command_reply_append(reply, &offset, "rtcp_jitter=%d\n", jitter);
    shell_command_reply_append(reply, &offset, "note=policy will consume injected values on next evaluation tick\n");
    LOG_WARN("[ADAPTIVE_CONTROL] shell inject rtcp stream=%d loss=%d rtt=%d jitter=%d",
             stream_idx,
             fraction_lost,
             rtt_ms,
             jitter);
    return 0;
}

/**
 * @description: 处理 clearRtcp 调试命令，清空指定码流或全部码流的 RTCP 指标。
 *
 * 用法：
 * clearRtcp <stream_idx|all>
 */
static int gateway_shell_clear_rtcp(void *user_data, const char *input, char *output)
{
    MediaGatewayCtx *ctx = NULL;
    MediaAdaptNetworkState *network = NULL;
    char target[32] = {0};
    char *end = NULL;
    char *reply = NULL;
    size_t offset = 0;
    long stream_value = 0;
    int stream_idx = 0;
    int begin_stream = 0;
    int end_stream = 0;

    if (!user_data || !output)
    {
        LOG_ERROR("gateway_shell_clear_rtcp failed: invalid argument user_data=%p output=%p",
                  user_data,
                  (void *)output);
        return -1;
    }

    ctx = (MediaGatewayCtx *)user_data;
    reply = output;
    reply[0] = '\0';
    shell_command_reply_append(reply, &offset, "cmd=clearRtcp\n");

    if (!input || sscanf(input, "%31s", target) != 1)
    {
        shell_command_reply_append(reply, &offset, "ret=-1\n");
        shell_command_reply_append(reply, &offset, "error=missing input\n");
        shell_command_reply_append(reply, &offset, "usage=clearRtcp <stream_idx|all>\n");
        return -1;
    }

    if (strcmp(target, "all") == 0)
    {
        begin_stream = 0;
        end_stream = ctx->config.video.stream_count;
    }
    else
    {
        errno = 0;
        stream_value = strtol(target, &end, 10);
        if (errno != 0 || end == target || (end && end[0] != '\0') ||
            stream_value < 0 || stream_value >= ctx->config.video.stream_count ||
            stream_value >= MEDIA_GATEWAY_MAX_STREAMS)
        {
            shell_command_reply_append(reply, &offset, "ret=-1\n");
            shell_command_reply_append(reply, &offset, "error=invalid stream index\n");
            shell_command_reply_append(reply, &offset, "usage=clearRtcp <stream_idx|all>\n");
            return -1;
        }
        begin_stream = (int)stream_value;
        end_stream = begin_stream + 1;
    }

    if (ctx->stats_lock_ready)
        pthread_mutex_lock(&ctx->stats_lock);
    for (stream_idx = begin_stream; stream_idx < end_stream && stream_idx < MEDIA_GATEWAY_MAX_STREAMS; ++stream_idx)
    {
        network = &ctx->adaptive_policy_state.stream_network[stream_idx];
        network->rtcp_fraction_lost = 0;
        network->rtcp_rtt_ms = 0;
        network->rtcp_jitter = 0;
        network->last_rtcp_feedback_ts_us = 0;
    }
    if (ctx->stats_lock_ready)
        pthread_mutex_unlock(&ctx->stats_lock);

    shell_command_reply_append(reply, &offset, "ret=0\n");
    shell_command_reply_append(reply, &offset, "begin_stream=%d\n", begin_stream);
    shell_command_reply_append(reply, &offset, "end_stream=%d\n", end_stream);
    shell_command_reply_append(reply, &offset, "note=policy will return to queue-depth-only network judgement on next evaluation tick\n");
    LOG_WARN("[ADAPTIVE_CONTROL] shell clear rtcp target=%s begin=%d end=%d",
             target,
             begin_stream,
             end_stream);
    return 0;
}

/**
 * @description: 注册 gateway 相关 shell 调试命令。
 */
int media_gateway_debug_register_shell_commands(MediaGatewayCtx *ctx)
{
    if (!ctx)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: ctx is NULL");
        return -1;
    }

    if (regShellCmd("getStatus", gateway_shell_get_status, ctx) != 0)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: regShellCmd getStatus");
        return -1;
    }
    if (regShellCmd("getFps", gateway_shell_get_fps, ctx) != 0)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: regShellCmd getFps");
        return -1;
    }
    if (regShellCmd("setFps", gateway_shell_set_fps, ctx) != 0)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: regShellCmd setFps");
        return -1;
    }
    if (regShellCmd("getIsp", gateway_shell_get_isp, ctx) != 0)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: regShellCmd getIsp");
        return -1;
    }
    if (regShellCmd("getAdapt", gateway_shell_get_adapt, ctx) != 0)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: regShellCmd getAdapt");
        return -1;
    }
    if (regShellCmd("setAdaptPolicy", gateway_shell_set_adapt_policy, ctx) != 0)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: regShellCmd setAdaptPolicy");
        return -1;
    }
    if (regShellCmd("injectRtcp", gateway_shell_inject_rtcp, ctx) != 0)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: regShellCmd injectRtcp");
        return -1;
    }
    if (regShellCmd("clearRtcp", gateway_shell_clear_rtcp, ctx) != 0)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: regShellCmd clearRtcp");
        return -1;
    }

    return 0;
}




