/**
 * @file mediaGateway.c
 * @brief MediaGateway 配置、模块生命周期和主循环调度实现。
 *
 * 主循环负责帧分发、运行期策略评估和异步控制事务协调，不直接执行
 * V4L2 或 MPP 运行期控制调用。硬件控制命令由资源所属 worker 串行执行。
 */

#include "mediaGateway.h"
#include "mediaGatewayClock.h"
#include "mediaGatewayMetrics.h"
#include "mediaGatewayPipeline.h"
#include "mediaGatewayPolicy.h"
#include "mediaGatewayProcess.h"
#include "mediaGatewayStats.h"
#include "mediaGatewayDebug.h"

#include "audioFrameSource.h"
#include "mediaControlMessage.h"
#include "mediaFrameSource.h"

#include "commonDef.h"

#include "logger.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


typedef enum {
    MEDIA_GATEWAY_FPS_TRANSITION_IDLE = 0,
    MEDIA_GATEWAY_FPS_TRANSITION_SENSOR_PENDING,
    MEDIA_GATEWAY_FPS_TRANSITION_ENCODERS_PENDING
} MediaGatewayVideoControlTransitionPhase;

typedef struct {
    MediaGatewayVideoControlTransitionPhase phase; /* 当前异步视频控制阶段。 */
    uint64_t next_request_id;             /* 下一个事务请求编号。 */
    uint64_t request_id;                  /* 当前事务请求编号。 */
    uint64_t retry_after_ts_us;           /* 失败后允许再次投递的时间。 */
    int target_fps;                       /* 当前事务目标帧率。 */
    uint32_t encoder_pending_mask;        /* 等待结果的编码器集合。 */
    uint32_t encoder_success_mask;        /* 已成功编码器集合。 */
    uint32_t encoder_failed_mask;         /* 待重试编码器集合。 */
} MediaGatewayVideoControlTransition;

typedef struct {
    MediaGatewayPipeline pipeline;                               /* 运行期音视频编码流水线。 */
    MediaFrameSource frame_sources[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 视频帧源线程对象。 */
    int frame_source_inited[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES];  /* 对应视频帧源是否已初始化。 */
    int frame_source_started[MEDIA_GATEWAY_MAX_CAPTURE_SOURCES]; /* 对应视频帧源线程是否已启动。 */
    AudioFrameSource audio_source;                               /* 音频帧源线程对象。 */
    int audio_source_inited;                                     /* 音频帧源是否已初始化。 */
    int audio_source_started;                                    /* 音频帧源线程是否已启动。 */
    ThreadMessageQueue result_queue;                              /* 所有 worker 共用的执行结果队列。 */
    int result_queue_inited;                                      /* 结果队列是否初始化成功。 */
    MediaGatewayVideoControlTransition video_control_transition;      /* 运行期视频控制异步事务。 */
} MediaGatewayRunResources;

/**
 * @brief 比较两个编码运行参数是否完全一致。
 * 统一控制器在投递编码参数前用该函数去重，避免重复下发相同运行参数。
 */
static int video_encode_params_equal(const MediaVideoEncodeParams *a,
                               const MediaVideoEncodeParams *b)
{
    if (!a || !b)
        return 0;
    return a->fps == b->fps &&
           a->bitrate == b->bitrate &&
           a->gop == b->gop &&
           a->rc_mode == b->rc_mode &&
           a->qp_init == b->qp_init &&
           a->qp_min == b->qp_min &&
           a->qp_max == b->qp_max &&
           a->qp_min_i == b->qp_min_i &&
           a->qp_max_i == b->qp_max_i &&
           a->qp_max_step == b->qp_max_step;
}

/**
 * @brief 判断统一控制器生成的目标编码运行参数是否尚未全部生效。
 * 该函数只比较 gateway 运行期配置快照，不直接访问 MPP。
 */
static int adaptive_target_params_need_apply(MediaGatewayCtx *ctx)
{
    int i = 0;
    MediaVideoEncodeParams current = {0};

    if (!ctx || !ctx->config.policy.network_encode.enabled)
        return 0;
    for (i = 0; i < ctx->config.video.stream_count && i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        if (!ctx->stream_enabled[i] || !ctx->encoder_ready[i])
            continue;
        memset(&current, 0, sizeof(current));
        current.fps = ctx->config.video.streams[i].fps;
        current.bitrate = ctx->config.video.streams[i].bitrate;
        current.gop = ctx->config.video.streams[i].gop;
        current.rc_mode = ctx->config.video.streams[i].rc_mode;
        current.qp_init = ctx->config.video.streams[i].qp_init;
        current.qp_min = ctx->config.video.streams[i].qp_min;
        current.qp_max = ctx->config.video.streams[i].qp_max;
        current.qp_min_i = ctx->config.video.streams[i].qp_min_i;
        current.qp_max_i = ctx->config.video.streams[i].qp_max_i;
        current.qp_max_step = ctx->config.video.streams[i].qp_max_step;
        if (!video_encode_params_equal(&current, &ctx->adaptive_policy_state.encode_params.target[i]))
            return 1;
    }
    return 0;
}

/**
 * @brief 获取当前应下发到硬件链路的最终目标帧率。
 * 亮度策略或网络策略任一开启后，都优先使用策略层融合后的最终目标。
 */
static int adaptive_effective_target_fps(MediaGatewayCtx *ctx)
{
    int runtime_policy_enabled = 0;

    if (!ctx)
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] get effective fps failed: ctx is NULL");
        return 0;
    }
    runtime_policy_enabled = ctx->config.policy.light_fps.enabled ||
                             ctx->config.policy.network_encode.enabled;
    if (!runtime_policy_enabled) /* 如果场景和网络反馈都没开启，则直接返回错误 */
    {
        LOG_ERROR("[ADAPTIVE_CONTROL] get effective fps failed: runtime policies are disabled");
        return 0;
    }
    if (ctx->adaptive_policy_state.output.target_fps > 0)
        return ctx->adaptive_policy_state.output.target_fps;
    LOG_ERROR("[ADAPTIVE_CONTROL] get effective fps failed: fused target_fps=%d",
              ctx->adaptive_policy_state.output.target_fps);
    return ctx->light_fps_state.target_fps;
}

/**
 * @brief 比较两个非空字符串是否相等。
 */
static int media_gateway_string_equal(const char *left, const char *right)
{
    if (!left || !right)
        return 0;
    if (left[0] == '\0' || right[0] == '\0')
        return 0;
    return strcmp(left, right) == 0;
}

/**
 * @brief 根据输出层上报的 sessionName/outputName 找到对应的码流下标。
 *
 * RTSP 反馈优先使用 session_name 匹配，因为主码流和子码流可能共用同一
 * RTSP server，但 URL session 必须不同；output_name 作为辅助校验，便于
 * 后续接入其他协议反馈时复用这条查找链路。
 */
static int media_gateway_find_feedback_stream_index(MediaGatewayCtx *ctx,
                                                    const MediaOutputNetFeedbackInfo *feedback)
{
    MediaGatewayStreamConfig *stream = NULL;
    MediaOutput *output = NULL;
    int stream_idx = 0;
    int output_idx = 0;
    int mapped_stream_idx = -1;
    int session_matched = 0;
    int output_matched = 0;
    int has_session_name = 0;

    if (!ctx || !feedback)
        return -1;
    has_session_name = feedback->session_name && feedback->session_name[0] != '\0';

    for (stream_idx = 0; stream_idx < ctx->config.video.stream_count &&
                         stream_idx < MEDIA_GATEWAY_MAX_STREAMS;
         ++stream_idx)
    {
        stream = &ctx->config.video.streams[stream_idx];
        if (!stream->enabled)
            continue;
        session_matched = media_gateway_string_equal(feedback->session_name,
                                                     stream->rtsp.session_name);
        if (session_matched)
            return stream_idx;
    }
    if (feedback->output_type == MEDIA_OUTPUT_TYPE_RTSP && has_session_name)
        return -1;

    for (output_idx = 0; output_idx < ctx->output_count &&
                         output_idx < MEDIA_GATEWAY_MAX_OUTPUTS;
         ++output_idx)
    {
        output = &ctx->outputs[output_idx];
        output_matched = media_gateway_string_equal(feedback->output_name,
                                                    output->config.name);
        if (!output_matched)
            continue;
        mapped_stream_idx = ctx->output_stream_index[output_idx];
        if (mapped_stream_idx >= 0 && mapped_stream_idx < MEDIA_GATEWAY_MAX_STREAMS)
            return mapped_stream_idx;
    }

    return -1;
}

/**
 * @brief 接收输出层上报的网络反馈。
 * RTSP output 将 RTCP RR 转换为统一反馈结构后调用本函数，gateway 按码流缓存指标。
 */
static void media_gateway_handle_output_network_feedback(const MediaOutputNetFeedbackInfo *feedback,
                                                         void *userdata)
{
    MediaGatewayCtx *ctx = (MediaGatewayCtx *)userdata;
    MediaAdaptNetworkState *network = NULL;
    int stream_idx = -1;

    if (!ctx || !feedback || feedback->is_audio)
        return;
    stream_idx = media_gateway_find_feedback_stream_index(ctx, feedback);
    if (stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_WARN("[ADAPTIVE_CONTROL] ignore network feedback: output=%s session=%s client=%s",
                 feedback->output_name ? feedback->output_name : "unknown",
                 feedback->session_name ? feedback->session_name : "unknown",
                 feedback->client_ip ? feedback->client_ip : "unknown");
        return;
    }

    network = &ctx->adaptive_policy_state.stream_network[stream_idx];
    if (ctx->stats_lock_ready)
        pthread_mutex_lock(&ctx->stats_lock);
    network->rtcp_fraction_lost = feedback->fraction_lost;
    network->rtcp_jitter = feedback->jitter;
    network->rtcp_rtt_ms = feedback->rtt_ms;
    network->last_rtcp_feedback_ts_us = media_gateway_get_now_us();
    if (ctx->stats_lock_ready)
        pthread_mutex_unlock(&ctx->stats_lock);
}

/**
 * @brief 将策略层生成的每路视频 RTP pacing 码率下发到输出层。
 *
 * mediaGateway 只负责计算每路 pacing_rate_bps；真正的 RTP 包级 pacer
 * 在 RTSP server 内部执行。非 RTSP 输出当前没有 RTP 包级执行点，输出层会忽略。
 */
