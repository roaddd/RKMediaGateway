#include "mediaGatewayProcess.h"

#include "mediaGatewayClock.h"
#include "mediaGatewayMetrics.h"

#include "logger.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
    SCALE_PATH_ISP_DIRECT = 0,
    SCALE_PATH_RGA = 1,
    SCALE_PATH_CPU_NEAREST = 2
} ScalePath;

#if defined(ENABLE_RGA_SCALER)
__attribute__((weak)) int media_gateway_rga_scale_nv12(const uint8_t *src,
                                                       int src_w,
                                                       int src_h,
                                                       uint8_t *dst,
                                                       int dst_w,
                                                       int dst_h);
#endif

/**
 * @description: 确保指定 stream 的缩放缓存容量足够。
 */
static int ensure_scaled_frame_cache(MediaGatewayCtx *ctx, int stream_idx, size_t need_size)
{
    uint8_t *new_buf;
    if (!ctx || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("ensure_scaled_frame_cache failed: invalid args ctx=%p stream=%d need=%zu",
                  (void *)ctx,
                  stream_idx,
                  need_size);
        return -1;
    }
    if (ctx->scaled_frame_cache_size[stream_idx] >= need_size)
        return 0;

    new_buf = (uint8_t *)realloc(ctx->scaled_frame_cache[stream_idx], need_size);
    if (!new_buf)
    {
        LOG_ERROR("realloc scaled frame cache failed stream=%d need=%zu", stream_idx, need_size);
        return -1;
    }
    ctx->scaled_frame_cache[stream_idx] = new_buf;
    ctx->scaled_frame_cache_size[stream_idx] = need_size;
    return 0;
}

/**
 * @description: CPU nearest-neighbor NV12 缩放 fallback。
 */
static int scale_nv12_nearest(const uint8_t *src,
                              int src_w,
                              int src_h,
                              uint8_t *dst,
                              int dst_w,
                              int dst_h)
{
    const uint8_t *src_y;
    const uint8_t *src_uv;
    uint8_t *dst_y;
    uint8_t *dst_uv;
    int x;
    int y;

    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
    {
        LOG_ERROR("scale_nv12_nearest failed: invalid args src=%p dst=%p src=%dx%d dst=%dx%d",
                  (const void *)src,
                  (void *)dst,
                  src_w,
                  src_h,
                  dst_w,
                  dst_h);
        return -1;
    }
    if ((src_w & 1) || (src_h & 1) || (dst_w & 1) || (dst_h & 1))
    {
        LOG_ERROR("scale_nv12_nearest failed: odd dimensions src=%dx%d dst=%dx%d",
                  src_w,
                  src_h,
                  dst_w,
                  dst_h);
        return -1;
    }

    src_y = src;
    src_uv = src + (size_t)src_w * src_h;
    dst_y = dst;
    dst_uv = dst + (size_t)dst_w * dst_h;

    for (y = 0; y < dst_h; ++y)
    {
        int sy = (y * src_h) / dst_h;
        const uint8_t *src_line = src_y + (size_t)sy * src_w;
        uint8_t *dst_line = dst_y + (size_t)y * dst_w;
        for (x = 0; x < dst_w; ++x)
        {
            int sx = (x * src_w) / dst_w;
            dst_line[x] = src_line[sx];
        }
    }

    for (y = 0; y < dst_h / 2; ++y)
    {
        int sy = (y * (src_h / 2)) / (dst_h / 2);
        const uint8_t *src_line = src_uv + (size_t)sy * src_w;
        uint8_t *dst_line = dst_uv + (size_t)y * dst_w;
        for (x = 0; x < dst_w; x += 2)
        {
            int sx = ((x / 2) * (src_w / 2)) / (dst_w / 2);
            dst_line[x] = src_line[sx * 2];
            dst_line[x + 1] = src_line[sx * 2 + 1];
        }
    }
    return 0;
}

/**
 * @description: 如果集成了 RGA hook，则尝试使用 RGA 缩放 NV12。
 */
static int scale_nv12_rga_if_available(const uint8_t *src,
                                       int src_w,
                                       int src_h,
                                       uint8_t *dst,
                                       int dst_w,
                                       int dst_h)
{
#if defined(ENABLE_RGA_SCALER)
    if (media_gateway_rga_scale_nv12)
        return media_gateway_rga_scale_nv12(src, src_w, src_h, dst, dst_w, dst_h);
#endif
    (void)src;
    (void)src_w;
    (void)src_h;
    (void)dst;
    (void)dst_w;
    (void)dst_h;
    return -1;
}

/**
 * @description: 为指定 stream 准备编码输入，必要时执行缩放。
 */
