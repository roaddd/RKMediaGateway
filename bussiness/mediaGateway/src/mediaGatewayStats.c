#include "mediaGatewayStats.h"

#include "mediaGatewayClock.h"
#include "mediaGatewayMetrics.h"

#include "logger.h"

#include <inttypes.h>
#include <string.h>

/**
 * @description: 清空当前吞吐统计窗口。
 *
 * 只重置窗口内累计帧数和字节数，不修改 last_ts_us。
 * 调用方在完成日志输出后再更新时间戳，保证下一轮统计窗口
 * 从本次日志输出时刻开始计算。
 */
static void reset_throughput_window(MediaGatewayCtx *ctx)
{
    int i;

    ctx->stats.frames = 0;
    ctx->stats.bytes = 0;
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        ctx->stats.stream_frames[i] = 0;
        ctx->stats.stream_bytes[i] = 0;
    }
    ctx->stats.audio_frames = 0;
    ctx->stats.audio_bytes = 0;
}

/**
 * @description: 打印所有输出通道的连接、队列和发送统计。
 *
 * 输出通道包括 RTSP、RTMP、GB28181 等协议实现。
 * 该日志用于判断编码端是否正常产出、下游协议队列是否堆积、
 * 是否丢帧、是否等待关键帧或正在重连。
 */
static void log_output_stats(MediaGatewayCtx *ctx)
{
    int i;

    for (i = 0; i < ctx->output_count; ++i)
    {
        MediaOutputStats stats;
        media_output_get_stats(&ctx->outputs[i], &stats);
        LOG_INFO("[OUTPUT] idx=%d stream=%d name=%s type=%d connected=%d queue=%d sent=%" PRIu64
                 " bytes=%" PRIu64 " dropped=%" PRIu64 " send_failures=%" PRIu64
                 " reconnects=%" PRIu64 " wait_key=%d",
                 i,
                 ctx->output_stream_index[i],
                 ctx->outputs[i].config.name ? ctx->outputs[i].config.name : "unknown",
                 ctx->outputs[i].type,
                 stats.connected,
                 stats.queue_depth,
                 stats.sent_frames,
                 stats.sent_bytes,
                 stats.dropped_frames,
                 stats.send_failures,
                 stats.reconnect_count,
                 stats.waiting_for_keyframe);
    }
}

/**
 * @description: 打印 ISP/RKAIQ 运行状态和 3A 观测值。
 *
 * 当前用于低照度基线测试，只查询并打印 ISP、AE、AWB 状态，
 * 不主动修改 ISP 参数，也不触发低照度策略。
 */
static void log_isp_status(MediaGatewayCtx *ctx)
{
    IspControllerStatus status;
    const char *health_name = "unknown";

    if (!ctx->config.isp.enabled)
        return;

    if (isp_controller_query_status(&ctx->isp, &status) != 0)
    {
        LOG_WARN("[ISP_STAT] query failed");
        return;
    }

    switch (status.health.state)
    {
    case ISP_CONTROLLER_HEALTH_DISABLED:
        health_name = "disabled";
        break;
    case ISP_CONTROLLER_HEALTH_OK:
        health_name = "ok";
        break;
    case ISP_CONTROLLER_HEALTH_NOT_STARTED:
        health_name = "not_started";
        break;
    case ISP_CONTROLLER_HEALTH_META_STALLED:
        health_name = "meta_stalled";
        break;
    case ISP_CONTROLLER_HEALTH_ERROR_LIMIT:
        health_name = "error_limit";
        break;
    default:
        break;
    }

    LOG_WARN("[ISP_STAT] enabled=%d started=%d sensor=%s health=%s controls_enabled=%d controls_applied=%d"
             " meta_frame=%" PRIu64 " meta_callbacks=%" PRIu64 " meta_stall_ms=%" PRIu64
             " errors=%" PRIu64 " last_error=%d uptime_ms=%" PRIu64,
             status.lifecycle.enabled,
             status.lifecycle.started,
             status.lifecycle.sensor_name[0] ? status.lifecycle.sensor_name : "unknown",
             health_name,
             status.control.enabled,
             status.control.applied,
             status.callbacks.meta_frame_id,
             status.callbacks.meta_callback_count,
             status.health.meta_stall_us / 1000ULL,
             status.callbacks.error_count,
             status.callbacks.last_error_code,
             status.lifecycle.uptime_us / 1000ULL);

    if (status.health.state != ISP_CONTROLLER_HEALTH_DISABLED &&
        status.health.state != ISP_CONTROLLER_HEALTH_OK)
    {
        LOG_WARN("[ISP_HEALTH] state=%s reason=%s", health_name, status.health.reason);
    }
    if (status.low_light.enabled)
    {
        LOG_WARN("[ISP_LOWLIGHT] active=%d boost=%d%% qp_delta=%d reason=%s",
                 status.low_light.active,
                 status.low_light.bitrate_boost_percent,
                 status.low_light.qp_delta,
                 status.low_light.reason[0] ? status.low_light.reason : "none");
    }

    if (status.ae.valid)
    {
        LOG_WARN("[ISP_AE] converged=%d exp_max=%d mean_luma=%.2f luma_dev=%.2f env_lux=%.2f"
                 " int_time=%.6f analog_gain=%.3f digital_gain=%.3f isp_dgain=%.3f iso=%d",
                 status.ae.converged,
                 status.ae.exp_max,
                 status.ae.mean_luma,
                 status.ae.luma_deviation,
                 status.ae.env_lux,
                 status.ae.integration_time,
                 status.ae.analog_gain,
                 status.ae.digital_gain,
                 status.ae.isp_dgain,
                 status.ae.iso);
    }
    else if (status.lifecycle.started)
    {
        LOG_WARN("[ISP_AE] status unavailable");
    }

    if (status.awb.valid)
    {
        LOG_WARN("[ISP_AWB] converged=%d cct=%.2f ccri=%.2f gain=(%.3f,%.3f,%.3f,%.3f) lv=%u",
                 status.awb.converged,
                 status.awb.cct,
                 status.awb.ccri,
                 status.awb.wb_rgain,
                 status.awb.wb_grgain,
                 status.awb.wb_gbgain,
                 status.awb.wb_bgain,
                 status.awb.lv_value);
    }
    else if (status.lifecycle.started)
    {
        LOG_WARN("[ISP_AWB] status unavailable");
    }
}