static void media_gateway_apply_output_pacing_rates(MediaGatewayCtx *ctx)
{
    int output_idx = 0;
    int stream_idx = 0;
    int pacing_rate_bps = 0;

    if (!ctx)
        return;

    for (output_idx = 0; output_idx < ctx->output_count &&
                         output_idx < MEDIA_GATEWAY_MAX_OUTPUTS;
         ++output_idx)
    {
        stream_idx = ctx->output_stream_index[output_idx];
        if (stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
            continue;
        if (ctx->config.policy.network_encode.enabled)
            pacing_rate_bps = ctx->adaptive_policy_state.output.pacing_rate_bps[stream_idx];
        else
            pacing_rate_bps = 0;
        if (media_output_set_video_pacing_rate(&ctx->outputs[output_idx], pacing_rate_bps) != 0)
        {
            LOG_WARN("[ADAPTIVE_CONTROL] apply output pacing failed output=%s stream=%d rate=%d",
                     ctx->outputs[output_idx].config.name ? ctx->outputs[output_idx].config.name : "unknown",
                     stream_idx,
                     pacing_rate_bps);
        }
    }
}

/**
 * @description: 返回非空字符串；输入为空时返回 fallback。
 */
static const char *safe_str(const char *value, const char *fallback)
{
    return (value && value[0] != '\0') ? value : fallback;
}

static int is_empty_string(const char *value)
{
    return !value || value[0] == '\0';
}

static int config_error_string(const char *field)
{
    LOG_ERROR("media_gateway config invalid: %s is empty", field);
    return -1;
}

static int config_error_int(const char *field, int value, const char *rule)
{
    LOG_ERROR("media_gateway config invalid: %s=%d %s", field, value, rule);
    return -1;
}

static int require_non_empty_string(const char *field, const char *value)
{
    return is_empty_string(value) ? config_error_string(field) : 0;
}

static int require_positive_int(const char *field, int value)
{
    return (value <= 0) ? config_error_int(field, value, "must be > 0") : 0;
}

static int require_even_positive_int(const char *field, int value)
{
    if (value <= 0)
        return config_error_int(field, value, "must be > 0");
    if (value & 1)
        return config_error_int(field, value, "must be even");
    return 0;
}

/**
 * @description: 归一化 ISP 运行时图像控制配置。
 *
 * 当控制功能未启用时，将所有控制项设置为“不修改”哨兵值，避免 RKAIQ 启动后
 * 被默认配置意外改写亮度、曝光、白平衡、降噪等 IQ/3A 参数。控制功能启用时保留
 * 用户配置，由 ispController 在下发前按各字段范围进一步裁剪。
 */
static void fill_default_isp_controls(IspControllerImageControls *controls)
{
    if (!controls)
        return;

    controls->enabled = controls->enabled ? 1 : 0;
    if (!controls->enabled)
    {
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
    }
}

/**
 * @description: 归一化低照度自动优化配置。
 *
 * 该函数只补齐策略阈值和合法范围，不直接修改 ISP 或编码器状态。真正的低照度检测、
 * RKAIQ 参数下发和码率联动会在运行期统计周期中完成。
 */
static void fill_default_low_light_config(IspControllerLowLightConfig *cfg)
{
    if (!cfg)
        return;

    /*
     * 低照度策略默认关闭，避免未经过板端验证时自动改变曝光、降噪、锐度和码率。
     * 启用后使用 env_lux + mean_luma 双阈值判断，并通过 enter/exit 差值形成回滞。
     */
    cfg->enabled = cfg->enabled ? 1 : 0;
    if (cfg->enter_lux <= 0.0f)
        cfg->enter_lux = 20.0f;
    if (cfg->exit_lux <= cfg->enter_lux)
        cfg->exit_lux = 35.0f;
    if (cfg->enter_mean_luma <= 0.0f)
        cfg->enter_mean_luma = 35.0f;
    if (cfg->exit_mean_luma <= cfg->enter_mean_luma)
        cfg->exit_mean_luma = 48.0f;
    if (cfg->nr_strength < -1)
        cfg->nr_strength = -1;
    if (cfg->sharpness < -1)
        cfg->sharpness = -1;
    if (cfg->normal_nr_strength < -1)
        cfg->normal_nr_strength = -1;
    if (cfg->normal_sharpness < -1)
        cfg->normal_sharpness = -1;
    /*
     * 码率提升按百分比配置，MediaGatewayStats 会以每路原始配置码率为基准计算目标码率。
     * 这里只做合法性归一化，不直接改 stream bitrate。
     */
    if (cfg->bitrate_boost_percent < 0)
        cfg->bitrate_boost_percent = 0;
    if (cfg->qp_delta < -20)
        cfg->qp_delta = -20;
    if (cfg->qp_delta > 20)
        cfg->qp_delta = 20;
    if (cfg->min_switch_interval_ms <= 0)
        cfg->min_switch_interval_ms = 5000;
}

static int normalize_supported_light_fps(int fps, int fallback)
{
    if (fps == 15 || fps == 30 || fps == 60)
        return fps;
    return fallback;
}

static void fill_default_light_fps_policy_config(MediaGatewayLightFpsPolicyConfig *cfg)
{
    if (!cfg)
        return;

    cfg->enabled = cfg->enabled ? 1 : 0;
    cfg->targets.normal_fps = normalize_supported_light_fps(cfg->targets.normal_fps, DEFAULT_ENCODE_FPS);
    cfg->targets.low_light_fps = normalize_supported_light_fps(cfg->targets.low_light_fps, 15);
    cfg->targets.bright_fps = normalize_supported_light_fps(cfg->targets.bright_fps, 60);
    if (cfg->timing.min_switch_interval_ms <= 0)
        cfg->timing.min_switch_interval_ms = DEFAULT_DYNAMIC_FPS_MIN_SWITCH_INTERVAL_MS;
    if (cfg->timing.evaluate_interval_ms <= 0)
        cfg->timing.evaluate_interval_ms = DEFAULT_DYNAMIC_FPS_EVALUATE_INTERVAL_MS;
    if (cfg->timing.bright_confirm_ms <= 0)
        cfg->timing.bright_confirm_ms = DEFAULT_DYNAMIC_FPS_BRIGHT_CONFIRM_MS;
    if (cfg->timing.low_light_confirm_ms <= 0)
        cfg->timing.low_light_confirm_ms = DEFAULT_DYNAMIC_FPS_LOW_LIGHT_CONFIRM_MS;
    if (cfg->timing.ae_scene_confirm_ms <= 0)
        cfg->timing.ae_scene_confirm_ms = 3000;
    if (cfg->ae.bright_max_exposure_us <= 0.0f)
        cfg->ae.bright_max_exposure_us = 8000.0f;
    if (cfg->ae.bright_max_analog_gain <= 0.0f)
        cfg->ae.bright_max_analog_gain = 2.0f;
    if (cfg->ae.bright_min_mean_luma <= 0.0f)
        cfg->ae.bright_min_mean_luma = 58.0f;
    if (cfg->ae.low_light_min_exposure_ratio <= 0.0f)
        cfg->ae.low_light_min_exposure_ratio = 0.85f;
    if (cfg->ae.low_light_min_analog_gain <= 0.0f)
        cfg->ae.low_light_min_analog_gain = 4.0f;
    if (cfg->ae.low_light_max_mean_luma <= 0.0f)
        cfg->ae.low_light_max_mean_luma = 42.0f;
}

/**
 * @brief 填充网络自适应编码参数控制默认参数。
 * enabled 保持调用方配置值，便于独立测试亮度调帧率和 RTCP/队列反馈调编码参数。
 */
static void fill_default_network_encode_policy_config(MediaGatewayNetworkEncodePolicyConfig *cfg,
                                                  const MediaGatewayLightFpsPolicyConfig *light_fps)
{
    if (!cfg || !light_fps)
        return;

    cfg->enabled = cfg->enabled ? 1 : 0;
    if (cfg->base_fps <= 0)
        cfg->base_fps = light_fps->targets.normal_fps > 0 ? light_fps->targets.normal_fps : DEFAULT_ENCODE_FPS;
    if (cfg->min_bitrate <= 0)
        cfg->min_bitrate = DEFAULT_ADAPTIVE_MIN_BITRATE;
    if (cfg->max_bitrate <= 0)
        cfg->max_bitrate = DEFAULT_ADAPTIVE_MAX_BITRATE;
    if (cfg->good_keyframe_interval_ms <= 0)
        cfg->good_keyframe_interval_ms = 2000;
    if (cfg->bad_keyframe_interval_ms <= 0)
        cfg->bad_keyframe_interval_ms = 1500;
    if (cfg->very_bad_keyframe_interval_ms <= 0)
        cfg->very_bad_keyframe_interval_ms = 1000;
    if (cfg->network.good_max_fps <= 0)
        cfg->network.good_max_fps = light_fps->targets.bright_fps;
    if (cfg->network.normal_max_fps <= 0)
        cfg->network.normal_max_fps = light_fps->targets.normal_fps;
    if (cfg->network.bad_max_fps <= 0)
        cfg->network.bad_max_fps = 20;
    if (cfg->network.very_bad_max_fps <= 0)
        cfg->network.very_bad_max_fps = light_fps->targets.low_light_fps;
    if (cfg->network.normal_queue_depth <= 0)
        cfg->network.normal_queue_depth = 4;
    if (cfg->network.bad_queue_depth <= 0)
        cfg->network.bad_queue_depth = 8;
    if (cfg->network.very_bad_queue_depth <= 0)
        cfg->network.very_bad_queue_depth = 16;
    if (cfg->network.normal_bitrate_percent <= 0)
        cfg->network.normal_bitrate_percent = 85;
    if (cfg->network.bad_bitrate_percent <= 0)
        cfg->network.bad_bitrate_percent = 65;
    if (cfg->network.very_bad_bitrate_percent <= 0)
        cfg->network.very_bad_bitrate_percent = 45;
    if (cfg->network.good_pacing_percent <= 0)
        cfg->network.good_pacing_percent = 160;
    if (cfg->network.normal_pacing_percent <= 0)
        cfg->network.normal_pacing_percent = 140;
    if (cfg->network.bad_pacing_percent <= 0)
        cfg->network.bad_pacing_percent = 120;
    if (cfg->network.very_bad_pacing_percent <= 0)
        cfg->network.very_bad_pacing_percent = 100;
}

/**
 * @description: 获取单调时钟时间戳，单位微秒。
 */
uint64_t media_gateway_get_now_us(void)
{
    struct timespec ts = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 归一化一个视频采集源配置。
 */
static void fill_default_capture_source(CaptureSourceConfig *dst,
                                        const CaptureSourceConfig *src,
                                        int source_idx)
{
    CaptureSourceConfig src_copy = {0};
    int has_src = 0;

    if (src)
    {
        src_copy = *src;
        has_src = 1;
    }

    memset(dst, 0, sizeof(*dst));
    if (has_src)
        *dst = src_copy;

    dst->enabled = dst->enabled ? 1 : 0;
    dst->name = safe_str(dst->name, (source_idx == 0) ? "main_path" : "self_path");
    dst->device_path = safe_str(dst->device_path, (source_idx == 0) ? "/dev/video0" : "/dev/video1");
    if (dst->width <= 0)
        dst->width = (source_idx == 0) ? CAPTURE_WIDTH : 1280;
    if (dst->height <= 0)
        dst->height = (source_idx == 0) ? CAPTURE_HEIGHT : 720;
    if (dst->width & 1)
        dst->width -= 1;
    if (dst->height & 1)
        dst->height -= 1;
    if (dst->pixelformat == 0)
        dst->pixelformat = CAPTURE_FORMAT;
    if (dst->buffer_count <= 0)
        dst->buffer_count = V4L2_CAPTURE_BUFFER_COUNT;
    if (dst->buffer_count > V4L2_CAPTURE_BUFFER_COUNT)
        dst->buffer_count = V4L2_CAPTURE_BUFFER_COUNT;
}

/**
 * @description: 归一化一个码流配置及其协议输出子配置。
 */
static void fill_default_stream(MediaGatewayStreamConfig *dst,
                                const MediaGatewayStreamConfig *src,
                                int stream_idx)
{
    int default_width = (stream_idx == 0) ? CAPTURE_WIDTH : (CAPTURE_WIDTH / 2);
    int default_height = (stream_idx == 0) ? CAPTURE_HEIGHT : (CAPTURE_HEIGHT / 2);
    MediaGatewayStreamConfig src_copy = {0};
    int has_src = 0;

    if (src)
    {
        src_copy = *src;
        has_src = 1;
    }

    memset(dst, 0, sizeof(*dst));
    if (has_src)
        *dst = src_copy;

    dst->enabled = dst->enabled ? 1 : 0;
    dst->name = safe_str(dst->name, (stream_idx == 0) ? "main" : "sub");
    if (!has_src || dst->source_index < 0 || dst->source_index >= MEDIA_GATEWAY_MAX_CAPTURE_SOURCES)
        dst->source_index = stream_idx;
    if (dst->width <= 0)
        dst->width = default_width;
    if (dst->height <= 0)
        dst->height = default_height;
    if (dst->width & 1)
        dst->width -= 1;
    if (dst->height & 1)
        dst->height -= 1;
    if (dst->fps <= 0)
        dst->fps = DEFAULT_ENCODE_FPS;
    if (dst->bitrate <= 0)
        dst->bitrate = (stream_idx == 0) ? DEFAULT_ENCODE_BITRATE : (DEFAULT_ENCODE_BITRATE / 2);
    if (dst->gop <= 0)
        dst->gop = DEFAULT_ENCODE_GOP;
    if (!has_src || dst->rc_mode < 0 || dst->rc_mode >= MPP_ENC_RC_MODE_BUTT)
        dst->rc_mode = DEFAULT_RC_MODE;
    if (dst->h264_profile <= 0)
        dst->h264_profile = DEFAULT_H264_PROFILE;
    if (dst->h264_level <= 0)
        dst->h264_level = DEFAULT_H264_LEVEL;
    dst->h264_cabac_en = dst->h264_cabac_en ? 1 : 0;

    dst->rtsp.name = safe_str(dst->rtsp.name, (stream_idx == 0) ? "rtsp-main" : "rtsp-sub");
    dst->rtsp.session_name = safe_str(dst->rtsp.session_name, (stream_idx == 0) ? "live_main" : "live_sub");
    dst->rtsp.server_ip = safe_str(dst->rtsp.server_ip, "0.0.0.0");
    if (dst->rtsp.server_port <= 0)
        dst->rtsp.server_port = 8554;
    dst->rtsp.user = safe_str(dst->rtsp.user, "admin");
    dst->rtsp.password = safe_str(dst->rtsp.password, "123456");
    if (dst->rtsp.queue_capacity <= 0)
        dst->rtsp.queue_capacity = 32;
    if (dst->rtsp.immediate_sps_pps_on_new_client != 0)
        dst->rtsp.immediate_sps_pps_on_new_client = 1;

    dst->rtmp.name = safe_str(dst->rtmp.name, (stream_idx == 0) ? "rtmp-main" : "rtmp-sub");
    dst->rtmp.video_codec_name = safe_str(dst->rtmp.video_codec_name, "H264");
    dst->rtmp.encoder_name = safe_str(dst->rtmp.encoder_name, "RKMediaGateway");
    if (dst->rtmp.queue_capacity <= 0)
        dst->rtmp.queue_capacity = 64;
    if (dst->rtmp.reconnect_interval_ms <= 0)
        dst->rtmp.reconnect_interval_ms = 1000;
    if (dst->rtmp.connect_timeout_ms <= 0)
        dst->rtmp.connect_timeout_ms = 3000;
    if (dst->rtmp.video_width <= 0)
        dst->rtmp.video_width = dst->width;
    if (dst->rtmp.video_height <= 0)
        dst->rtmp.video_height = dst->height;
    if (dst->rtmp.video_fps <= 0)
        dst->rtmp.video_fps = dst->fps;
    if (dst->rtmp.video_bitrate <= 0)
        dst->rtmp.video_bitrate = dst->bitrate;

    dst->gb28181.name = safe_str(dst->gb28181.name, (stream_idx == 0) ? "gb28181-main" : "gb28181-sub");
    dst->gb28181.server_ip = safe_str(dst->gb28181.server_ip, "192.168.1.1");
    if (dst->gb28181.server_port <= 0)
        dst->gb28181.server_port = 5060;
    dst->gb28181.server_domain = safe_str(dst->gb28181.server_domain, "3402000000");
    dst->gb28181.server_id = safe_str(dst->gb28181.server_id, "34020000002000000001");
    dst->gb28181.device_id = safe_str(dst->gb28181.device_id, "34020000001320000001");
    dst->gb28181.device_domain = safe_str(dst->gb28181.device_domain, dst->gb28181.server_domain);
    dst->gb28181.device_password = safe_str(dst->gb28181.device_password, "12345678");
    dst->gb28181.bind_ip = safe_str(dst->gb28181.bind_ip, "0.0.0.0");
    if (dst->gb28181.local_sip_port <= 0)
        dst->gb28181.local_sip_port = 5060;
    dst->gb28181.sip_contact_ip = safe_str(dst->gb28181.sip_contact_ip, "127.0.0.1");
    dst->gb28181.media_ip = safe_str(dst->gb28181.media_ip, dst->gb28181.sip_contact_ip);
    if (dst->gb28181.media_port <= 0)
        dst->gb28181.media_port = 30000;
    if (dst->gb28181.register_expires <= 0)
        dst->gb28181.register_expires = 3600;
    if (dst->gb28181.keepalive_interval_sec <= 0)
        dst->gb28181.keepalive_interval_sec = 60;
    if (dst->gb28181.register_retry_interval_sec <= 0)
        dst->gb28181.register_retry_interval_sec = 5;
    dst->gb28181.device_name = safe_str(dst->gb28181.device_name, "RK3568 Camera");
    dst->gb28181.manufacturer = safe_str(dst->gb28181.manufacturer, "Topeet");
    dst->gb28181.model = safe_str(dst->gb28181.model, "RKMediaGateway");
    dst->gb28181.firmware = safe_str(dst->gb28181.firmware, "1.0.0");
    dst->gb28181.channel_id = safe_str(dst->gb28181.channel_id, dst->gb28181.device_id);
    dst->gb28181.user_agent = safe_str(dst->gb28181.user_agent, "RKMediaGateway-GB28181/1.0");
    if (dst->gb28181.queue_capacity <= 0)
        dst->gb28181.queue_capacity = 64;
}

/**
 * @description: 归一化 gateway 顶层配置和外部声明的码流配置。
 */
static int validate_raw_capture_source_config(const CaptureSourceConfig *cfg, int source_idx)
{
    if (!cfg || !cfg->enabled)
        return 0;

    if (require_non_empty_string("capture_sources[].name", cfg->name) != 0 ||
        require_non_empty_string("capture_sources[].device_path", cfg->device_path) != 0 ||
        require_even_positive_int("capture_sources[].width", cfg->width) != 0 ||
        require_even_positive_int("capture_sources[].height", cfg->height) != 0 ||
        require_positive_int("capture_sources[].buffer_count", cfg->buffer_count) != 0)
    {
        LOG_ERROR("media_gateway config invalid: capture source index=%d", source_idx);
        return -1;
    }
    if (cfg->pixelformat == 0)
    {
        LOG_ERROR("media_gateway config invalid: capture_sources[].pixelformat is 0 source=%d", source_idx);
        return -1;
    }
    if (cfg->buffer_count > V4L2_CAPTURE_BUFFER_COUNT)
    {
        LOG_ERROR("media_gateway config invalid: capture_sources[].buffer_count=%d exceeds max=%d source=%d",
                  cfg->buffer_count,
                  V4L2_CAPTURE_BUFFER_COUNT,
                  source_idx);
        return -1;
    }
    return 0;
}

static int validate_raw_rtsp_config(const MediaOutputRtspConfig *cfg, int stream_idx)
{
    if (require_non_empty_string("streams[].rtsp.name", cfg->name) != 0 ||
        require_non_empty_string("streams[].rtsp.session_name", cfg->session_name) != 0 ||
        require_non_empty_string("streams[].rtsp.server_ip", cfg->server_ip) != 0 ||
        require_positive_int("streams[].rtsp.server_port", cfg->server_port) != 0 ||
        require_positive_int("streams[].rtsp.queue_capacity", cfg->queue_capacity) != 0)
    {
        LOG_ERROR("media_gateway config invalid: RTSP stream=%d", stream_idx);
        return -1;
    }
    if (cfg->auth_enable &&
        (require_non_empty_string("streams[].rtsp.user", cfg->user) != 0 ||
         require_non_empty_string("streams[].rtsp.password", cfg->password) != 0))
    {
        LOG_ERROR("media_gateway config invalid: RTSP auth stream=%d", stream_idx);
        return -1;
    }
    return 0;
}

static int validate_raw_rtmp_config(const MediaOutputRtmpConfig *cfg, int stream_idx)
{
    if (require_non_empty_string("streams[].rtmp.name", cfg->name) != 0 ||
        require_non_empty_string("streams[].rtmp.publish_url", cfg->publish_url) != 0 ||
        require_positive_int("streams[].rtmp.queue_capacity", cfg->queue_capacity) != 0 ||
        require_positive_int("streams[].rtmp.reconnect_interval_ms", cfg->reconnect_interval_ms) != 0 ||
        require_positive_int("streams[].rtmp.connect_timeout_ms", cfg->connect_timeout_ms) != 0 ||
        require_positive_int("streams[].rtmp.video_width", cfg->video_width) != 0 ||
        require_positive_int("streams[].rtmp.video_height", cfg->video_height) != 0 ||
        require_positive_int("streams[].rtmp.video_fps", cfg->video_fps) != 0 ||
        require_positive_int("streams[].rtmp.video_bitrate", cfg->video_bitrate) != 0 ||
        require_non_empty_string("streams[].rtmp.video_codec_name", cfg->video_codec_name) != 0 ||
        require_non_empty_string("streams[].rtmp.encoder_name", cfg->encoder_name) != 0)
    {
        LOG_ERROR("media_gateway config invalid: RTMP stream=%d", stream_idx);
        return -1;
    }
    return 0;
}

static int validate_raw_gb28181_config(const MediaOutputGb28181Config *cfg, int stream_idx)
{
    if (require_non_empty_string("streams[].gb28181.name", cfg->name) != 0 ||
        require_non_empty_string("streams[].gb28181.server_ip", cfg->server_ip) != 0 ||
        require_positive_int("streams[].gb28181.server_port", cfg->server_port) != 0 ||
        require_non_empty_string("streams[].gb28181.server_domain", cfg->server_domain) != 0 ||
        require_non_empty_string("streams[].gb28181.server_id", cfg->server_id) != 0 ||
        require_non_empty_string("streams[].gb28181.device_id", cfg->device_id) != 0 ||
        require_non_empty_string("streams[].gb28181.device_domain", cfg->device_domain) != 0 ||
        require_non_empty_string("streams[].gb28181.device_password", cfg->device_password) != 0 ||
        require_non_empty_string("streams[].gb28181.bind_ip", cfg->bind_ip) != 0 ||
        require_positive_int("streams[].gb28181.local_sip_port", cfg->local_sip_port) != 0 ||
        require_non_empty_string("streams[].gb28181.sip_contact_ip", cfg->sip_contact_ip) != 0 ||
        require_non_empty_string("streams[].gb28181.media_ip", cfg->media_ip) != 0 ||
        require_positive_int("streams[].gb28181.media_port", cfg->media_port) != 0 ||
        require_positive_int("streams[].gb28181.register_expires", cfg->register_expires) != 0 ||
        require_positive_int("streams[].gb28181.keepalive_interval_sec", cfg->keepalive_interval_sec) != 0 ||
        require_positive_int("streams[].gb28181.register_retry_interval_sec", cfg->register_retry_interval_sec) != 0 ||
        require_non_empty_string("streams[].gb28181.device_name", cfg->device_name) != 0 ||
        require_non_empty_string("streams[].gb28181.manufacturer", cfg->manufacturer) != 0 ||
        require_non_empty_string("streams[].gb28181.model", cfg->model) != 0 ||
        require_non_empty_string("streams[].gb28181.firmware", cfg->firmware) != 0 ||
        require_non_empty_string("streams[].gb28181.channel_id", cfg->channel_id) != 0 ||
        require_non_empty_string("streams[].gb28181.user_agent", cfg->user_agent) != 0 ||
        require_positive_int("streams[].gb28181.queue_capacity", cfg->queue_capacity) != 0)
    {
        LOG_ERROR("media_gateway config invalid: GB28181 stream=%d", stream_idx);
        return -1;
    }
    return 0;
}

static int validate_raw_stream_config(const MediaGatewayStreamConfig *cfg,
                                      int stream_idx,
                                      int capture_source_count)
{
    if (!cfg || !cfg->enabled)
        return 0;

    if (require_non_empty_string("streams[].name", cfg->name) != 0 ||
        require_even_positive_int("streams[].width", cfg->width) != 0 ||
        require_even_positive_int("streams[].height", cfg->height) != 0 ||
        require_positive_int("streams[].fps", cfg->fps) != 0 ||
        require_positive_int("streams[].bitrate", cfg->bitrate) != 0 ||
        require_positive_int("streams[].gop", cfg->gop) != 0 ||
        require_positive_int("streams[].h264_profile", cfg->h264_profile) != 0 ||
        require_positive_int("streams[].h264_level", cfg->h264_level) != 0)
    {
        LOG_ERROR("media_gateway config invalid: stream=%d", stream_idx);
        return -1;
    }
    if (cfg->source_index < 0 || cfg->source_index >= capture_source_count)
    {
        LOG_ERROR("media_gateway config invalid: streams[].source_index=%d out of range stream=%d capture_source_count=%d",
                  cfg->source_index,
                  stream_idx,
                  capture_source_count);
        return -1;
    }
    if (cfg->rc_mode < 0 || cfg->rc_mode >= MPP_ENC_RC_MODE_BUTT)
    {
        LOG_ERROR("media_gateway config invalid: streams[].rc_mode=%d stream=%d", cfg->rc_mode, stream_idx);
        return -1;
    }
    if (cfg->enable_rtsp && validate_raw_rtsp_config(&cfg->rtsp, stream_idx) != 0)
        return -1;
    if (cfg->enable_rtmp && validate_raw_rtmp_config(&cfg->rtmp, stream_idx) != 0)
        return -1;
    if (cfg->enable_gb28181 && validate_raw_gb28181_config(&cfg->gb28181, stream_idx) != 0)
        return -1;
    return 0;
}

static int fill_default_config(MediaGatewayConfig *dst, const MediaGatewayConfig *src)
{
    int i = 0;
    int source_idx = 0;
    MediaGatewayConfig src_copy = {0};
    int has_src = 0;

    if (!src)
    {
        memset(dst, 0, sizeof(*dst));
        LOG_ERROR("media_gateway config invalid: config is NULL");
        return -1;
    }

    if (src)
    {
        src_copy = *src;
        has_src = 1;
    }
    memset(dst, 0, sizeof(*dst));
    if (has_src)
        *dst = src_copy;

    if (dst->system.runtime.stats_interval_sec <= 0)
        return config_error_int("runtime.stats_interval_sec", dst->system.runtime.stats_interval_sec, "must be > 0");
    if (dst->system.runtime.capture_retry_ms <= 0)
        return config_error_int("runtime.capture_retry_ms", dst->system.runtime.capture_retry_ms, "must be > 0");
    if (dst->system.runtime.max_consecutive_failures <= 0)
        return config_error_int("runtime.max_consecutive_failures", dst->system.runtime.max_consecutive_failures, "must be > 0");
    if (dst->system.record.file_path && dst->system.record.file_path[0] != '\0' && dst->system.record.flush_interval_frames <= 0)
        return config_error_int("record.flush_interval_frames", dst->system.record.flush_interval_frames, "must be > 0 when record is enabled");
    if (dst->system.bench.enabled)
    {
        if (dst->system.bench.sample_every <= 0)
            return config_error_int("bench.sample_every", dst->system.bench.sample_every, "must be > 0 when bench is enabled");
        if (dst->system.bench.print_interval_sec <= 0)
            return config_error_int("bench.print_interval_sec", dst->system.bench.print_interval_sec, "must be > 0 when bench is enabled");
    }
    if (dst->input.capture_source_count < 0 || dst->input.capture_source_count > MEDIA_GATEWAY_MAX_CAPTURE_SOURCES)
        return config_error_int("capture_source_count", dst->input.capture_source_count, "must be between 0 and MEDIA_GATEWAY_MAX_CAPTURE_SOURCES");
    if (dst->video.stream_count < 0 || dst->video.stream_count > MEDIA_GATEWAY_MAX_STREAMS)
        return config_error_int("stream_count", dst->video.stream_count, "must be between 0 and MEDIA_GATEWAY_MAX_STREAMS");
    if (dst->video.stream_count > 0 && dst->input.capture_source_count <= 0)
        return config_error_int("capture_source_count", dst->input.capture_source_count, "must be > 0 when streams are configured");
    for (i = 0; i < dst->video.stream_count; ++i)
    {
        if (validate_raw_stream_config(&dst->video.streams[i], i, dst->input.capture_source_count) != 0)
            return -1;
        if (dst->video.streams[i].enabled)
            dst->input.capture_sources[dst->video.streams[i].source_index].enabled = 1;
    }
    for (i = 0; i < dst->input.capture_source_count; ++i)
    {
        if (validate_raw_capture_source_config(&dst->input.capture_sources[i], i) != 0)
            return -1;
    }
    if (dst->input.isp.enabled)
    {
        if (require_non_empty_string("isp.video_device", dst->input.isp.video_device) != 0 ||
            require_non_empty_string("isp.iq_dir", dst->input.isp.iq_dir) != 0 ||
            require_positive_int("isp.width", dst->input.isp.width) != 0 ||
            require_positive_int("isp.height", dst->input.isp.height) != 0 ||
            require_positive_int("isp.meta_timeout_ms", dst->input.isp.meta_timeout_ms) != 0 ||
            require_positive_int("isp.max_error_count", dst->input.isp.max_error_count) != 0)
            return -1;
        if (dst->input.isp.working_mode < 0)
            return config_error_int("isp.working_mode", dst->input.isp.working_mode, "must be >= 0");
    }
    if (dst->input.isp.low_light.enabled)
    {
        if (dst->input.isp.low_light.enter_lux <= 0.0f ||
            dst->input.isp.low_light.exit_lux <= dst->input.isp.low_light.enter_lux ||
            dst->input.isp.low_light.enter_mean_luma <= 0.0f ||
            dst->input.isp.low_light.exit_mean_luma <= dst->input.isp.low_light.enter_mean_luma)
        {
            LOG_ERROR("media_gateway config invalid: isp.low_light thresholds are invalid");
            return -1;
        }
        if (dst->input.isp.low_light.min_switch_interval_ms <= 0)
            return config_error_int("isp.low_light.min_switch_interval_ms", dst->input.isp.low_light.min_switch_interval_ms, "must be > 0");
        if (dst->input.isp.low_light.bitrate_boost_percent < 0)
            return config_error_int("isp.low_light.bitrate_boost_percent", dst->input.isp.low_light.bitrate_boost_percent, "must be >= 0");
        if (dst->input.isp.low_light.qp_delta < -20 || dst->input.isp.low_light.qp_delta > 20)
            return config_error_int("isp.low_light.qp_delta", dst->input.isp.low_light.qp_delta, "must be between -20 and 20");
        if (dst->input.isp.low_light.nr_strength < -1 ||
            dst->input.isp.low_light.sharpness < -1 ||
            dst->input.isp.low_light.normal_nr_strength < -1 ||
            dst->input.isp.low_light.normal_sharpness < -1)
        {
            LOG_ERROR("media_gateway config invalid: isp.low_light image control values must be >= -1");
            return -1;
        }
    }
    if (dst->audio.source.enabled)
    {
        if (require_non_empty_string("audio.device_name", dst->audio.source.device_name) != 0 ||
            require_positive_int("audio.sample_rate", dst->audio.source.sample_rate) != 0 ||
            require_positive_int("audio.channels", dst->audio.source.channels) != 0 ||
            require_positive_int("audio.period_frames", dst->audio.source.period_frames) != 0 ||
            require_positive_int("audio.buffer_periods", dst->audio.source.buffer_periods) != 0 ||
            require_positive_int("audio.source_slots", dst->audio.source.source_slots) != 0 ||
            require_positive_int("audio.retry_ms", dst->audio.source.retry_ms) != 0 ||
            require_positive_int("audio.max_consecutive_failures", dst->audio.source.max_consecutive_failures) != 0)
            return -1;
        if (dst->audio.source.format != AUDIO_SAMPLE_FORMAT_S16LE)
            return config_error_int("audio.format", dst->audio.source.format, "is unsupported");
        if (dst->audio.source.codec != MEDIA_CODEC_G711A &&
            dst->audio.source.codec != MEDIA_CODEC_G711U &&
            dst->audio.source.codec != MEDIA_CODEC_AAC)
            return config_error_int("audio.codec", dst->audio.source.codec, "is unsupported");
        if (dst->audio.source.codec == MEDIA_CODEC_AAC)
        {
            if (require_positive_int("audio.aac_bitrate", dst->audio.source.aac_bitrate) != 0 ||
                require_positive_int("audio.aac_profile", dst->audio.source.aac_profile) != 0)
                return -1;
        }
        if (dst->audio.source.bind_stream_index < 0 || dst->audio.source.bind_stream_index >= dst->video.stream_count)
            return config_error_int("audio.bind_stream_index", dst->audio.source.bind_stream_index, "must reference a configured stream");
    }
    if (dst->policy.light_fps.enabled)
    {
        if (normalize_supported_light_fps(dst->policy.light_fps.targets.normal_fps, 0) != dst->policy.light_fps.targets.normal_fps ||
            normalize_supported_light_fps(dst->policy.light_fps.targets.low_light_fps, 0) != dst->policy.light_fps.targets.low_light_fps ||
            normalize_supported_light_fps(dst->policy.light_fps.targets.bright_fps, 0) != dst->policy.light_fps.targets.bright_fps)
        {
            LOG_ERROR("media_gateway config invalid: light_fps target fps must be one of 15/30/60");
            return -1;
        }
        if (dst->policy.light_fps.timing.min_switch_interval_ms <= 0 ||
            dst->policy.light_fps.timing.evaluate_interval_ms <= 0 ||
            dst->policy.light_fps.timing.bright_confirm_ms <= 0 ||
            dst->policy.light_fps.timing.low_light_confirm_ms <= 0 ||
            dst->policy.light_fps.timing.ae_scene_confirm_ms <= 0)
        {
            LOG_ERROR("media_gateway config invalid: light_fps timing values must be > 0");
            return -1;
        }
        if (dst->policy.light_fps.ae.bright_max_exposure_us <= 0.0f ||
            dst->policy.light_fps.ae.bright_max_analog_gain <= 0.0f ||
            dst->policy.light_fps.ae.bright_min_mean_luma <= 0.0f ||
            dst->policy.light_fps.ae.low_light_min_exposure_ratio <= 0.0f ||
            dst->policy.light_fps.ae.low_light_min_analog_gain <= 0.0f ||
            dst->policy.light_fps.ae.low_light_max_mean_luma <= 0.0f)
        {
            LOG_ERROR("media_gateway config invalid: light_fps AE values must be > 0");
            return -1;
        }
    }

    dst->output.switches.enable_rtsp = dst->output.switches.enable_rtsp ? 1 : 0;
    dst->output.switches.enable_rtmp = dst->output.switches.enable_rtmp ? 1 : 0;
    dst->output.switches.enable_gb28181 = dst->output.switches.enable_gb28181 ? 1 : 0;
    if (dst->video.encode.fps <= 0)
        dst->video.encode.fps = DEFAULT_ENCODE_FPS;
    if (dst->video.encode.bitrate <= 0)
        dst->video.encode.bitrate = DEFAULT_ENCODE_BITRATE;
    if (dst->video.encode.gop <= 0)
        dst->video.encode.gop = DEFAULT_ENCODE_GOP;
    if (!has_src || dst->video.encode.rc_mode < 0 || dst->video.encode.rc_mode >= MPP_ENC_RC_MODE_BUTT)
        dst->video.encode.rc_mode = DEFAULT_RC_MODE;
    if (dst->video.encode.h264_profile <= 0)
        dst->video.encode.h264_profile = DEFAULT_H264_PROFILE;
    if (dst->video.encode.h264_level <= 0)
        dst->video.encode.h264_level = DEFAULT_H264_LEVEL;
    dst->video.encode.h264_cabac_en = dst->video.encode.h264_cabac_en ? 1 : 0;
    dst->system.runtime.low_latency_mode = dst->system.runtime.low_latency_mode ? 1 : 0;
    if (dst->system.runtime.stats_interval_sec <= 0)
        dst->system.runtime.stats_interval_sec = DEFAULT_STATS_INTERVAL_SEC;
    if (dst->system.runtime.capture_retry_ms <= 0)
        dst->system.runtime.capture_retry_ms = DEFAULT_CAPTURE_RETRY_MS;
    if (dst->system.runtime.max_consecutive_failures <= 0)
        dst->system.runtime.max_consecutive_failures = DEFAULT_MAX_CONSECUTIVE_FAILURES;
    if (dst->system.record.flush_interval_frames <= 0)
        dst->system.record.flush_interval_frames = DEFAULT_RECORD_FLUSH_INTERVAL_FRAMES;
    dst->system.bench.enabled = dst->system.bench.enabled ? 1 : 0;
    if (dst->system.bench.sample_every <= 0)
        dst->system.bench.sample_every = DEFAULT_BENCH_SAMPLE_EVERY;
    if (dst->system.bench.print_interval_sec <= 0)
        dst->system.bench.print_interval_sec = DEFAULT_BENCH_PRINT_INTERVAL_SEC;
    fill_default_light_fps_policy_config(&dst->policy.light_fps);
    fill_default_network_encode_policy_config(&dst->policy.network_encode, &dst->policy.light_fps);

    dst->input.isp.enabled = dst->input.isp.enabled ? 1 : 0;
    dst->input.isp.video_device = safe_str(dst->input.isp.video_device, "/dev/video0");
    dst->input.isp.iq_dir = safe_str(dst->input.isp.iq_dir, "thirdparty/rkaiq");
    dst->input.isp.force_iq_file = safe_str(dst->input.isp.force_iq_file, "");
    dst->input.isp.sensor_name = safe_str(dst->input.isp.sensor_name, "");
    if (dst->input.isp.working_mode < 0)
        dst->input.isp.working_mode = 0;
    dst->input.isp.keep_external_hw_state = dst->input.isp.keep_external_hw_state ? 1 : 0;
    dst->input.isp.fallback_on_error = dst->input.isp.fallback_on_error ? 1 : 0;
    dst->input.isp.health_check_enable = dst->input.isp.health_check_enable ? 1 : 0;
    if (dst->input.isp.meta_timeout_ms <= 0)
        dst->input.isp.meta_timeout_ms = 2000;
    if (dst->input.isp.max_error_count <= 0)
        dst->input.isp.max_error_count = 3;
    dst->input.isp.restart_on_fault = dst->input.isp.restart_on_fault ? 1 : 0;
    fill_default_isp_controls(&dst->input.isp.controls);
    fill_default_low_light_config(&dst->input.isp.low_light);

    if (dst->input.capture_source_count > MEDIA_GATEWAY_MAX_CAPTURE_SOURCES)
        dst->input.capture_source_count = MEDIA_GATEWAY_MAX_CAPTURE_SOURCES;
    for (i = 0; i < dst->input.capture_source_count; ++i)
        fill_default_capture_source(&dst->input.capture_sources[i], has_src ? &src_copy.input.capture_sources[i] : NULL, i);
    for (i = dst->input.capture_source_count; i < MEDIA_GATEWAY_MAX_CAPTURE_SOURCES; ++i)
        memset(&dst->input.capture_sources[i], 0, sizeof(dst->input.capture_sources[i]));

    dst->audio.source.enabled = dst->audio.source.enabled ? 1 : 0;
    dst->audio.source.device_name = safe_str(dst->audio.source.device_name, AUDIO_CAPTURE_DEFAULT_DEVICE);
    if (dst->audio.source.sample_rate <= 0)
        dst->audio.source.sample_rate = AUDIO_CAPTURE_DEFAULT_SAMPLE_RATE;
    if (dst->audio.source.channels <= 0)
        dst->audio.source.channels = AUDIO_CAPTURE_DEFAULT_CHANNELS;
    if (dst->audio.source.format == 0)
        dst->audio.source.format = AUDIO_SAMPLE_FORMAT_S16LE;
    if (dst->audio.source.period_frames <= 0)
        dst->audio.source.period_frames = AUDIO_CAPTURE_DEFAULT_PERIOD_FRAMES;
    if (dst->audio.source.buffer_periods <= 0)
        dst->audio.source.buffer_periods = AUDIO_CAPTURE_DEFAULT_BUFFER_PERIODS;
    if (dst->audio.source.source_slots <= 0)
        dst->audio.source.source_slots = AUDIO_FRAME_SOURCE_DEFAULT_SLOTS;
    if (dst->audio.source.retry_ms <= 0)
        dst->audio.source.retry_ms = DEFAULT_AUDIO_RETRY_MS;
    if (dst->audio.source.max_consecutive_failures <= 0)
        dst->audio.source.max_consecutive_failures = DEFAULT_AUDIO_MAX_CONSECUTIVE_FAILURES;
    if (dst->audio.source.codec == MEDIA_CODEC_NONE)
    {
        dst->audio.source.codec = (dst->audio.source.g711_mode == G711_ENCODER_MODE_ULAW) ? MEDIA_CODEC_G711U : MEDIA_CODEC_G711A;
    }
    if (dst->audio.source.codec != MEDIA_CODEC_G711A &&
        dst->audio.source.codec != MEDIA_CODEC_G711U &&
        dst->audio.source.codec != MEDIA_CODEC_AAC)
    {
        dst->audio.source.codec = MEDIA_CODEC_G711A;
    }
    if (dst->audio.source.codec == MEDIA_CODEC_G711U)
        dst->audio.source.g711_mode = G711_ENCODER_MODE_ULAW;
    else if (dst->audio.source.codec == MEDIA_CODEC_G711A)
        dst->audio.source.g711_mode = G711_ENCODER_MODE_ALAW;
    if (dst->audio.source.aac_bitrate <= 0)
        dst->audio.source.aac_bitrate = 32000;
    if (dst->audio.source.aac_profile <= 0)
        dst->audio.source.aac_profile = 2;
    if (dst->audio.source.bind_stream_index < 0 || dst->audio.source.bind_stream_index >= MEDIA_GATEWAY_MAX_STREAMS)
        dst->audio.source.bind_stream_index = DEFAULT_AUDIO_BIND_STREAM_INDEX;

    if (dst->video.stream_count > MEDIA_GATEWAY_MAX_STREAMS)
        dst->video.stream_count = MEDIA_GATEWAY_MAX_STREAMS;
    for (i = 0; i < dst->video.stream_count; ++i)
        fill_default_stream(&dst->video.streams[i], has_src ? &src_copy.video.streams[i] : &dst->video.streams[i], i);
    for (i = dst->video.stream_count; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
        memset(&dst->video.streams[i], 0, sizeof(dst->video.streams[i]));

    for (i = 0; i < dst->video.stream_count; ++i)
    {
        if (dst->video.streams[i].enabled)
        {
            source_idx = dst->video.streams[i].source_index;
            if (source_idx < 0 || source_idx >= dst->input.capture_source_count)
            {
                source_idx = 0;
                dst->video.streams[i].source_index = 0;
            }
            dst->input.capture_sources[source_idx].enabled = 1;
        }
    }
    return 0;
}

/**
 * @description: 为指定 stream 创建其启用的协议输出通道。
 */
static int setup_outputs_for_stream(MediaGatewayCtx *ctx, int stream_idx)
{
    const MediaGatewayStreamConfig *s = &ctx->config.video.streams[stream_idx];
    MediaOutputConfig output_config = {0};

    if (!s->enabled)
        return 0;

    /* 该路流是否开启了RTSP */
    if (s->enable_rtsp)
    {
        if (ctx->output_count >= MEDIA_GATEWAY_MAX_OUTPUTS)
        {
            LOG_ERROR("setup_outputs_for_stream failed: RTSP output limit stream=%d count=%d max=%d",
                      stream_idx,
                      ctx->output_count,
                      MEDIA_GATEWAY_MAX_OUTPUTS);
            return -1;
        }
        memset(&output_config, 0, sizeof(output_config));
        output_config.type = MEDIA_OUTPUT_TYPE_RTSP;
        output_config.protocol.rtsp = s->rtsp;
        output_config.protocol.rtsp.feedback_holder = &ctx->network_feedback_holder;
        if (ctx->config.audio.source.enabled && ctx->config.audio.source.bind_stream_index == stream_idx)
        {
            output_config.protocol.rtsp.audio_codec = ctx->config.audio.source.codec;
            output_config.protocol.rtsp.audio_sample_rate = ctx->config.audio.source.sample_rate;
            output_config.protocol.rtsp.audio_channels = ctx->config.audio.source.channels;
            output_config.protocol.rtsp.aac_profile = ctx->config.audio.source.aac_profile;
        }
        else
        {
            output_config.protocol.rtsp.audio_codec = MEDIA_CODEC_NONE;
        }
        if (media_output_setup(&ctx->outputs[ctx->output_count], &output_config) != 0)
        {
            LOG_ERROR("setup_outputs_for_stream failed: setup RTSP stream=%d name=%s",
                      stream_idx,
                      s->name ? s->name : "unknown");
            return -1;
        }
        ctx->output_stream_index[ctx->output_count] = stream_idx;
        ctx->rtsp_output_index[stream_idx] = ctx->output_count;
        ctx->output_count++;
    }

    /* 该路流是否开启了RTMP */
    if (s->enable_rtmp)
    {
#if defined(ENABLE_RTMP_OUTPUT)
        if (ctx->output_count >= MEDIA_GATEWAY_MAX_OUTPUTS)
        {
            LOG_ERROR("setup_outputs_for_stream failed: RTMP output limit stream=%d count=%d max=%d",
                      stream_idx,
                      ctx->output_count,
                      MEDIA_GATEWAY_MAX_OUTPUTS);
            return -1;
        }
        memset(&output_config, 0, sizeof(output_config));
        output_config.type = MEDIA_OUTPUT_TYPE_RTMP;
        output_config.protocol.rtmp = s->rtmp;
        if (media_output_setup(&ctx->outputs[ctx->output_count], &output_config) != 0)
        {
            LOG_ERROR("setup_outputs_for_stream failed: setup RTMP stream=%d name=%s",
                      stream_idx,
                      s->name ? s->name : "unknown");
            return -1;
        }
        ctx->output_stream_index[ctx->output_count] = stream_idx;
        ctx->output_count++;
#endif
    }
    /* 该路流是否开启了GB28181 */
    if (s->enable_gb28181)
    {
        if (ctx->output_count >= MEDIA_GATEWAY_MAX_OUTPUTS)
        {
            LOG_ERROR("setup_outputs_for_stream failed: GB28181 output limit stream=%d count=%d max=%d",
                      stream_idx,
                      ctx->output_count,
                      MEDIA_GATEWAY_MAX_OUTPUTS);
            return -1;
        }
        memset(&output_config, 0, sizeof(output_config));
        output_config.type = MEDIA_OUTPUT_TYPE_GB28181;
        output_config.protocol.gb28181 = s->gb28181;
        if (media_output_setup(&ctx->outputs[ctx->output_count], &output_config) != 0)
        {
            LOG_ERROR("setup_outputs_for_stream failed: setup GB28181 stream=%d name=%s",
                      stream_idx,
                      s->name ? s->name : "unknown");
            return -1;
        }
        ctx->output_stream_index[ctx->output_count] = stream_idx;
        ctx->gb28181_output_index[stream_idx] = ctx->output_count;
        ctx->output_count++;
    }
    return 0;
}

/**
 * @description: 为全部启用 stream 创建输出通道。
 */
static int setup_outputs(MediaGatewayCtx *ctx)
{
    int i = 0;

    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        if (!ctx->stream_enabled[i])
            continue;
        /* 为stream创建每个协议对应的输出通道 */
        if (setup_outputs_for_stream(ctx, i) != 0)
        {
            LOG_ERROR("setup_outputs failed: stream=%d name=%s",
                      i,
                      ctx->config.video.streams[i].name ? ctx->config.video.streams[i].name : "unknown");
            return -1;
        }
    }
    if (ctx->output_count <= 0)
    {
        LOG_ERROR("setup_outputs failed: no enabled output configured");
        return -1;
    }
    return 0;
}

/**
 * @description: 启动所有已创建的输出线程。
 */
static int start_outputs(MediaGatewayCtx *ctx)
{
    int i = 0;

    for (i = 0; i < ctx->output_count; ++i)
    {
        if (media_output_start(&ctx->outputs[i]) != 0)
        {
            LOG_ERROR("start_outputs failed: idx=%d name=%s stream=%d",
                      i,
                      ctx->outputs[i].config.name ? ctx->outputs[i].config.name : "unknown",
                      ctx->output_stream_index[i]);
            return -1;
        }
    }
    return 0;
}

/**
 * @description: 停止所有输出线程。
 */
static void stop_outputs(MediaGatewayCtx *ctx)
{
    int i = 0;

    for (i = 0; i < ctx->output_count; ++i)
        media_output_stop(&ctx->outputs[i]);
}

/**
 * @description: 释放所有输出对象及其协议实现对象。
 */
static void deinit_outputs(MediaGatewayCtx *ctx)
{
    int i = 0;
    void *impl = NULL;

    for (i = 0; i < ctx->output_count; ++i)
    {
        impl = ctx->outputs[i].impl;
        media_output_deinit(&ctx->outputs[i]);
        free(impl);
    }
    ctx->output_count = 0;
}

/**
 * @description: 打印归一化后的关键配置。
 */
static void log_effective_config(const MediaGatewayConfig *cfg)
{
    if (!cfg)
        return;
    LOG_INFO("[CFG] capture_source_count=%d stream_count=%d low_latency=%d stats_interval_sec=%d isp_enable=%d",
             cfg->input.capture_source_count,
             cfg->video.stream_count,
             cfg->system.runtime.low_latency_mode,
             cfg->system.runtime.stats_interval_sec,
             cfg->input.isp.enabled);
    LOG_INFO("[CFG] light_fps enable=%d normal=%d low_light=%d bright=%d min_switch_ms=%d eval_ms=%d ae_bright(exp<=%.2fus gain<=%.2f luma>=%.2f) ae_low(exp_ratio>=%.2f gain>=%.2f luma<=%.2f)",
             cfg->policy.light_fps.enabled,
             cfg->policy.light_fps.targets.normal_fps,
             cfg->policy.light_fps.targets.low_light_fps,
             cfg->policy.light_fps.targets.bright_fps,
             cfg->policy.light_fps.timing.min_switch_interval_ms,
             cfg->policy.light_fps.timing.evaluate_interval_ms,
             cfg->policy.light_fps.ae.bright_max_exposure_us,
             cfg->policy.light_fps.ae.bright_max_analog_gain,
             cfg->policy.light_fps.ae.bright_min_mean_luma,
             cfg->policy.light_fps.ae.low_light_min_exposure_ratio,
             cfg->policy.light_fps.ae.low_light_min_analog_gain,
             cfg->policy.light_fps.ae.low_light_max_mean_luma);
}

/**
 * @description: 初始化基础上下文、统计锁和归一化配置。
 */
static int init_gateway_base(MediaGatewayCtx *ctx, const MediaGatewayConfig *config)
{
    int i = 0;

    memset(ctx, 0, sizeof(*ctx));
    if (pthread_mutex_init(&ctx->stats_lock, NULL) != 0)
    {
        LOG_ERROR("failed: pthread_mutex_init stats_lock");
        return -1;
    }
    ctx->stats_lock_ready = 1;
    if (fill_default_config(&ctx->config, config) != 0)
    {
        LOG_ERROR("failed: invalid config");
        return -1;
    }
    log_set_level((LogLevel)ctx->config.system.log.level);
    log_effective_config(&ctx->config);
    ctx->network_feedback_holder.callback = media_gateway_handle_output_network_feedback;
    ctx->network_feedback_holder.userdata = ctx;
    if (ctx->config.video.stream_count > 0)
    {
        ctx->light_fps_state.current_fps = ctx->config.video.streams[0].fps;
        ctx->light_fps_state.target_fps = ctx->config.video.streams[0].fps;
        ctx->light_fps_state.last_logged_target_fps = ctx->config.video.streams[0].fps;
        ctx->adaptive_policy_state.output.target_fps = ctx->config.video.streams[0].fps;
    }
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        ctx->rtsp_output_index[i] = -1;
        ctx->gb28181_output_index[i] = -1;
        if (i < ctx->config.video.stream_count)
        {
            ctx->adaptive_policy_state.encode_params.base[i].fps = ctx->config.video.streams[i].fps;
            ctx->adaptive_policy_state.encode_params.base[i].bitrate = ctx->config.video.streams[i].bitrate;
            ctx->adaptive_policy_state.encode_params.base[i].gop = ctx->config.video.streams[i].gop;
            ctx->adaptive_policy_state.encode_params.base[i].rc_mode = ctx->config.video.streams[i].rc_mode;
            ctx->adaptive_policy_state.encode_params.base[i].qp_init = ctx->config.video.streams[i].qp_init;
            ctx->adaptive_policy_state.encode_params.base[i].qp_min = ctx->config.video.streams[i].qp_min;
            ctx->adaptive_policy_state.encode_params.base[i].qp_max = ctx->config.video.streams[i].qp_max;
            ctx->adaptive_policy_state.encode_params.base[i].qp_min_i = ctx->config.video.streams[i].qp_min_i;
            ctx->adaptive_policy_state.encode_params.base[i].qp_max_i = ctx->config.video.streams[i].qp_max_i;
            ctx->adaptive_policy_state.encode_params.base[i].qp_max_step = ctx->config.video.streams[i].qp_max_step;
            ctx->adaptive_policy_state.encode_params.target[i].fps = ctx->config.video.streams[i].fps;
            ctx->adaptive_policy_state.encode_params.target[i].bitrate = ctx->config.video.streams[i].bitrate;
            ctx->adaptive_policy_state.encode_params.target[i].gop = ctx->config.video.streams[i].gop;
            ctx->adaptive_policy_state.encode_params.target[i].rc_mode = ctx->config.video.streams[i].rc_mode;
            ctx->adaptive_policy_state.encode_params.target[i].qp_init = ctx->config.video.streams[i].qp_init;
            ctx->adaptive_policy_state.encode_params.target[i].qp_min = ctx->config.video.streams[i].qp_min;
            ctx->adaptive_policy_state.encode_params.target[i].qp_max = ctx->config.video.streams[i].qp_max;
            ctx->adaptive_policy_state.encode_params.target[i].qp_min_i = ctx->config.video.streams[i].qp_min_i;
            ctx->adaptive_policy_state.encode_params.target[i].qp_max_i = ctx->config.video.streams[i].qp_max_i;
            ctx->adaptive_policy_state.encode_params.target[i].qp_max_step = ctx->config.video.streams[i].qp_max_step;
        }
    }
    return 0;
}

/**
 * @description: 初始化 RKAIQ/ISP 控制链路；失败时可按配置降级继续运行。
 */
static int init_gateway_isp(MediaGatewayCtx *ctx)
{
    IspControllerConfig isp_config = {0};
    int source_idx = 0;

    if (!ctx->config.input.isp.enabled)
    {
        LOG_WARN("[ISP] disabled in config; gateway continues with plain V4L2 capture");
        return 0;
    }

    isp_config.lifecycle.enabled = ctx->config.input.isp.enabled;
    isp_config.sensor.sensor_name = ctx->config.input.isp.sensor_name;
    isp_config.sensor.video_device = safe_str(ctx->config.input.isp.video_device,
                                              ctx->config.input.capture_sources[0].device_path);
    isp_config.sensor.iq_dir = ctx->config.input.isp.iq_dir;
    isp_config.sensor.force_iq_file = ctx->config.input.isp.force_iq_file;
    isp_config.sensor.width = ctx->config.input.isp.width;
    isp_config.sensor.height = ctx->config.input.isp.height;
    isp_config.sensor.working_mode = ctx->config.input.isp.working_mode;
    isp_config.lifecycle.keep_external_hw_state = ctx->config.input.isp.keep_external_hw_state;
    isp_config.lifecycle.fallback_on_error = ctx->config.input.isp.fallback_on_error;
    isp_config.controls = ctx->config.input.isp.controls;
    isp_config.health.check_enable = ctx->config.input.isp.health_check_enable;
    isp_config.health.meta_timeout_ms = ctx->config.input.isp.meta_timeout_ms;
    isp_config.health.max_error_count = ctx->config.input.isp.max_error_count;
    isp_config.health.restart_on_fault = ctx->config.input.isp.restart_on_fault;
    isp_config.low_light = ctx->config.input.isp.low_light;

    if (ctx->config.input.capture_source_count > 0)
        source_idx = 0;
    if (isp_config.sensor.width <= 0)
        isp_config.sensor.width = ctx->config.input.capture_sources[source_idx].width;
    if (isp_config.sensor.height <= 0)
        isp_config.sensor.height = ctx->config.input.capture_sources[source_idx].height;

    if (isp_controller_init(&ctx->isp, &isp_config) != 0)
    {
        LOG_ERROR("init ISP failed: sensor=%s iq_dir=%s fallback=%d",
                  safe_str(isp_config.sensor.sensor_name, "auto"),
                  safe_str(isp_config.sensor.iq_dir, "unknown"),
                  isp_config.lifecycle.fallback_on_error);
        return -1;
    }

    ctx->isp_ready = isp_controller_is_started(&ctx->isp);
    if (ctx->config.input.isp.enabled && !ctx->isp_ready)
    {
        LOG_WARN("[ISP] requested but not active; gateway continues with plain V4L2 capture");
    }
    return 0;
}

/**
 * @description: 初始化所有启用的视频采集设备。
 */
static int init_gateway_captures(MediaGatewayCtx *ctx)
{
    int i = 0;
    V4L2CaptureConfig capture_config = {0};
    const CaptureSourceConfig *source = NULL;

    for (i = 0; i < ctx->config.input.capture_source_count; ++i)
    {
        source = &ctx->config.input.capture_sources[i];
        if (!source->enabled)
            continue;

        memset(&capture_config, 0, sizeof(capture_config));
        capture_config.device_path = source->device_path;
        capture_config.width = source->width;
        capture_config.height = source->height;
        capture_config.pixelformat = source->pixelformat;
        capture_config.buffer_count = source->buffer_count;
        if (v4l2_capture_init_with_config(&ctx->captures[i], &capture_config) < 0)
        {
            LOG_ERROR("init capture source=%d name=%s device=%s",
                      i,
                      source->name ? source->name : "unknown",
                      source->device_path ? source->device_path : "unknown");
            return -1;
        }
        ctx->capture_ready[i] = 1;
        ctx->config.input.capture_sources[i].width = ctx->captures[i].format.width;
        ctx->config.input.capture_sources[i].height = ctx->captures[i].format.height;
        ctx->config.input.capture_sources[i].pixelformat = ctx->captures[i].format.pixelformat;
    }
    return 0;
}

/**
 * @description: 初始化所有启用 stream 的视频编码器。
 */
static int init_gateway_video_encoders(MediaGatewayCtx *ctx)
{
    int i = 0;
    int source_idx = 0;

    for (i = 0; i < ctx->config.video.stream_count; ++i)
    {
        if (!ctx->config.video.streams[i].enabled)
            continue;
        source_idx = ctx->config.video.streams[i].source_index;
        if (source_idx < 0 || source_idx >= ctx->config.input.capture_source_count || !ctx->capture_ready[source_idx])
        {
            LOG_ERROR("stream=%d name=%s capture source not ready index=%d",
                      i,
                      ctx->config.video.streams[i].name ? ctx->config.video.streams[i].name : "unknown",
                      source_idx);
            return -1;
        }
        if (media_gateway_reset_encoder(ctx, i) != 0)
        {
            LOG_ERROR("reset_encoder stream=%d name=%s",
                      i,
                      ctx->config.video.streams[i].name ? ctx->config.video.streams[i].name : "unknown");
            return -1;
        }
        ctx->stream_enabled[i] = 1;
    }
    return 0;
}

/**
 * @description: 初始化音频采集设备和配置选择的音频编码器。
 */
static int init_gateway_audio(MediaGatewayCtx *ctx)
{
    AudioCaptureConfig audio_capture_config = {0};
    G711EncoderConfig g711_config = {0};
    AacEncoderConfig aac_config = {0};

    if (ctx->config.audio.source.enabled)
    {
        audio_capture_config.device_name = ctx->config.audio.source.device_name;
        audio_capture_config.sample_rate = ctx->config.audio.source.sample_rate;
        audio_capture_config.channels = ctx->config.audio.source.channels;
        audio_capture_config.format = ctx->config.audio.source.format;
        audio_capture_config.period_frames = ctx->config.audio.source.period_frames;
        audio_capture_config.buffer_periods = ctx->config.audio.source.buffer_periods;
        if (audio_capture_init(&ctx->audio_capture, &audio_capture_config) != 0)
        {
            LOG_ERROR("init audio capture failed: device=%s rate=%d channels=%d",
                      ctx->config.audio.source.device_name ? ctx->config.audio.source.device_name : "unknown",
                      ctx->config.audio.source.sample_rate,
                      ctx->config.audio.source.channels);
            return -1;
        }
        ctx->audio_capture_ready = 1;
        ctx->config.audio.source.sample_rate = ctx->audio_capture.config.sample_rate;
        ctx->config.audio.source.period_frames = ctx->audio_capture.config.period_frames;

        if (ctx->config.audio.source.codec == MEDIA_CODEC_AAC)
        {
            memset(&aac_config, 0, sizeof(aac_config));
            aac_config.sample_rate = ctx->audio_capture.config.sample_rate;
            aac_config.channels = ctx->audio_capture.config.channels;
            aac_config.bitrate = ctx->config.audio.source.aac_bitrate;
            aac_config.profile = ctx->config.audio.source.aac_profile;
            aac_config.max_samples_per_frame = ctx->audio_capture.config.period_frames;
            if (aac_encoder_init(&ctx->aac_encoder, &aac_config) != 0)
            {
                LOG_ERROR("init aac encoder failed");
                return -1;
            }
        }
        else
        {
            memset(&g711_config, 0, sizeof(g711_config));
            g711_config.mode = ctx->config.audio.source.g711_mode;
            g711_config.sample_rate = ctx->audio_capture.config.sample_rate;
            g711_config.channels = ctx->audio_capture.config.channels;
            g711_config.max_samples_per_frame = ctx->audio_capture.config.period_frames;
            if (g711_encoder_init(&ctx->audio_encoder, &g711_config) != 0)
            {
                LOG_ERROR("init g711 encoder failed");
                return -1;
            }
        }
        ctx->audio_encoder_ready = 1;
    }
    return 0;
}

/**
 * @description: 创建并启动全部输出通道。
 */
static int init_gateway_outputs(MediaGatewayCtx *ctx)
{
    if (setup_outputs(ctx) != 0)
    {
        LOG_ERROR("init_gateway_outputs failed: setup outputs");
        return -1;
    }
    if (start_outputs(ctx) != 0)
    {
        LOG_ERROR("init_gateway_outputs failed: start outputs");
        return -1;
    }
    return 0;
}

/**
 * @description: 按配置打开本地录像文件。
 */
static int init_gateway_record_file(MediaGatewayCtx *ctx)
{
    if (ctx->config.system.record.file_path && ctx->config.system.record.file_path[0] != '\0')
    {
        ctx->record_fp = fopen(ctx->config.system.record.file_path, "ab");
        if (!ctx->record_fp)
        {
            LOG_ERROR("init gateway record file failed: open record file path=%s errno=%d(%s)",
                      ctx->config.system.record.file_path,
                      errno,
                      strerror(errno));
            return -1;
        }
    }
    return 0;
}

/**
 * @description: 初始化运行期统计和 benchmark 窗口。
 */
static void init_gateway_runtime_stats(MediaGatewayCtx *ctx)
{
    ctx->running = 1;
    ctx->stats.last_ts_us = media_gateway_get_now_us();
    ctx->bench.enable = ctx->config.system.bench.enabled ? 1 : 0;
    ctx->bench.sample_every = ctx->config.system.bench.sample_every;
    ctx->bench.print_interval_sec = ctx->config.system.bench.print_interval_sec;
    if (ctx->bench.sample_every <= 0)
        ctx->bench.sample_every = DEFAULT_BENCH_SAMPLE_EVERY;
    if (ctx->bench.print_interval_sec <= 0)
        ctx->bench.print_interval_sec = DEFAULT_BENCH_PRINT_INTERVAL_SEC;
    ctx->bench.last_ts_us = ctx->stats.last_ts_us;
    LOG_INFO("[CFG] bench enable=%d sample_every=%d print_interval_sec=%d",
             ctx->bench.enable,
             ctx->bench.sample_every,
             ctx->bench.print_interval_sec);
    media_gateway_bench_reset_window(ctx);
}

/**
 * @description: 初始化 media gateway 上下文和所有长期持有的资源。
 */
int media_gateway_init(MediaGatewayCtx *ctx, const MediaGatewayConfig *config)
{
    if (!ctx)
    {
        LOG_ERROR("ctx is NULL");
        return -1;
    }
    if (init_gateway_base(ctx, config) != 0)
    {
        LOG_ERROR("init gateway base");
        return -1;
    }
    /* 初始化ISP控制 */
    if (init_gateway_isp(ctx) != 0)
    {
        LOG_ERROR("init gateway ISP");
        goto fail;
    }
    if (init_gateway_captures(ctx) != 0)
    {
        LOG_ERROR("init gateway captures");
        goto fail;
    }
    if (init_gateway_video_encoders(ctx) != 0)
    {
        LOG_ERROR("init gateway video encoders");
        goto fail;
    }
    if (init_gateway_audio(ctx) != 0)
    {
        LOG_ERROR("init gateway audio");
        goto fail;
    }
    if (init_gateway_outputs(ctx) != 0)
    {
        LOG_ERROR("init gateway outputs");
        goto fail;
    }
    if (init_gateway_record_file(ctx) != 0)
    {
        LOG_ERROR("init gateway record file");
        goto fail;
    }

    init_gateway_runtime_stats(ctx);

    /* 初始化调试命令 */
    if (media_gateway_debug_register_shell_commands(ctx) != 0)
        LOG_WARN("warning: register shell commands failed");
    return 0;

fail:
    LOG_ERROR("rollback: deinit partially initialized resources");
    media_gateway_deinit(ctx);
    return -1;
}

/**
 * @description: 启动 run 生命周期内的视频采集帧源。
 */
static int start_run_frame_sources(MediaGatewayCtx *ctx, MediaGatewayRunResources *res)
{
    int source_idx = 0;

    for (source_idx = 0; source_idx < ctx->config.input.capture_source_count; ++source_idx)
    {
        if (!ctx->capture_ready[source_idx])
            continue;
        if (media_frame_source_init(&res->frame_sources[source_idx],
                                    &ctx->captures[source_idx],
                                    ctx->config.input.capture_sources[source_idx].name,
                                    ctx->config.system.runtime.capture_retry_ms,
                                    ctx->config.system.runtime.max_consecutive_failures,
                                    source_idx,
                                    &res->result_queue) != 0)
        {
            LOG_ERROR("media_gateway_run failed: init frame source source=%d", source_idx);
            return -1;
        }
        res->frame_source_inited[source_idx] = 1;
        if (media_frame_source_start(&res->frame_sources[source_idx]) != 0)
        {
            LOG_ERROR("media_gateway_run failed: start frame source source=%d", source_idx);
            return -1;
        }
        res->frame_source_started[source_idx] = 1;
    }
    return 0;
}

/**
 * @description: 启动 run 生命周期内的音频采集帧源。
 */
static int start_run_audio_source(MediaGatewayCtx *ctx, MediaGatewayRunResources *res)
{
    if (ctx->config.audio.source.enabled && ctx->audio_capture_ready)
    {
        if (audio_frame_source_init(&res->audio_source,
                                    &ctx->audio_capture,
                                    ctx->config.audio.source.source_slots,
                                    ctx->config.audio.source.retry_ms,
                                    ctx->config.audio.source.max_consecutive_failures) != 0)
        {
            LOG_ERROR("media_gateway_run failed: init audio frame source");
            return -1;
        }
        res->audio_source_inited = 1;
        if (audio_frame_source_start(&res->audio_source) != 0)
        {
            LOG_ERROR("media_gateway_run failed: start audio frame source");
            return -1;
        }
        res->audio_source_started = 1;
    }
    return 0;
}

/**
 * @description: 从一个视频采集源取最新帧并发布到绑定的 stream 输入槽。
 */
static int dispatch_video_source_once(MediaGatewayCtx *ctx,
                                      MediaGatewayRunResources *res,
                                      int source_idx,
                                      int *got_frame)
{
    MediaFrame frame = {0};
    int stream_idx = 0;
    int slot_index = -1;
    int acquire_ret = 0;
    int ret = 0;

    if (!res->frame_source_started[source_idx])
        return 0;
    /* 从视频采集线程队列里获取最新帧 */
    acquire_ret = media_frame_source_acquire_latest(&res->frame_sources[source_idx], &frame, &slot_index, 10);
    if (acquire_ret < 0)
    {
        LOG_ERROR("media_gateway_run failed: frame source stopped by fatal error source=%d", source_idx);
        return -1;
    }
    if (acquire_ret == 0)
        return 0;

    *got_frame = 1;
    for (stream_idx = 0; stream_idx < ctx->config.video.stream_count; ++stream_idx)
    {
        if (!ctx->stream_enabled[stream_idx])
            continue;
        if (ctx->config.video.streams[stream_idx].source_index != source_idx)
            continue;
        /* 将最新采集帧引用发布到对应的视频编码线程输入槽。 */
        if (media_gateway_video_input_publish(&res->pipeline.video.workers[stream_idx].input, &frame) != 0)
        {
            LOG_ERROR("media_gateway_run failed: publish video frame source=%d stream=%d",
                      source_idx,
                      stream_idx);
            ret = -1;
            ctx->running = 0;
            break;
        }
    }

    media_frame_source_release(&res->frame_sources[source_idx], slot_index);
    media_frame_reset(&frame);
    return ret;
}

/**
 * @description: 非阻塞 drain 音频帧并发布到音频编码 FIFO。
 */
static int drain_audio_source_once(MediaGatewayCtx *ctx,
                                   MediaGatewayRunResources *res,
                                   int *got_frame)
{
    AudioFrame audio_frame = {0};
    int audio_slot_index = -1;
    int audio_drain_count = 0;
    int acquire_ret = 0;

    if (!res->audio_source_started)
        return 0;

    while (audio_drain_count < ctx->config.audio.source.source_slots)
    {
        audio_slot_index = -1;
        /* 从音频采集线程队列里面获取最旧帧 */
        acquire_ret = audio_frame_source_acquire(&res->audio_source, &audio_frame, &audio_slot_index, 0);
        if (acquire_ret < 0)
        {
            LOG_ERROR("media_gateway_run failed: audio frame source fatal error");
            return -1;
        }
        if (acquire_ret == 0)
            break;

        *got_frame = 1;
        /* 将音频帧发布到音频编码队列 */
        if (media_gateway_audio_queue_publish(&res->pipeline.audio.queue, &audio_frame) != 0)
        {
            LOG_ERROR("media_gateway_run failed: publish audio frame=%" PRIu64, audio_frame.frame_id);
            ctx->running = 0;
            audio_frame_source_release(&res->audio_source, audio_slot_index);
            return -1;
        }
        audio_frame_source_release(&res->audio_source, audio_slot_index);
        audio_drain_count++;
    }
    return 0;
}

/**
 * @brief 非阻塞向指定编码线程投递设置帧率命令。
 */
static int submit_encoder_fps_command(MediaGatewayRunResources *res,
                                      int stream_idx,
                                      uint64_t request_id,
                                      int target_fps)
{
    MediaSetFpsRequest request = {0};
    ThreadMessage command = {0};
    int ret = 0;

    if (!res || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS ||
        request_id == 0 || target_fps <= 0)
    {
        LOG_ERROR("submit_encoder_fps_command failed: res=%p stream=%d request=%" PRIu64 " target=%d",
                  (void *)res,
                  stream_idx,
                  request_id,
                  target_fps);
        return -1;
    }

    /*
     * payload 位于当前函数栈上，但 push_copy 会在返回前深拷贝数据，
     * 因此命令投递成功后无需延长 request 的生命周期。
     */
    request.target_fps = target_fps;
    command.type = MEDIA_CONTROL_MESSAGE_SET_FPS;
    command.request_id = request_id;
    command.endpoint_type = MEDIA_CONTROL_ENDPOINT_ENCODER;
    command.endpoint_index = stream_idx;
    command.data = &request;
    command.data_size = sizeof(request);
    ret = media_gateway_pipeline_submit_video_command(&res->pipeline,
                                                      stream_idx,
                                                      &command);
    if (ret != 0)
    {
        LOG_ERROR("submit_encoder_fps_command failed: submit stream=%d request=%" PRIu64 " target=%d ret=%d",
                  stream_idx,
                  request_id,
                  target_fps,
                  ret);
    }
    /* 本函数只负责非阻塞投递，MPP 配置由对应编码线程执行。 */
    return ret;
}

/**
 * @brief 非阻塞向指定编码线程投递完整编码运行参数命令。
 * 联合控制器使用该接口一次性下发 fps、码率、GOP、RC 和 QP。
 */
static int submit_encoder_params_command(MediaGatewayRunResources *res,
                                          int stream_idx,
                                          uint64_t request_id,
                                          const MediaVideoEncodeParams *params)
{
    MediaSetVideoEncodeParamsRequest request = {0};
    ThreadMessage command = {0};
    int ret = 0;

    if (!res || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS ||
        request_id == 0 || !params || params->fps <= 0 ||
        params->bitrate <= 0 || params->gop <= 0)
    {
        LOG_ERROR("submit_encoder_params_command failed: res=%p stream=%d request=%" PRIu64 " params=%p",
                  (void *)res,
                  stream_idx,
                  request_id,
                  (const void *)params);
        return -1;
    }

    request.params = *params;
    command.type = MEDIA_CONTROL_MESSAGE_SET_VIDEO_ENCODE_PARAMS;
    command.request_id = request_id;
    command.endpoint_type = MEDIA_CONTROL_ENDPOINT_ENCODER;
    command.endpoint_index = stream_idx;
    command.data = &request;
    command.data_size = sizeof(request);
    ret = media_gateway_pipeline_submit_video_command(&res->pipeline,
                                                      stream_idx,
                                                      &command);
    if (ret != 0)
    {
        LOG_ERROR("submit_encoder_params_command failed: submit stream=%d request=%" PRIu64 " fps=%d bitrate=%d gop=%d ret=%d",
                  stream_idx,
                  request_id,
                  params->fps,
                  params->bitrate,
                  params->gop,
                  ret);
    }
    return ret;
}

/**
 * @brief 非阻塞向第一个视频采集线程投递设置帧率命令。
 */
static int submit_sensor_fps_command(MediaGatewayRunResources *res,
                                     uint64_t request_id,
                                     int target_fps)
{
    MediaSetFpsRequest request = {0};
    ThreadMessage command = {0};
    int ret = 0;

    if (!res || request_id == 0 || target_fps <= 0 ||
        !res->frame_source_inited[0] || !res->frame_source_started[0])
    {
        LOG_ERROR("submit_sensor_fps_command failed: res=%p request=%" PRIu64 " target=%d inited=%d started=%d",
                  (void *)res,
                  request_id,
                  target_fps,
                  res ? res->frame_source_inited[0] : 0,
                  res ? res->frame_source_started[0] : 0);
        return -1;
    }

    /*
     * 当前动态帧率策略只控制 capture source 0。命令参数由消息队列深拷贝，
     * 实际 VIDIOC_S_PARM 调用在采集线程的两次取帧操作之间执行。
     */
    request.target_fps = target_fps;
    command.type = MEDIA_CONTROL_MESSAGE_SET_FPS;
    command.request_id = request_id;
    command.endpoint_type = MEDIA_CONTROL_ENDPOINT_CAPTURE;
    command.endpoint_index = 0;
    command.data = &request;
    command.data_size = sizeof(request);
    ret = media_frame_source_submit_command(&res->frame_sources[0], &command);
    if (ret != 0)
    {
        LOG_ERROR("submit_sensor_fps_command failed: submit request=%" PRIu64 " target=%d ret=%d",
                  request_id,
                  target_fps,
                  ret);
    }
    /* 返回值只表示命令是否成功入队，不表示 Sensor 已经完成切换。 */
    return ret;
}

/**
 * @brief 投递当前事务中尚未成功的编码器控制命令。
 */
static void submit_pending_encoder_control_commands(MediaGatewayCtx *ctx,
                                                MediaGatewayRunResources *res,
                                                uint32_t retry_mask)
{
    MediaGatewayVideoControlTransition *transition = NULL;
    MediaGatewayStreamConfig *stream_cfg = NULL;
    const MediaVideoEncodeParams *params = NULL;
    uint32_t bit = 0;
    int stream_idx = 0;
    int submit_ret = 0;

    if (!ctx || !res)
    {
        LOG_ERROR("submit_pending_encoder_control_commands failed: ctx=%p res=%p retry_mask=0x%x",
                  (void *)ctx,
                  (void *)res,
                  retry_mask);
        return;
    }

    transition = &res->video_control_transition;
    /*
     * 每轮投递前重新构造 pending/failed 集合。success_mask 保留此前已经
     * 成功的编码器，重试时无需再次配置这些编码器。
     */
    transition->encoder_pending_mask = 0;
    transition->encoder_failed_mask = 0;
    for (stream_idx = 0;
         stream_idx < ctx->config.video.stream_count && stream_idx < MEDIA_GATEWAY_MAX_STREAMS;
         ++stream_idx)
    {
        bit = 1U << stream_idx;
        stream_cfg = &ctx->config.video.streams[stream_idx];
        if (!stream_cfg->enabled || stream_cfg->source_index != 0 ||
            !ctx->encoder_ready[stream_idx])
            continue;
        /*
         * retry_mask 为 0 表示首次投递全部相关编码器；非 0 时只投递上轮
         * 失败的编码器，避免重复配置已经成功的流。
         */
        if (retry_mask != 0 && (retry_mask & bit) == 0)
            continue;

        params = NULL;
        if (ctx->config.policy.network_encode.enabled)
            params = &ctx->adaptive_policy_state.encode_params.target[stream_idx];
        if (params && params->fps > 0 && params->bitrate > 0 && params->gop > 0)
        {
            submit_ret = submit_encoder_params_command(res,
                                                        stream_idx,
                                                        transition->request_id,
                                                        params);
        }
        else
        {
            submit_ret = submit_encoder_fps_command(res,
                                                    stream_idx,
                                                    transition->request_id,
                                                    transition->target_fps);
        }
        if (submit_ret == 0)
        {
            /* 入队成功后等待该编码线程通过统一结果队列回传执行结果。 */
            transition->encoder_pending_mask |= bit;
        }
        else
        {
            transition->encoder_failed_mask |= bit;
            LOG_ERROR("submit_pending_encoder_control_commands failed: stream=%d request=%" PRIu64 " target=%d ret=%d",
                      stream_idx,
                      transition->request_id,
                      transition->target_fps,
                      submit_ret);
        }
    }
    /*
     * 命令入队失败也视为该编码器本轮失败。设置统一退避时间，避免主循环
     * 每次迭代都立即重试并持续刷日志或占用队列。
     */
    if (transition->encoder_failed_mask != 0)
    {
        transition->retry_after_ts_us = media_gateway_get_now_us() +
                                        (uint64_t)MEDIA_GATEWAY_COMMAND_RETRY_MS * 1000ULL;
    }
}

/**
 * @brief 在整条链路确认成功后提交视频控制事务状态。
 */
static void commit_video_control_transition(MediaGatewayCtx *ctx,
                                  MediaGatewayRunResources *res)
{
    MediaGatewayVideoControlTransition *transition = NULL;
    int old_fps = 0;

    if (!ctx || !res)
    {
        LOG_ERROR("commit_video_control_transition failed: ctx=%p res=%p",
                  (void *)ctx,
                  (void *)res);
        return;
    }

    transition = &res->video_control_transition;
    /*
     * 只有编码器阶段全部返回、且没有失败项时才能提交全链路状态。
     * 该校验防止调用顺序错误导致 current_fps 提前变化。
     */
    if (transition->phase != MEDIA_GATEWAY_FPS_TRANSITION_ENCODERS_PENDING ||
        transition->target_fps <= 0 ||
        transition->encoder_pending_mask != 0 ||
        transition->encoder_failed_mask != 0)
    {
        LOG_ERROR("commit_video_control_transition failed: phase=%d target=%d pending=0x%x failed=0x%x request=%" PRIu64,
                  transition->phase,
                  transition->target_fps,
                  transition->encoder_pending_mask,
                  transition->encoder_failed_mask,
                  transition->request_id);
        return;
    }

    /*
     * 此处提交的是“整条受控链路已经一致”的 FPS。Sensor 和各编码器的
     * 实际硬件配置此前已由各自 worker 完成。
     */
    old_fps = ctx->light_fps_state.current_fps;
    ctx->light_fps_state.current_fps = transition->target_fps;
    if (old_fps != transition->target_fps)
        ctx->light_fps_state.last_switch_ts_us = media_gateway_get_now_us();
    LOG_WARN("[VIDEO_CONTROL] async transition completed old=%d new=%d request=%" PRIu64,
             old_fps,
             transition->target_fps,
             transition->request_id);
    /* 提交完成后清理事务位图，使状态机可以接受下一次目标切换。 */
    transition->phase = MEDIA_GATEWAY_FPS_TRANSITION_IDLE;
    transition->encoder_pending_mask = 0;
    transition->encoder_success_mask = 0;
    transition->encoder_failed_mask = 0;
    transition->retry_after_ts_us = 0;
}

/**
 * @brief 处理一条 worker 控制命令执行结果。
 */
static void handle_fps_command_result(MediaGatewayCtx *ctx,
                                      MediaGatewayRunResources *res,
                                      const ThreadMessage *result)
{
    MediaGatewayVideoControlTransition *transition = NULL;
    uint32_t bit = 0;

    /*
     * 结果处理运行在 gateway 主循环线程中。worker 只负责执行命令并回传结果，
     * 所有事务阶段和编码器位图均由本线程更新，因此这里不需要额外加锁。
     */
    if (!ctx || !res || !result)
    {
        LOG_ERROR("handle_fps_command_result failed: ctx=%p res=%p result=%p",
                  (void *)ctx,
                  (void *)res,
                  (const void *)result);
        return;
    }
    transition = &res->video_control_transition;

    /*
     * request_id 用于关联命令与结果。事务重试或下一次切换开始后，旧 worker
     * 结果可能延迟到达，必须丢弃，避免旧结果修改当前事务状态。
     */
    if (result->request_id != transition->request_id)
    {
        LOG_ERROR("[VIDEO_CONTROL] ignore stale result request=%" PRIu64 " current_request=%" PRIu64
                 " endpoint_type=%u endpoint_index=%d",
                 result->request_id,
                 transition->request_id,
                 result->endpoint_type,
                 result->endpoint_index);
        return;
    }
    /* 统一结果队列未来可能承载其他控制命令，此函数只处理帧率或完整编码运行参数结果。 */
    if (result->type != MEDIA_CONTROL_MESSAGE_SET_FPS &&
        result->type != MEDIA_CONTROL_MESSAGE_SET_VIDEO_ENCODE_PARAMS)
    {
        LOG_ERROR("[VIDEO_CONTROL] ignore unexpected result type=%u request=%" PRIu64
                 " expected_type=%u/%u",
                 result->type,
                 result->request_id,
                 MEDIA_CONTROL_MESSAGE_SET_FPS,
                 MEDIA_CONTROL_MESSAGE_SET_VIDEO_ENCODE_PARAMS);
        return;
    }

    /*
     * 第一阶段等待采集线程修改 Sensor FPS。
     * Sensor 失败时不能继续修改编码器，否则会造成采集帧率与编码帧率不一致；
     * 事务退回 IDLE，并设置退避时间，由 drive 函数稍后重新发起完整事务。
     */
    if (transition->phase == MEDIA_GATEWAY_FPS_TRANSITION_SENSOR_PENDING &&
        result->endpoint_type == MEDIA_CONTROL_ENDPOINT_CAPTURE &&
        result->endpoint_index == 0)
    {
        if (result->status != 0)
        {
            LOG_ERROR("[VIDEO_CONTROL] sensor async apply failed target=%d request=%" PRIu64,
                      transition->target_fps,
                      transition->request_id);
            transition->phase = MEDIA_GATEWAY_FPS_TRANSITION_IDLE;
            transition->retry_after_ts_us = media_gateway_get_now_us() +
                                            (uint64_t)MEDIA_GATEWAY_COMMAND_RETRY_MS * 1000ULL;
            return;
        }

        /*
         * Sensor 已切换成功，进入编码器阶段。初次投递 retry_mask 为 0，
         * 表示向所有启用且绑定 source 0 的编码器发送控制命令。
         */
        transition->phase = MEDIA_GATEWAY_FPS_TRANSITION_ENCODERS_PENDING;
        transition->encoder_success_mask = 0;
        submit_pending_encoder_control_commands(ctx, res, 0);
        return;
    }

    /*
     * 非 Sensor 结果必须是编码器阶段的合法编码线程结果。
     * 端点类型、下标或事务阶段不匹配时，不允许更新任何位图。
     */
    if (transition->phase != MEDIA_GATEWAY_FPS_TRANSITION_ENCODERS_PENDING ||
        result->endpoint_type != MEDIA_CONTROL_ENDPOINT_ENCODER ||
        result->endpoint_index < 0 ||
        result->endpoint_index >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("[VIDEO_CONTROL] ignore result with unexpected state phase=%d endpoint_type=%u endpoint_index=%d request=%" PRIu64,
                  transition->phase,
                  result->endpoint_type,
                  result->endpoint_index,
                  result->request_id);
        return;
    }

    /*
     * 每个编码器使用一个 bit 记录状态：
     * pending_mask 表示仍在等待结果，success_mask 表示已经成功，
     * failed_mask 表示执行失败、后续需要重试。
     */
    bit = 1U << result->endpoint_index;
    /*
     * pending 位已经清除，说明该编码器结果已处理，或者本次事务从未向它
     * 投递命令。忽略重复/非请求结果，避免重复修改事务状态。
     */
    if ((transition->encoder_pending_mask & bit) == 0)
    {
        LOG_ERROR("[VIDEO_CONTROL] ignore duplicate or unsolicited encoder result stream=%d request=%" PRIu64
                  " pending_mask=0x%x",
                  result->endpoint_index,
                  result->request_id,
                 transition->encoder_pending_mask);
        return;
    }

    /* 收到结果后先清除 pending，再根据 worker 返回状态记录成功或失败。 */
    transition->encoder_pending_mask &= ~bit;
    if (result->status == 0)
    {
        transition->encoder_success_mask |= bit;
    }
    else
    {
        /*
         * 编码器失败不会回滚已经成功的 Sensor 和其他编码器。
         * 记录失败位并设置退避时间，drive 函数后续只重试 failed_mask 中的流。
         */
        transition->encoder_failed_mask |= bit;
        transition->retry_after_ts_us = media_gateway_get_now_us() +
                                        (uint64_t)MEDIA_GATEWAY_COMMAND_RETRY_MS * 1000ULL;
        LOG_ERROR("[VIDEO_CONTROL] encoder async apply failed stream=%d target=%d request=%" PRIu64,
                  result->endpoint_index,
                  transition->target_fps,
                  transition->request_id);
    }
}

/**
 * @brief 非阻塞处理 worker 回传结果，并限制单次处理数量。
 */
static void drain_worker_results(MediaGatewayCtx *ctx,
                                 MediaGatewayRunResources *res)
{
    ThreadMessage result = {0};
    int pop_ret = 0;
    int result_count = 0;

    if (!ctx || !res || !res->result_queue_inited)
    {
        LOG_ERROR("drain_worker_results failed: ctx=%p res=%p result_queue_inited=%d",
                  (void *)ctx,
                  (void *)res,
                  res ? res->result_queue_inited : 0);
        return;
    }

    /*
     * 结果队列是多生产者、单消费者模型：采集线程和各编码线程负责 push，
     * gateway 主循环负责 try_pop。使用非阻塞读取，结果未到达时立即返回。
     */
    while (result_count < MEDIA_GATEWAY_MAX_RESULTS_PER_LOOP)
    {
        pop_ret = thread_message_queue_try_pop(&res->result_queue, &result);
        if (pop_ret < 0)
        {
            if (pop_ret != -3)
            {
                LOG_ERROR("drain_worker_results failed: pop result ret=%d processed=%d",
                          pop_ret,
                          result_count);
            }
            break;
        }
        if (pop_ret == 0)
            break;

        /*
         * 先处理结果，再释放 pop 转移给本线程的动态 payload。
         * 单轮限制处理数量，避免结果积压时长时间占用帧调度主循环。
         */
        handle_fps_command_result(ctx, res, &result);
        thread_message_release(&result);
        result_count++;
    }
}

/**
 * @brief 因策略目标变化取消尚未完成的视频控制事务。
 *
 * 视频控制命令可能因为 Sensor 或编码器临时失败而进入重试。如果重试期间策略目标已经变化，
 * 继续执行旧事务会导致硬件恢复后先切到旧目标、再切到新目标，形成一次不必要的波动。
 * 因此未完成事务发现目标变化时必须作废。
 *
 * 取消事务时只清理 gateway 侧状态，不尝试从 worker 队列中删除已投递命令：
 * 1. 已经入队的旧命令可能正在被 worker 执行，跨线程删除反而会引入复杂同步；
 * 2. 后续新事务会分配新的 request_id，旧结果回到统一结果队列时会被 request_id 过滤；
 * 3. 状态机回到 IDLE 后，会按最新 target_fps 重新发起完整 Sensor -> Encoder 切换。
 *
 * @param ctx gateway 上下文，用于读取最新策略目标。
 * @param res 当前 run 生命周期资源，保存未完成事务状态。
 */
static void cancel_video_control_transition_for_target_change(MediaGatewayCtx *ctx,
                                                    MediaGatewayRunResources *res)
{
    MediaGatewayVideoControlTransition *transition = NULL;
    int old_target_fps = 0;
    int new_target_fps = 0;

    if (!ctx || !res)
    {
        LOG_ERROR("cancel_video_control_transition_for_target_change failed: ctx=%p res=%p",
                  (void *)ctx,
                  (void *)res);
        return;
    }

    transition = &res->video_control_transition;
    if (transition->phase == MEDIA_GATEWAY_FPS_TRANSITION_IDLE)
        return;

    old_target_fps = transition->target_fps;
    new_target_fps = adaptive_effective_target_fps(ctx);
    if (old_target_fps == new_target_fps)
        return;

    LOG_WARN("[VIDEO_CONTROL] cancel pending transition because target changed old_target=%d new_target=%d request=%" PRIu64 " phase=%d pending=0x%x failed=0x%x",
             old_target_fps,
             new_target_fps,
             transition->request_id,
             transition->phase,
             transition->encoder_pending_mask,
             transition->encoder_failed_mask);

    /*
     * 保留 request_id 不变，等待下一次启动新事务时再递增。
     * 这样旧 worker 结果在新事务启动前也不会被误处理：phase 已经回到 IDLE，
     * handle_fps_command_result 会因阶段/端点不匹配而忽略。
     */
    transition->phase = MEDIA_GATEWAY_FPS_TRANSITION_IDLE;
    transition->target_fps = 0;
    transition->encoder_pending_mask = 0;
    transition->encoder_success_mask = 0;
    transition->encoder_failed_mask = 0;
    transition->retry_after_ts_us = 0;
}

/**
 * @brief 驱动运行期视频控制异步切换事务。
 *
 * 本函数由 run_gateway_once 周期性调用，是亮度帧率策略、网络自适应策略与资源工作线程之间
 * 的异步协调器。函数本身只读取状态、收集结果和投递命令，不直接调用
 * V4L2 ioctl 或 MPP control，因此不会因硬件配置操作阻塞主循环的帧调度。
 *
 * 状态机执行顺序：
 * 1. 非阻塞读取采集线程和编码线程回传的命令执行结果；
 * 2. Sensor 切换成功后，再向 source_index 为 0 的相关编码线程投递 FPS 或完整编码参数命令；
 * 3. 等待所有相关编码器返回结果，全部成功后提交 current_fps；
 * 4. 未完成事务期间如果策略 target_fps 已变化，取消旧事务并按最新目标重新发起；
 * 5. 某路编码器失败时，只在退避时间到达后重试失败的编码器；
 * 6. 空闲状态下发现 target_fps 与 current_fps 不同时，创建新的 request_id，
 *    并首先向采集线程投递 Sensor FPS 切换命令；
 * 7. 空闲状态下只有网络侧编码参数变化时，跳过 Sensor，直接向编码线程投递完整编码参数命令。
 *
 * 同一时间只允许一个视频控制事务处于执行状态。request_id 用于过滤上一事务
 * 延迟到达的旧结果，pending/success/failed 位图用于记录各编码器执行状态。
 *
 * @param ctx gateway 上下文，提供融合后的控制目标和当前已提交状态。
 * @param res 当前 run 生命周期资源，保存命令队列、结果队列和事务状态。
 */
static void drive_runtime_video_control_transition(MediaGatewayCtx *ctx,
                                         MediaGatewayRunResources *res)
{
    MediaGatewayVideoControlTransition *transition = NULL;
    uint64_t now_us = 0;
    uint32_t retry_mask = 0;
    int target_fps = 0;
    int need_params_apply = 0;
    int need_sensor_fps = 0;
    int submit_ret = 0;

    if (!ctx || !res ||
        (!ctx->config.policy.light_fps.enabled &&
         !ctx->config.policy.network_encode.enabled))
        return;

    transition = &res->video_control_transition;
    now_us = media_gateway_get_now_us();
    /*
     * 如果旧事务还没完成，而策略层已经根据新环境更新了 target_fps，
     * 必须先作废旧事务，再消费 worker 结果。否则旧 Sensor 成功结果可能
     * 在 drain 阶段触发旧目标编码器命令投递，恢复后仍可能产生一次旧目标波动。
     */
    cancel_video_control_transition_for_target_change(ctx, res);

    /*
     * 消费 worker 回传结果，使事务状态反映最新执行进度，
     * 再决定提交事务、重试失败编码器或启动下一次切换。
     */
    drain_worker_results(ctx, res);

    /*
     * 编码器阶段没有 pending 项，表示本轮所有已投递命令都已有结果。
     */
    if (transition->phase == MEDIA_GATEWAY_FPS_TRANSITION_ENCODERS_PENDING &&
        transition->encoder_pending_mask == 0)
    {
        /* 无失败项时提交整条链路状态 */
        if (transition->encoder_failed_mask == 0)
        {
            commit_video_control_transition(ctx, res);
        }
        /* 有失败项时等待退避时间到达后重试失败编码器 */
        else if (now_us >= transition->retry_after_ts_us)
        {
            retry_mask = transition->encoder_failed_mask;
            submit_pending_encoder_control_commands(ctx, res, retry_mask);
        }
        return;
    }

    /*
     * 先拆分本轮需要执行的控制动作：
     * 1. 最终帧率变化时，需要先切 Sensor，再切编码器；
     * 2. 仅网络反馈导致码率/GOP/QP 等编码参数变化时，直接投递编码器命令。
     */
    target_fps = adaptive_effective_target_fps(ctx);
    /* 判断根据网络反馈得到的编码参数是否与目前的一致，如果一致则不需要进行配置  */
    need_params_apply = adaptive_target_params_need_apply(ctx);
    need_sensor_fps = (target_fps != ctx->light_fps_state.current_fps);
    
    if (transition->phase != MEDIA_GATEWAY_FPS_TRANSITION_IDLE ||
        (!need_sensor_fps && !need_params_apply) ||
        now_us < transition->retry_after_ts_us ||
        (need_sensor_fps && !res->frame_source_started[0]))
        return;

    /*
     * 为新事务生成非零 request_id。worker 回传相同 request_id，
     * 主循环据此过滤延迟到达的旧事务结果。
     */
    transition->next_request_id++;
    if (transition->next_request_id == 0)
        transition->next_request_id = 1;
    transition->request_id = transition->next_request_id;
    transition->target_fps = target_fps;

    if (!need_sensor_fps && need_params_apply)
    {
        /*
         * 网络自适应只改变编码运行参数时，不需要触碰 Sensor。
         * 直接进入编码器阶段，等待所有相关编码线程回传执行结果后提交状态。
         */
        transition->phase = MEDIA_GATEWAY_FPS_TRANSITION_ENCODERS_PENDING;
        transition->encoder_success_mask = 0;
        submit_pending_encoder_control_commands(ctx, res, 0);
        if (transition->encoder_pending_mask == 0 &&
            transition->encoder_failed_mask == 0)
        {
            commit_video_control_transition(ctx, res);
        }
        LOG_WARN("[VIDEO_CONTROL] async encoder params transition started current=%d target=%d request=%" PRIu64,
                 ctx->light_fps_state.current_fps,
                 transition->target_fps,
                 transition->request_id);
        return;
    }

    /*
     * 切换顺序必须从 Sensor 开始。只有 Sensor 返回成功后，
     * handle_fps_command_result 才会向编码线程投递命令。
     */
    submit_ret = submit_sensor_fps_command(res,
                                           transition->request_id,
                                           transition->target_fps);
    if (submit_ret != 0)
    {
        /* 命令入队失败时保持 IDLE，设置退避时间后重新发起完整事务。 */
        transition->retry_after_ts_us = now_us +
                                        (uint64_t)MEDIA_GATEWAY_COMMAND_RETRY_MS * 1000ULL;
        LOG_WARN("[VIDEO_CONTROL] submit sensor command failed target=%d ret=%d",
                 transition->target_fps,
                 submit_ret);
        return;
    }
    /* Sensor 命令成功入队，状态机转入等待采集线程结果阶段。 */
    transition->phase = MEDIA_GATEWAY_FPS_TRANSITION_SENSOR_PENDING;
    LOG_WARN("[VIDEO_CONTROL] async transition started current=%d target=%d request=%" PRIu64,
             ctx->light_fps_state.current_fps,
             transition->target_fps,
             transition->request_id);
}

/**
 * @description: 执行一次 run 主循环调度。
 * 这个函数是 run 循环的核心，负责从视频采集线程队列里获取最新帧并发布到对应的视频编码线程输入队列，
 * 没有让编码线程直接从采集线程队列获取帧的原因是多个编码线程可能对应一个采集源
 */
static int run_gateway_once(MediaGatewayCtx *ctx, MediaGatewayRunResources *res, int *got_frame)
{
    int source_idx = 0;

    *got_frame = 0;
    for (source_idx = 0; source_idx < ctx->config.input.capture_source_count; ++source_idx)
    {
        /* 从视频采集线程队列里面获取最新帧并放入编码线程得到输入队列 */
        if (dispatch_video_source_once(ctx, res, source_idx, got_frame) != 0)
        {
            LOG_ERROR("run_gateway_once failed: dispatch video source=%d", source_idx);
            return -1;
        }
    }

    /* 从音频采集线程队列里面获取最新帧并放入音频编码线程输入队列 */
    if (drain_audio_source_once(ctx, res, got_frame) != 0)
    {
        LOG_ERROR("run_gateway_once failed: drain audio source");
        return -1;
    }

    /* 检查 pipeline 状态 */
    if (media_gateway_pipeline_get_ret(&res->pipeline) != 0)
    {
        LOG_ERROR("run_gateway_once failed: pipeline ret");
        return -1;
    }

    /* 策略只计算目标值，硬件变更通过命令队列异步交给资源所属线程执行。 */
    media_gateway_update_runtime_policies_if_due(ctx);
    media_gateway_apply_output_pacing_rates(ctx);
    drive_runtime_video_control_transition(ctx, res);

    pthread_mutex_lock(&ctx->stats_lock);
    media_gateway_log_throughput_if_due(ctx);
    pthread_mutex_unlock(&ctx->stats_lock);
    if (!*got_frame)
        usleep(1000);
    return 0;
}

/**
 * @description: 清理 run 生命周期内创建的短期资源。
 */
static void cleanup_run_resources(MediaGatewayCtx *ctx, MediaGatewayRunResources *res)
{
    int source_idx = 0;

    ctx->running = 0;
    if (res->audio_source_inited)
        audio_frame_source_deinit(&res->audio_source);
    for (source_idx = 0; source_idx < MEDIA_GATEWAY_MAX_CAPTURE_SOURCES; ++source_idx)
    {
        if (res->frame_source_inited[source_idx])
            media_frame_source_deinit(&res->frame_sources[source_idx]);
    }
    media_gateway_pipeline_join_workers(&res->pipeline);
    media_gateway_pipeline_deinit(&res->pipeline);
    if (res->result_queue_inited)
    {
        thread_message_queue_deinit(&res->result_queue);
        res->result_queue_inited = 0;
    }
}

/**
 * @description: 启动 gateway 运行循环，负责采集帧分发和 worker 生命周期管理。
 */
int media_gateway_run(MediaGatewayCtx *ctx)
{
    MediaGatewayRunResources res = {0};
    int ret = 0;
    int got_frame = 0;

    if (!ctx || !ctx->running)
    {
        LOG_ERROR("media_gateway_run failed: invalid ctx or not running");
        return -1;
    }

    res.video_control_transition.next_request_id = 0;

    /* 初始化所有 worker 共用的执行结果队列 */
    if (thread_message_queue_init(&res.result_queue,
                                  MEDIA_GATEWAY_RESULT_QUEUE_CAPACITY) != 0)
    {
        LOG_ERROR("media_gateway_run failed: init result queue");
        ret = -1;
        goto out;
    }
    res.result_queue_inited = 1;

    /* 初始化各路流的对应编码pipeline的输入队列和控制命令队列 */
    if (media_gateway_pipeline_init(&res.pipeline, ctx, &res.result_queue) != 0)
    {
        LOG_ERROR("media_gateway_run failed: init pipeline");
        ret = -1;
        goto out;
    }
    /* 启动所有的视频编码线程和音频编码线程 */
    if (media_gateway_pipeline_start_workers(&res.pipeline) != 0)
    {
        LOG_ERROR("media_gateway_run failed: start pipeline workers");
        ret = -1;
        goto out;
    }
    /* 启动视频采集线程 */
    if (start_run_frame_sources(ctx, &res) != 0)
    {
        ret = -1;
        goto out;
    }
    /* 启动音频采集线程 */
    if (start_run_audio_source(ctx, &res) != 0)
    {
        ret = -1;
        goto out;
    }

    /* 进入主循环 */
    while (ctx->running)
    {
        got_frame = 0;
        if (run_gateway_once(ctx, &res, &got_frame) != 0)
        {
            ret = -1;
            break;
        }
    }

out:
    cleanup_run_resources(ctx, &res);
    return ret;
}

/**
 * @description: 请求 gateway 运行循环退出。
 */
void media_gateway_stop(MediaGatewayCtx *ctx)
{
    if (!ctx)
        return;
    ctx->running = 0;
}

/**
 * @description: 释放 media gateway 初始化阶段持有的全部资源。
 */
void media_gateway_deinit(MediaGatewayCtx *ctx)
{
    int i = 0;

    if (!ctx)
        return;

    stop_outputs(ctx);
    deinit_outputs(ctx);

    if (ctx->record_fp)
    {
        fflush(ctx->record_fp);
        fclose(ctx->record_fp);
        ctx->record_fp = NULL;
    }
    if (ctx->audio_encoder_ready)
    {
        if (ctx->config.audio.source.codec == MEDIA_CODEC_AAC)
            aac_encoder_deinit(&ctx->aac_encoder);
        else
            g711_encoder_deinit(&ctx->audio_encoder);
        ctx->audio_encoder_ready = 0;
    }
    if (ctx->audio_capture_ready)
    {
        audio_capture_deinit(&ctx->audio_capture);
        ctx->audio_capture_ready = 0;
    }
    for (i = 0; i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        if (ctx->encoder_ready[i])
        {
            mpp_encoder_deinit(&ctx->encoders[i]);
            ctx->encoder_ready[i] = 0;
        }
        free(ctx->scaled_frame_cache[i]);
        ctx->scaled_frame_cache[i] = NULL;
        ctx->scaled_frame_cache_size[i] = 0;
    }
    for (i = 0; i < MEDIA_GATEWAY_MAX_CAPTURE_SOURCES; ++i)
    {
        if (ctx->capture_ready[i])
        {
            v4l2_capture_deinit(&ctx->captures[i]);
            ctx->capture_ready[i] = 0;
        }
    }
    if (ctx->isp_ready || ctx->isp.initialized || ctx->isp.status_lock_ready)
    {
        isp_controller_deinit(&ctx->isp);
        ctx->isp_ready = 0;
    }
    if (ctx->stats_lock_ready)
    {
        pthread_mutex_destroy(&ctx->stats_lock);
        ctx->stats_lock_ready = 0;
    }
    memset(&ctx->config, 0, sizeof(ctx->config));
    ctx->running = 0;
}