static int prepare_stream_encode_input(MediaGatewayCtx *ctx,
                                       int stream_idx,
                                       const uint8_t *raw_frame,
                                       size_t raw_len,
                                       const uint8_t **encode_input,
                                       size_t *encode_input_len,
                                       ScalePath *path_used)
{
    const MediaGatewayStreamConfig *stream_cfg;
    size_t scaled_len;

    if (!ctx || !raw_frame || !encode_input || !encode_input_len || !path_used)
    {
        LOG_ERROR("prepare_stream_encode_input failed: invalid args ctx=%p raw=%p encode_input=%p len=%p path=%p",
                  (void *)ctx,
                  (const void *)raw_frame,
                  (void *)encode_input,
                  (void *)encode_input_len,
                  (void *)path_used);
        return -1;
    }
    if (stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("prepare_stream_encode_input failed: invalid stream=%d", stream_idx);
        return -1;
    }

    stream_cfg = &ctx->config.video.streams[stream_idx];

    {
        int source_idx = stream_cfg->source_index;
        int capture_width;
        int capture_height;

        if (source_idx < 0 || source_idx >= ctx->config.input.capture_source_count)
        {
            LOG_ERROR("prepare_stream_encode_input failed: invalid source=%d stream=%d source_count=%d",
                      source_idx,
                      stream_idx,
                      ctx->config.input.capture_source_count);
            return -1;
        }
        capture_width = ctx->config.input.capture_sources[source_idx].width;
        capture_height = ctx->config.input.capture_sources[source_idx].height;

        if (stream_cfg->width == capture_width && stream_cfg->height == capture_height)
        {
            *encode_input = raw_frame;
            *encode_input_len = raw_len;
            *path_used = SCALE_PATH_ISP_DIRECT;
            return 0;
        }

        scaled_len = (size_t)stream_cfg->width * stream_cfg->height * 3 / 2;
        if (ensure_scaled_frame_cache(ctx, stream_idx, scaled_len) != 0)
        {
            LOG_ERROR("prepare_stream_encode_input failed: ensure scale cache stream=%d size=%zu",
                      stream_idx,
                      scaled_len);
            return -1;
        }

        if (scale_nv12_rga_if_available(raw_frame,
                                        capture_width,
                                        capture_height,
                                        ctx->scaled_frame_cache[stream_idx],
                                        stream_cfg->width,
                                        stream_cfg->height) == 0)
        {
            *encode_input = ctx->scaled_frame_cache[stream_idx];
            *encode_input_len = scaled_len;
            *path_used = SCALE_PATH_RGA;
            return 0;
        }

        if (scale_nv12_nearest(raw_frame,
                               capture_width,
                               capture_height,
                               ctx->scaled_frame_cache[stream_idx],
                               stream_cfg->width,
                               stream_cfg->height) != 0)
        {
            LOG_ERROR("prepare_stream_encode_input failed: CPU scale stream=%d src=%dx%d dst=%dx%d",
                      stream_idx,
                      capture_width,
                      capture_height,
                      stream_cfg->width,
                      stream_cfg->height);
            return -1;
        }

        *encode_input = ctx->scaled_frame_cache[stream_idx];
        *encode_input_len = scaled_len;
        *path_used = SCALE_PATH_CPU_NEAREST;
        return 0;
    }
}

/**
 * @description: 将 stream 配置转换为 MPP 编码器选项。
 */
static void build_encoder_options(const MediaGatewayStreamConfig *cfg, MppEncoderOptions *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->rc_mode = (MppEncoderRcMode)cfg->rc_mode;
    opt->h264_profile = (MppEncoderH264Profile)cfg->h264_profile;
    opt->h264_level = (MppEncoderH264Level)cfg->h264_level;
    opt->h264_cabac_en = (MppEncoderCabacMode)cfg->h264_cabac_en;
    opt->qp_init = cfg->qp_init;
    opt->qp_min = cfg->qp_min;
    opt->qp_max = cfg->qp_max;
    opt->qp_min_i = cfg->qp_min_i;
    opt->qp_max_i = cfg->qp_max_i;
    opt->qp_max_step = cfg->qp_max_step;
}

/**
 * @description: 使用 V4L2 协商结果覆盖 MPP 输入 stride，优先尝试 DMA-BUF 直通。
 */
