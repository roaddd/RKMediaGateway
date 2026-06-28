/**
 * @file mediaGatewayDebug.c
 * @brief MediaGateway shell 调试命令实现。
 */

#include "mediaGatewayDebug.h"

#include "logger.h"
#include "shellCommandServer.h"

#include <inttypes.h>

/**
 * @description: 处理 getStatus 调试命令，输出 gateway 主状态。
 *
 * 该命令用于快速确认主循环、采集源、编码器、输出链路和吞吐窗口的当前值。
 * 读取过程不改变业务状态，吞吐统计通过 media_gateway_get_throughput 内部锁保护。
 */
static int gateway_shell_get_status(void *user_data, const char *input, char *output)
{
    MediaGatewayCtx *ctx = NULL;
    MediaGatewayThroughput throughput = {0};
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
    media_gateway_get_throughput(ctx, &throughput);

    shell_command_reply_append(reply, &offset, "cmd=getStatus\n");
    shell_command_reply_append(reply, &offset, "running=%d\n", ctx->running);
    shell_command_reply_append(reply, &offset, "isp_ready=%d\n", ctx->isp_ready);
    shell_command_reply_append(reply, &offset, "capture_source_count=%d\n", ctx->config.capture_source_count);
    shell_command_reply_append(reply, &offset, "stream_count=%d\n", ctx->config.stream_count);
    shell_command_reply_append(reply, &offset, "output_count=%d\n", ctx->output_count);
    shell_command_reply_append(reply, &offset, "audio_enabled=%d\n", ctx->config.audio.enabled);
    shell_command_reply_append(reply, &offset, "audio_capture_ready=%d\n", ctx->audio_capture_ready);
    shell_command_reply_append(reply, &offset, "audio_encoder_ready=%d\n", ctx->audio_encoder_ready);
    shell_command_reply_append(reply, &offset, "throughput_fps=%.2f\n", throughput.fps);
    shell_command_reply_append(reply, &offset, "throughput_bitrate_kbps=%.2f\n", throughput.bitrate_kbps);
    shell_command_reply_append(reply, &offset, "throughput_frames=%" PRIu64 "\n", throughput.frames);
    shell_command_reply_append(reply, &offset, "throughput_bytes=%" PRIu64 "\n", throughput.bytes);

    for (i = 0; i < MEDIA_GATEWAY_MAX_CAPTURE_SOURCES; ++i)
    {
        shell_command_reply_append(reply, &offset, "capture%d_ready=%d\n", i, ctx->capture_ready[i]);
    }
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        shell_command_reply_append(reply, &offset, "stream%d_enabled=%d\n", i, ctx->stream_enabled[i]);
        shell_command_reply_append(reply, &offset, "encoder%d_ready=%d\n", i, ctx->encoder_ready[i]);
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
    MediaGatewayDynamicFpsState *state = NULL;
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
    state = &ctx->dynamic_fps_state;
    reply = output;
    reply[0] = '\0';

    shell_command_reply_append(reply, &offset, "cmd=getFps\n");
    shell_command_reply_append(reply, &offset, "dynamic_fps_enabled=%d\n", ctx->config.dynamic_fps.enabled);
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
    shell_command_reply_append(reply, &offset, "isp_config_enabled=%d\n", ctx->config.isp.enabled);
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
 * @description: ?? gateway ?? shell ???
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
    if (regShellCmd("getIsp", gateway_shell_get_isp, ctx) != 0)
    {
        LOG_ERROR("media_gateway_debug_register_shell_commands failed: regShellCmd getIsp");
        return -1;
    }

    return 0;
}