/**
 * @description: 到达统计周期时输出吞吐、输出通道、ISP 和 benchmark 日志。
 *
 * 该函数在 gateway 主循环中持有 stats_lock 调用。它会根据
 * stats_interval_sec 判断是否到达统计周期；未到周期直接返回。
 * 到达周期后打印日志、重置窗口并刷新 last_ts_us，形成下一轮统计窗口。
 */
void media_gateway_log_throughput_if_due(MediaGatewayCtx *ctx)
{
    uint64_t now = media_gateway_get_now_us();
    uint64_t span_us = now - ctx->stats.last_ts_us;
    double span_sec;
    double fps;
    double kbps;
    int i;

    if (span_us < (uint64_t)ctx->config.stats_interval_sec * 1000000ULL)
        return;

    span_sec = (double)span_us / 1000000.0;
    fps = (span_sec > 0.0) ? ((double)ctx->stats.frames / span_sec) : 0.0;
    kbps = (span_sec > 0.0) ? ((double)ctx->stats.bytes * 8.0 / 1000.0 / span_sec) : 0.0;
    LOG_INFO("[STAT] total_fps=%.2f total_bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64,
             fps, kbps, ctx->stats.frames, ctx->stats.bytes);

    for (i = 0; i < ctx->config.stream_count; ++i)
    {
        double sfps;
        double skbps;
        if (!ctx->stream_enabled[i])
            continue;
        sfps = (span_sec > 0.0) ? ((double)ctx->stats.stream_frames[i] / span_sec) : 0.0;
        skbps = (span_sec > 0.0) ? ((double)ctx->stats.stream_bytes[i] * 8.0 / 1000.0 / span_sec) : 0.0;
        LOG_WARN("[STAT] stream=%d name=%s fps=%.2f bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64,
                 i,
                 ctx->config.streams[i].name ? ctx->config.streams[i].name : "unknown",
                 sfps,
                 skbps,
                 ctx->stats.stream_frames[i],
                 ctx->stats.stream_bytes[i]);
    }
    if (ctx->config.audio.enabled)
    {
        double afps = (span_sec > 0.0) ? ((double)ctx->stats.audio_frames / span_sec) : 0.0;
        double akbps = (span_sec > 0.0) ? ((double)ctx->stats.audio_bytes * 8.0 / 1000.0 / span_sec) : 0.0;
        LOG_WARN("[STAT] audio fps=%.2f bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64,
                 afps,
                 akbps,
                 ctx->stats.audio_frames,
                 ctx->stats.audio_bytes);
    }

    log_output_stats(ctx);
    log_isp_status(ctx);
    media_gateway_bench_log_and_reset_if_due(ctx);
    reset_throughput_window(ctx);
    ctx->stats.last_ts_us = now;
}

/**
 * @description: 读取当前统计窗口的吞吐估算值。
 *
 * 该接口不会重置统计窗口，只根据当前窗口累计帧数、字节数和
 * last_ts_us 计算即时 fps/bitrate，供外部状态查询或测试程序读取。
 */
void media_gateway_get_throughput(MediaGatewayCtx *ctx, MediaGatewayThroughput *throughput)
{
    uint64_t now;
    uint64_t span_us;

    if (!ctx || !throughput)
        return;

    memset(throughput, 0, sizeof(*throughput));
    if (ctx->stats_lock_ready)
        pthread_mutex_lock(&ctx->stats_lock);
    now = media_gateway_get_now_us();
    span_us = now - ctx->stats.last_ts_us;
    throughput->frames = ctx->stats.frames;
    throughput->bytes = ctx->stats.bytes;
    if (span_us > 0)
    {
        double span_sec = (double)span_us / 1000000.0;
        throughput->fps = (double)ctx->stats.frames / span_sec;
        throughput->bitrate_kbps = (double)ctx->stats.bytes * 8.0 / 1000.0 / span_sec;
    }
    if (ctx->stats_lock_ready)
        pthread_mutex_unlock(&ctx->stats_lock);
}