static void apply_capture_layout_encoder_options(MediaGatewayCtx *ctx, int stream_idx, MppEncoderOptions *opt)
{
    const MediaGatewayStreamConfig *stream_cfg;
    const V4L2CaptureFormat *capture_format;
    int source_idx;

    if (!ctx || !opt || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
        return;

    stream_cfg = &ctx->config.video.streams[stream_idx];
    source_idx = stream_cfg->source_index;
    if (source_idx < 0 || source_idx >= ctx->config.input.capture_source_count || !ctx->capture_ready[source_idx])
        return;

    capture_format = &ctx->captures[source_idx].format;
    if (capture_format->pixelformat != V4L2_PIX_FMT_NV12 ||
        capture_format->num_planes != 1 ||
        stream_cfg->width != capture_format->width ||
        stream_cfg->height != capture_format->height ||
        capture_format->planes[0].bytesperline == 0)
    {
        return;
    }

    /*
     * 尝试让 MPP 按 V4L2 single-plane NV12 的真实布局解释 DMA-BUF。
     * main 1920x1080 的 V4L2 sizeimage 是紧凑 NV12，因此 ver_stride 先用
     * 有效高度 1080，而不是默认对齐到 1088。
     */
    opt->input_hor_stride = (int)capture_format->planes[0].bytesperline;
    opt->input_ver_stride = capture_format->height;
    LOG_INFO("stream=%d use capture layout for MPP input: size=%dx%d stride=%dx%d sizeimage=%u",
             stream_idx,
             capture_format->width,
             capture_format->height,
             opt->input_hor_stride,
             opt->input_ver_stride,
             capture_format->planes[0].sizeimage);
}

/**
 * @description: 按当前 stream 配置重建 MPP 编码器。
 */
int media_gateway_reset_encoder(MediaGatewayCtx *ctx, int stream_idx)
{
    MppEncoderOptions options;
    const MediaGatewayStreamConfig *stream_cfg;
    if (!ctx || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
    {
        LOG_ERROR("media_gateway_reset_encoder failed: invalid args ctx=%p stream=%d",
                  (void *)ctx,
                  stream_idx);
        return -1;
    }

    stream_cfg = &ctx->config.video.streams[stream_idx];
    build_encoder_options(stream_cfg, &options);
    apply_capture_layout_encoder_options(ctx, stream_idx, &options);
    if (ctx->encoder_ready[stream_idx])
    {
        mpp_encoder_deinit(&ctx->encoders[stream_idx]);
        ctx->encoder_ready[stream_idx] = 0;
    }
    if (mpp_encoder_init(&ctx->encoders[stream_idx],
                         stream_cfg->width,
                         stream_cfg->height,
                         stream_cfg->fps,
                         stream_cfg->bitrate,
                         stream_cfg->gop,
                         &options) != 0)
    {
        LOG_ERROR("media_gateway_reset_encoder failed: mpp init stream=%d size=%dx%d fps=%d bitrate=%d gop=%d",
                  stream_idx,
                  stream_cfg->width,
                  stream_cfg->height,
                  stream_cfg->fps,
                  stream_cfg->bitrate,
                  stream_cfg->gop);
        return -1;
    }
    ctx->encoder_ready[stream_idx] = 1;
    return 0;
}

/**
 * @description: 消费外部输出通道的 IDR 请求并通知编码器。
 */
static void trigger_external_idr_if_needed(MediaGatewayCtx *ctx, int stream_idx)
{
    int output_idx;
    int need_idr = 0;
    if (!ctx || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
        return;

    /*
     * 按码流遍历所有输出通道，而不是只检查 RTSP 和 GB28181。
     * WebRTC 新 video Track 就绪后也会请求 IDR；统一扫描可避免
     * 为每种新协议在 MediaGatewayCtx 中维护一套专用 output 索引。
     */
    for (output_idx = 0; output_idx < ctx->output_count; ++output_idx)
    {
        if (ctx->output_stream_index[output_idx] != stream_idx) {
            continue;
        }
        if (media_output_consume_external_idr_request(&ctx->outputs[output_idx])) {
            need_idr = 1;
        }
    }

    if (need_idr)
    {
        if (mpp_encoder_request_idr(&ctx->encoders[stream_idx]) != 0)
            LOG_WARN("stream=%d failed to request IDR from external output event", stream_idx);
    }
}

/**
 * @description: 准备单帧编码输入并记录缩放 fallback 告警。
 */
static int ensure_stream_input(MediaGatewayCtx *ctx,
                               MediaGatewayRunState *state,
                               int stream_idx,
                               const MediaFrame *frame,
                               const uint8_t **encode_input,
                               size_t *encode_input_len)
{
    ScalePath scale_path = SCALE_PATH_ISP_DIRECT;
    const MediaGatewayStreamConfig *stream_cfg = &ctx->config.video.streams[stream_idx];

    if (prepare_stream_encode_input(ctx,
                                    stream_idx,
                                    frame->raw_frame,
                                    (size_t)frame->raw_len,
                                    encode_input,
                                    encode_input_len,
                                    &scale_path) != 0)
    {
        LOG_ERROR("media_gateway_run failed: prepare_stream_encode_input stream=%d name=%s",
                  stream_idx,
                  stream_cfg->name ? stream_cfg->name : "unknown");
        return -1;
    }

    if (scale_path == SCALE_PATH_CPU_NEAREST && !state->rga_fallback_warned[stream_idx])
    {
        LOG_WARN("stream=%d fallback to CPU nearest scaler (RGA unavailable or failed)",
                 stream_idx);
        state->rga_fallback_warned[stream_idx] = 1;
    }
    return 0;
}

/**
 * @description: 检查当前 V4L2 DMA-BUF 是否与 MPP 输入布局满足直通条件。
 */
static int can_encode_stream_dmabuf_direct(const MediaGatewayCtx *ctx,
                                           int stream_idx,
                                           const MediaFrame *frame,
                                           int dmabuf_fd,
                                           size_t dmabuf_size,
                                           const char **reason)
{
    const MediaGatewayStreamConfig *stream_cfg;
    const V4L2CaptureFormat *capture_format;
    const MppEncoderCtx *encoder;
    size_t mpp_frame_size;

    if (reason)
        *reason = "invalid arguments";
    if (!ctx || !frame || stream_idx < 0 || stream_idx >= MEDIA_GATEWAY_MAX_STREAMS)
        return 0;

    stream_cfg = &ctx->config.video.streams[stream_idx];
    encoder = &ctx->encoders[stream_idx];
    if (!frame->capture_buffer || !frame->capture_buffer->capture)
    {
        if (reason) *reason = "frame has no retained capture buffer";
        return 0;
    }
    if (dmabuf_fd < 0)
    {
        if (reason) *reason = "capture buffer has no exported dmabuf fd";
        return 0;
    }

    capture_format = &frame->capture_buffer->capture->format;
    if (capture_format->pixelformat != V4L2_PIX_FMT_NV12)
    {
        if (reason) *reason = "capture pixelformat is not NV12";
        return 0;
    }
    if (capture_format->num_planes != 1)
    {
        if (reason) *reason = "capture NV12 is not single-plane layout";
        return 0;
    }
    if (stream_cfg->width != capture_format->width ||
        stream_cfg->height != capture_format->height ||
        encoder->input.width != capture_format->width ||
        encoder->input.height != capture_format->height)
    {
        if (reason) *reason = "capture and encoder effective size differ";
        return 0;
    }
    if ((int)capture_format->planes[0].bytesperline != encoder->input.hor_stride)
    {
        if (reason) *reason = "capture bytesperline differs from MPP hor_stride";
        return 0;
    }

    /*
     * MPP 按自己的 hor_stride/ver_stride 解释外部 NV12。当前 single-plane
     * V4L2 协商结果能直接给出行步进和容量，先要求两边容量覆盖 MPP 读区间，
     * 避免 compact 1080p buffer 被按 1088 高度布局直通。
     */
    mpp_frame_size = (size_t)encoder->input.hor_stride * encoder->input.ver_stride * 3 / 2;
    if ((size_t)capture_format->planes[0].sizeimage < mpp_frame_size)
    {
        if (reason) *reason = "capture sizeimage is smaller than MPP stride frame";
        return 0;
    }
    if (dmabuf_size < mpp_frame_size)
    {
        if (reason) *reason = "dmabuf size is smaller than MPP stride frame";
        return 0;
    }

    if (reason)
        *reason = "layout matched";
    return 1;
}

/**
 * @description: 编码一帧视频，并在连续失败时重建编码器。
 */
static int encode_stream_frame(MediaGatewayCtx *ctx,
                               MediaGatewayRunState *state,
                               int stream_idx,
                               const MediaFrame *frame,
                               const uint8_t *encode_input,
                               size_t encode_input_len,
                               uint8_t **h264_data,
                               size_t *h264_len,
                               int *is_key_frame,
                               uint64_t *encode_start_ts_us,
                               uint64_t *encode_done_ts_us,
                               MppEncoderTiming *encoder_timing)
{
    const MediaGatewayStreamConfig *stream_cfg = &ctx->config.video.streams[stream_idx];
    int dmabuf_fd = frame->capture_buffer ? v4l2_capture_buffer_dmabuf_fd(frame->capture_buffer) : -1;
    size_t dmabuf_size = frame->capture_buffer ? v4l2_capture_buffer_size(frame->capture_buffer) : 0;
    const char *dmabuf_direct_reason = NULL;
    int dmabuf_direct = can_encode_stream_dmabuf_direct(ctx,
                                                        stream_idx,
                                                        frame,
                                                        dmabuf_fd,
                                                        dmabuf_size,
                                                        &dmabuf_direct_reason);
    /* 如果有输出通道需要请求IDR帧，则通知编码器 */
    trigger_external_idr_if_needed(ctx, stream_idx);

    if (!state->dmabuf_direct_logged[stream_idx])
    {
        if (dmabuf_direct)
        {
            const V4L2CaptureFormat *format = &frame->capture_buffer->capture->format;
            LOG_WARN("DMA-BUF direct enabled stream=%d name=%s size=%dx%d bytesperline=%u sizeimage=%u mpp_stride=%dx%d",
                     stream_idx,
                     stream_cfg->name ? stream_cfg->name : "unknown",
                     format->width,
                     format->height,
                     format->planes[0].bytesperline,
                     format->planes[0].sizeimage,
                     ctx->encoders[stream_idx].input.hor_stride,
                     ctx->encoders[stream_idx].input.ver_stride);
        }
        else
        {
            LOG_WARN("DMA-BUF direct disabled stream=%d name=%s reason=%s; fallback to copy encode",
                     stream_idx,
                     stream_cfg->name ? stream_cfg->name : "unknown",
                     dmabuf_direct_reason ? dmabuf_direct_reason : "unknown");
        }
        state->dmabuf_direct_logged[stream_idx] = 1;
    }

    /* Only the matched V4L2 layout can bypass the copy encode path. */
    if (dmabuf_direct &&
        mpp_encoder_encode_dmabuf(&ctx->encoders[stream_idx],
                                  dmabuf_fd,
                                  dmabuf_size,
                                  frame->frame_id,
                                  h264_data,
                                  h264_len,
                                  is_key_frame,
                                  encode_start_ts_us,
                                  encode_done_ts_us,
                                  encoder_timing) == 0)
    {
        state->consecutive_encode_fail[stream_idx] = 0;
        return 0;
    }

    if (mpp_encoder_encode_frame(&ctx->encoders[stream_idx],
                                 encode_input,
                                 encode_input_len,
                                 frame->frame_id,
                                 h264_data,
                                 h264_len,
                                 is_key_frame,
                                 encode_start_ts_us,
                                 encode_done_ts_us,
                                 encoder_timing) == 0)
    {
        state->consecutive_encode_fail[stream_idx] = 0;
        return 0;
    }

    state->consecutive_encode_fail[stream_idx]++;
    if (state->consecutive_encode_fail[stream_idx] >= 3)
    {
        if (media_gateway_reset_encoder(ctx, stream_idx) != 0)
        {
            LOG_ERROR("media_gateway_run failed: reset_encoder stream=%d name=%s",
                      stream_idx,
                      stream_cfg->name ? stream_cfg->name : "unknown");
            return -1;
        }
        state->consecutive_encode_fail[stream_idx] = 0;
    }
    return 1;
}

/**
 * @description: 将 H264 视频包分发到当前 stream 绑定的所有输出。
 */
static int enqueue_stream_packet(MediaGatewayCtx *ctx,
                                 int stream_idx,
                                 const MediaFrame *frame,
                                 uint8_t *h264_data,
                                 size_t h264_len,
                                 int is_key_frame,
                                 uint64_t encode_start_ts_us,
                                 const MppEncoderTiming *encoder_timing)
{
    MediaBuffer *buffer = NULL;
    MediaPacket packet;
    int i;
    int output_hit = 0;

    /* 从media_buffer_pool中获取buffer */
    if (media_buffer_create_copy(h264_data, h264_len, &buffer) != 0)
    {
        LOG_ERROR("media_gateway_run failed: media_buffer_create_copy stream=%d size=%zu",
                  stream_idx,
                  h264_len);
        return -1;
    }

    media_packet_init(&packet);
    packet.frame_type = MEDIA_FRAME_TYPE_VIDEO;
    packet.codec = MEDIA_CODEC_H264;
    packet.buffer = buffer;
    packet.frame_id = frame->frame_id;
    /*
     * MediaPacket 的 PTS/DTS 是输出层媒体时间戳，单位是微秒。
     * 这里使用 V4L2 DQBUF 的驱动时间戳，后续 RTSP 会将 pts_us 换算成 RTP timestamp，
     * 播放端依赖它判断播放节奏、帧顺序和音视频同步。
     */
    packet.pts_us = frame->dqbuf_ts_us;
    /*
     * 当前 H264 编码链路不使用 B 帧，编码顺序和显示顺序一致，因此 DTS 与 PTS 相同。
     */
    packet.dts_us = frame->dqbuf_ts_us;
    packet.path_metrics.enqueue_ts_us = media_gateway_get_now_us();
    packet.path_metrics.dqbuf_to_encode_start_us = (encode_start_ts_us >= frame->dqbuf_ts_us)
                                                       ? (encode_start_ts_us - frame->dqbuf_ts_us)
                                                       : 0;
    packet.path_metrics.encode_us = encoder_timing ? encoder_timing->encode_frame_total_us : 0;
    packet.path_metrics.stream_name = ctx->config.video.streams[stream_idx].name;
    /* 打印本包路径延时的条件：1.配置开启了统计选项；2.采样间隔帧数大于0；3.当前采集帧号是采样间隔帧数的倍数 */
    packet.path_metrics.sample = ctx->bench.enable &&
                                 ctx->bench.sample_every > 0 &&
                                 ((frame->frame_id % (uint64_t)ctx->bench.sample_every) == 0);
    packet.is_key_frame = is_key_frame;
    media_gateway_metrics_log_frame_trace(ctx, stream_idx, frame, &packet, h264_len);

    for (i = 0; i < ctx->output_count; ++i)
    {
        /* 检查输出通道是否绑定到当前流 */
        if (ctx->output_stream_index[i] != stream_idx)
            continue;
        output_hit = 1;
        media_output_enqueue(&ctx->outputs[i], &packet);
    }

    if (output_hit)
    {
        ctx->stats.streams[stream_idx].frames++;
        ctx->stats.streams[stream_idx].bytes += h264_len;
    }
    media_packet_reset(&packet);
    return 0;
}

/**
 * @description: 将编码后的音频包分发到支持音频的输出。
 */
static int enqueue_audio_packet(MediaGatewayCtx *ctx,
                                int stream_idx,
                                const AudioFrame *frame,
                                const uint8_t *audio_data,
                                size_t audio_len,
                                uint64_t pts_us,
                                MediaCodecType codec)
{
    MediaBuffer *buffer = NULL;
    MediaPacket packet;
    int i;
    int output_hit = 0;

    if (!ctx || !frame || !audio_data || audio_len == 0)
    {
        LOG_ERROR("enqueue_audio_packet failed: invalid args ctx=%p frame=%p audio=%p len=%zu",
                  (void *)ctx,
                  (const void *)frame,
                  (const void *)audio_data,
                  audio_len);
        return -1;
    }
    if (stream_idx < 0 || stream_idx >= ctx->config.video.stream_count)
    {
        LOG_ERROR("enqueue_audio_packet failed: invalid stream=%d stream_count=%d",
                  stream_idx,
                  ctx->config.video.stream_count);
        return -1;
    }
    /* 从media_buffer_pool中获取buffer */
    if (media_buffer_create_copy(audio_data, audio_len, &buffer) != 0)
    {
        LOG_ERROR("enqueue_audio_packet failed: media_buffer_create_copy stream=%d size=%zu",
                  stream_idx,
                  audio_len);
        return -1;
    }

    media_packet_init(&packet);
    packet.frame_type = MEDIA_FRAME_TYPE_AUDIO; /* 区分该帧是音频帧 */
    packet.codec = codec;
    packet.buffer = buffer;
    packet.frame_id = frame->frame_id;
    packet.pts_us = pts_us;
    packet.dts_us = pts_us;
    packet.is_key_frame = 0;

    for (i = 0; i < ctx->output_count; ++i)
    {
        if (ctx->output_stream_index[i] != stream_idx)
            continue;
        if (ctx->outputs[i].type != MEDIA_OUTPUT_TYPE_RTSP &&
            ctx->outputs[i].type != MEDIA_OUTPUT_TYPE_RTMP &&
            ctx->outputs[i].type != MEDIA_OUTPUT_TYPE_GB28181 &&
            ctx->outputs[i].type != MEDIA_OUTPUT_TYPE_WEBRTC)
        {
            continue;
        }
        output_hit = 1;
        media_output_enqueue(&ctx->outputs[i], &packet);
    }

    if (output_hit)
    {
        ctx->stats.audio.frames++;
        ctx->stats.audio.bytes += audio_len;
    }
    media_packet_reset(&packet);
    return 0;
}

/**
 * @description: 处理一帧 PCM 音频，完成 G711 编码和输出分发。
 */
int media_gateway_process_audio(MediaGatewayCtx *ctx, const AudioFrame *frame)
{
    const uint8_t *audio_data = NULL;
    size_t audio_len = 0;
    uint64_t audio_pts_us = 0;
    MediaCodecType codec = MEDIA_CODEC_NONE;
    int stream_idx;
    int ret;

    if (!ctx || !frame || !frame->data || frame->size == 0)
    {
        LOG_ERROR("media_gateway_process_audio failed: invalid args ctx=%p frame=%p data=%p size=%zu",
                  (void *)ctx,
                  (const void *)frame,
                  frame ? (const void *)frame->data : NULL,
                  frame ? frame->size : 0);
        return -1;
    }
    if (!ctx->audio_encoder_ready)
        return 0;
    if (frame->format != AUDIO_SAMPLE_FORMAT_S16LE)
    {
        LOG_ERROR("process_gateway_audio failed: unsupported format=%d channels=%d",
                  frame->format,
                  frame->channels);
        return -1;
    }
    /* 确定音频数据绑定的流索引,因为音频需要跟某一路视频时间线绑定 */
    stream_idx = ctx->config.audio.source.bind_stream_index;
    if (stream_idx < 0 || stream_idx >= ctx->config.video.stream_count || !ctx->stream_enabled[stream_idx])
        return 0;

    if (ctx->config.audio.source.encoder.codec == MEDIA_CODEC_AAC)
    {
        if (aac_encoder_encode_s16le(&ctx->aac_encoder,
                                     (const int16_t *)frame->data,
                                     frame->samples_per_channel,
                                     frame->pts_us,
                                     &audio_data,
                                     &audio_len,
                                     &audio_pts_us,
                                     &codec) != 0)
        {
            LOG_ERROR("process_gateway_audio failed: aac encode frame=%" PRIu64, frame->frame_id);
            return -1;
        }
        if (audio_len == 0)
            return 0;
    }
    else if (ctx->config.audio.source.encoder.codec == MEDIA_CODEC_OPUS)
    {
        /* 输出为单个裸 Opus packet，WebRTC 层可直接放入一个 RTP payload。 */
        if (opus_audio_encoder_encode_s16le(&ctx->opus_encoder,
                                            (const int16_t *)frame->data,
                                            frame->samples_per_channel,
                                            &audio_data,
                                            &audio_len,
                                            &codec) != 0)
        {
            LOG_ERROR("process_gateway_audio failed: opus encode frame=%" PRIu64, frame->frame_id);
            return -1;
        }
        audio_pts_us = frame->pts_us;
    }
    else
    {
        if (frame->channels != 1)
        {
            LOG_ERROR("process_gateway_audio failed: G711 requires mono channels=%d", frame->channels);
            return -1;
        }
        if (g711_encoder_encode_s16le(&ctx->audio_encoder,
                                      (const int16_t *)frame->data,
                                      frame->samples_per_channel,
                                      &audio_data,
                                      &audio_len,
                                      &codec) != 0)
        {
            LOG_ERROR("process_gateway_audio failed: g711 encode frame=%" PRIu64, frame->frame_id);
            return -1;
        }
        audio_pts_us = frame->pts_us;
    }
    pthread_mutex_lock(&ctx->stats_lock);
    ret = enqueue_audio_packet(ctx, stream_idx, frame, audio_data, audio_len, audio_pts_us, codec);
    pthread_mutex_unlock(&ctx->stats_lock);
    return ret;
}

/**
 * @description: 按配置记录指定视频帧的 benchmark 样本。
 */
static void record_stream_benchmark(MediaGatewayCtx *ctx,
                                    int stream_idx,
                                    const MediaFrame *frame,
                                    uint64_t encode_start_ts_us,
                                    uint64_t encode_done_ts_us,
                                    const MppEncoderTiming *encoder_timing)
{
    uint64_t output_queued_ts_us = 0;
    uint64_t dqbuf_to_encode_start_us = 0;
    uint64_t encode_start_to_done_us = 0;
    uint64_t dqbuf_to_encode_done_us = 0;
    uint64_t dqbuf_to_output_queued_us = 0;

    if (!ctx->bench.enable)
        return;
    if ((frame->frame_id % (uint64_t)ctx->bench.sample_every) != 0)
        return;

    output_queued_ts_us = media_gateway_get_now_us();
    dqbuf_to_encode_start_us = (encode_start_ts_us >= frame->dqbuf_ts_us) ? (encode_start_ts_us - frame->dqbuf_ts_us) : 0;
    encode_start_to_done_us = (encode_done_ts_us >= encode_start_ts_us) ? (encode_done_ts_us - encode_start_ts_us) : 0;
    dqbuf_to_encode_done_us = (encode_done_ts_us >= frame->dqbuf_ts_us) ? (encode_done_ts_us - frame->dqbuf_ts_us) : 0;
    dqbuf_to_output_queued_us = (output_queued_ts_us >= frame->dqbuf_ts_us) ? (output_queued_ts_us - frame->dqbuf_ts_us) : 0;

    media_gateway_bench_record_sample(ctx,
                                      stream_idx,
                                      frame->metrics.camera_buffer_wait_us,
                                      frame->metrics.dqbuf_ioctl_duration_us,
                                      frame->metrics.capture_call_duration_us,
                                      frame->metrics.mmap_to_frame_cache_copy_us,
                                      frame->metrics.frame_source_publish_us,
                                      frame->metrics.frame_source_publish_copy_us,
                                      frame->metrics.video_input_publish_copy_us,
                                      frame->metrics.video_input_acquire_copy_us,
                                      dqbuf_to_encode_start_us,
                                      encode_start_to_done_us,
                                      encoder_timing,
                                      dqbuf_to_encode_done_us,
                                      dqbuf_to_output_queued_us);
}

/**
 * @description: 可选地将 stream 0 的 H264 数据写入本地文件。
 */
static void maybe_record_stream_file(MediaGatewayCtx *ctx,
                                     int stream_idx,
                                     uint64_t frame_id,
                                     const uint8_t *h264_data,
                                     size_t h264_len)
{
    size_t written;
    if (!ctx->record_fp || stream_idx != 0)
        return;

    written = fwrite(h264_data, 1, h264_len, ctx->record_fp);
    if (written != h264_len)
        LOG_WARN("local record write short: %zu/%zu", written, h264_len);
    if ((frame_id % (uint64_t)ctx->config.system.record.flush_interval_frames) == 0)
        fflush(ctx->record_fp);
}

/**
 * @description: 处理一帧视频，完成输入准备、编码、分发和统计。
 */
int media_gateway_process_stream(MediaGatewayCtx *ctx,
                                 MediaGatewayRunState *state,
                                 const MediaFrame *frame,
                                 int stream_idx)
{
    const uint8_t *encode_input = NULL;
    size_t encode_input_len = 0;
    uint8_t *h264_data = NULL;
    size_t h264_len = 0;
    int is_key_frame = 0;
    uint64_t encode_start_ts_us = 0;
    uint64_t encode_done_ts_us = 0;
    MppEncoderTiming encoder_timing = {0};
    int encode_ret;

    if (!ctx->stream_enabled[stream_idx])
        return 0;

    if (ensure_stream_input(ctx, state, stream_idx, frame, &encode_input, &encode_input_len) != 0)
    {
        LOG_ERROR("media_gateway_process_stream failed: ensure input stream=%d frame=%" PRIu64,
                  stream_idx,
                  frame ? frame->frame_id : 0);
        return -1;
    }

    encode_ret = encode_stream_frame(ctx,
                                     state,
                                     stream_idx,
                                     frame,
                                     encode_input,
                                     encode_input_len,
                                     &h264_data,
                                     &h264_len,
                                     &is_key_frame,
                                     &encode_start_ts_us,
                                     &encode_done_ts_us,
                                     &encoder_timing);
    if (encode_ret != 0)
    {
        LOG_ERROR("media_gateway_process_stream failed: encode stream=%d frame=%" PRIu64 " ret=%d",
                  stream_idx,
                  frame ? frame->frame_id : 0,
                  encode_ret);
        return (encode_ret < 0) ? -1 : 0;
    }

    if (!h264_data || h264_len == 0)
    {
        LOG_WARN("media_gateway_process_stream failed: empty encode output stream=%d frame=%" PRIu64,
                  stream_idx,
                  frame ? frame->frame_id : 0);
        return 0;
    }

    pthread_mutex_lock(&ctx->stats_lock);
    if (enqueue_stream_packet(ctx,
                              stream_idx,
                              frame,
                              h264_data,
                              h264_len,
                              is_key_frame,
                              encode_start_ts_us,
                              &encoder_timing) != 0)
    {
        LOG_ERROR("media_gateway_process_stream failed: enqueue stream=%d frame=%" PRIu64 " size=%zu key=%d",
                  stream_idx,
                  frame->frame_id,
                  h264_len,
                  is_key_frame);
        pthread_mutex_unlock(&ctx->stats_lock);
        return -1;
    }

    /*  */
    record_stream_benchmark(ctx, stream_idx, frame, encode_start_ts_us, encode_done_ts_us, &encoder_timing);
    // maybe_record_stream_file(ctx, stream_idx, frame->frame_id, h264_data, h264_len);
    pthread_mutex_unlock(&ctx->stats_lock);
    return 0;
}
