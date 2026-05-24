#include "mppEncoder.h"
#include "logger.h"
#include "mpp_meta.h"
#include "rk_mpi_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#define MPP_ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))

/**
 * @description: 输出 MPP 接口错误日志
 * @param {const char *} msg
 * @param {MPP_RET} ret
 * @return {static void}
 */
static void mpp_log_error(const char *msg, MPP_RET ret) {
    LOG_ERROR("%s: ret=%d", msg, ret);
}

/**
 * @description: 确保编码输出缓存空间足够
 * @param {MppEncoderCtx *} enc
 * @param {size_t} need_size
 * @return {static int}
 */
static int ensure_packet_cache(MppEncoderCtx *enc, size_t need_size) {
    // 输出码流长度会波动，按需扩容缓存，避免每帧都 malloc/free。
    if (enc->packet_cache_size >= need_size) {
        return 0;
    }

    uint8_t *new_buf = (uint8_t *)realloc(enc->packet_cache, need_size);
    if (!new_buf) {
        LOG_ERROR("ensure_packet_cache failed: realloc need=%zu", need_size);
        return -1;
    }

    enc->packet_cache = new_buf;
    enc->packet_cache_size = need_size;
    return 0;
}

/**
 * @description: 获取当前单调时钟时间，单位微秒
 * @return {static uint64_t}
 */
static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 将 NV12 数据拷贝到 MPP 输入缓冲区
 * @param {MppEncoderCtx *} enc
 * @param {uint8_t *} dst
 * @param {const uint8_t *} src
 * @return {static void}
 */
static void copy_nv12_to_mpp_buffer(MppEncoderCtx *enc, uint8_t *dst, const uint8_t *src) {
    size_t y_src_stride = (size_t)enc->width;
    size_t uv_src_stride = (size_t)enc->width;
    size_t y_dst_stride = (size_t)enc->hor_stride;
    size_t uv_dst_stride = (size_t)enc->hor_stride;

    const uint8_t *src_y = src;
    const uint8_t *src_uv = src + (size_t)enc->width * enc->height;
    uint8_t *dst_y = dst;
    uint8_t *dst_uv = dst + (size_t)enc->hor_stride * enc->ver_stride;

    // 先清空整块缓冲，避免未覆盖到的对齐区域出现脏数据。
    memset(dst, 0, (size_t)enc->hor_stride * enc->ver_stride * 3 / 2);

    for (int h = 0; h < enc->height; ++h) {
        memcpy(dst_y + (size_t)h * y_dst_stride, src_y + (size_t)h * y_src_stride, y_src_stride);
    }

    for (int h = 0; h < enc->height / 2; ++h) {
        memcpy(dst_uv + (size_t)h * uv_dst_stride, src_uv + (size_t)h * uv_src_stride, uv_src_stride);
    }
}

/**
 * @description: 初始化 MPP 编码器
 * @param {MppEncoderCtx *} enc
 * @param {int} width
 * @param {int} height
 * @param {int} fps
 * @param {int} bitrate
 * @param {int} gop
 * @param {const MppEncoderOptions *} options
 * @return {int}
 */
int mpp_encoder_init(MppEncoderCtx *enc, int width, int height, int fps, int bitrate, int gop, const MppEncoderOptions *options) {
    if (!enc || width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0 || gop <= 0) {
        LOG_ERROR("mpp_encoder_init failed: invalid params enc=%p size=%dx%d fps=%d bitrate=%d gop=%d",
                  (void *)enc,
                  width,
                  height,
                  fps,
                  bitrate,
                  gop);
        return -1;
    }

    memset(enc, 0, sizeof(*enc));
    enc->width = width;
    enc->height = height;
    enc->hor_stride = MPP_ALIGN(width, 16);
    enc->ver_stride = MPP_ALIGN(height, 16);
    enc->fps = fps;
    enc->bitrate = bitrate;
    enc->gop = gop;
    enc->pts = 0;

    // 1) 创建编码上下文并初始化为 H264 编码器。
    MPP_RET ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret != MPP_OK) {
        mpp_log_error("mpp_create failed", ret);
        return -1;
    }

    ret = mpp_init(enc->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) {
        mpp_log_error("mpp_init failed", ret);
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
        return -1;
    }

    // 2) 读取默认配置后覆盖关键参数（输入格式、码率控制、H264 细节）。
    mpp_enc_cfg_init(&enc->cfg);
    ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_CFG, enc->cfg);
    if (ret != MPP_OK) {
        mpp_log_error("MPP_ENC_GET_CFG failed", ret);
        mpp_encoder_deinit(enc);
        return -1;
    }

    mpp_enc_cfg_set_s32(enc->cfg, "prep:width", enc->width);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:height", enc->height);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:hor_stride", enc->hor_stride);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:ver_stride", enc->ver_stride);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:format", MPP_FMT_YUV420SP); /* 图像色彩空间格式以及内存排布方式 */
    /* Rate Control(RC) 模块 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:mode", (options && options->rc_mode > 0) ? options->rc_mode : MPP_ENC_RC_MODE_CBR); /* 码率控制模式 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:gop", enc->gop); /*两个I帧之间的间隔 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_flex", 0); /* 输入帧率是否可变, fps_in_flex=0 表示固定输入帧率 */

    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_num", enc->fps); /* 输入帧率分数值的分子部分，默认值为30 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_denorm", 1); /* 输入帧率分数值的分母部分，默认值为1 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_flex", 0); /* 输出帧率是否可变的标志位，默认为0,fps_out_flex=0 表示固定输出帧率 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_num", enc->fps); /* 输出帧率分数值的分子部分，默认值为30 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_denorm", 1); /* 输出帧率分数值的分母部分，默认值为1 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_target", enc->bitrate); /* 定码率(CBR)模式下的目标码率 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_max", enc->bitrate * 17 / 16); /* 变码率(VBR)和自适应码率模式(AVBR)下的最高码率 */
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_min", enc->bitrate * 15 / 16); /* 变码率(VBR)和自适应码率模式(AVBR)下的最低码率 */

    if (options) {
        if (options->qp_init > 0) {
            mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_init", options->qp_init); /* 初始QP值 */
        }
        if (options->qp_min > 0) {
            mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_min", options->qp_min); /* P、B帧的最小QP值 */
        }
        if (options->qp_max > 0) {
            mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_max", options->qp_max); /* P、B帧的最大QP值 */
        }
        if (options->qp_min_i > 0) {
            mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_min_i", options->qp_min_i); /* I帧的最小QP值 */
        }
        if (options->qp_max_i > 0) {
            mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_max_i", options->qp_max_i); /* I帧的最大QP值 */
        }
        if (options->qp_max_step > 0) {
            mpp_enc_cfg_set_s32(enc->cfg, "rc:qp_max_step", options->qp_max_step);
        }
    }
    mpp_enc_cfg_set_s32(enc->cfg, "codec:type", MPP_VIDEO_CodingAVC); /* 表示MppEncCodecCfg对应的协议类型，需要与MppCtx初始化函数mpp_init的参数一致 */
    // 低延时思路：
    // RTSP 推流链路按 Annex-B 拆 NALU 发包，强制编码器输出 Annex-B，
    // 避免格式不一致导致的解析/重组等待。
    mpp_enc_cfg_set_s32(enc->cfg, "h264:stream_type", 0);
    mpp_enc_cfg_set_s32(enc->cfg, "h264:profile", (options && options->h264_profile > 0) ? options->h264_profile : 100); /* SPS中的profile_idc参数 */
    mpp_enc_cfg_set_s32(enc->cfg, "h264:level", (options && options->h264_level > 0) ? options->h264_level : 40); /* SPS中的level_idc参数 */
    mpp_enc_cfg_set_s32(enc->cfg, "h264:cabac_en", (options && options->h264_cabac_en >= 0) ? options->h264_cabac_en : 1);

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, enc->cfg);
    if (ret != MPP_OK) {
        mpp_log_error("MPP_ENC_SET_CFG failed", ret);
        mpp_encoder_deinit(enc);
        return -1;
    }

    // 每个 IDR 前输出 SPS/PPS，便于后续 RTSP/网络场景中途加入观看端。
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret != MPP_OK) {
        mpp_log_error("MPP_ENC_SET_HEADER_MODE failed", ret);
        mpp_encoder_deinit(enc);
        return -1;
    }

    // 3) 申请输入帧缓冲。优先 DRM，失败回退 ION（兼容不同系统配置）。
    ret = mpp_buffer_group_get_internal(&enc->frame_group, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK) {
        ret = mpp_buffer_group_get_internal(&enc->frame_group, MPP_BUFFER_TYPE_ION);
        if (ret != MPP_OK) {
            mpp_log_error("mpp_buffer_group_get_internal failed", ret);
            mpp_encoder_deinit(enc);
            return -1;
        }
    }

    size_t frame_size = (size_t)enc->hor_stride * enc->ver_stride * 3 / 2;
    ret = mpp_buffer_get(enc->frame_group, &enc->frame_buffer, frame_size);
    if (ret != MPP_OK) {
        mpp_log_error("mpp_buffer_get failed", ret);
        mpp_encoder_deinit(enc);
        return -1;
    }

    // 4) 初始化 MppFrame 元数据，后续每帧只更新数据和 pts 即可。
    ret = mpp_frame_init(&enc->frame);
    if (ret != MPP_OK) {
        mpp_log_error("mpp_frame_init failed", ret);
        mpp_encoder_deinit(enc);
        return -1;
    }

    mpp_frame_set_width(enc->frame, enc->width);
    mpp_frame_set_height(enc->frame, enc->height);
    mpp_frame_set_hor_stride(enc->frame, enc->hor_stride);
    mpp_frame_set_ver_stride(enc->frame, enc->ver_stride);
    mpp_frame_set_fmt(enc->frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(enc->frame, enc->frame_buffer);

    printf("[INFO] mpp encoder input format: fmt=%d width=%d height=%d hor_stride=%d ver_stride=%d frame_size=%zu\n",
           MPP_FMT_YUV420SP,
           enc->width,
           enc->height,
           enc->hor_stride,
           enc->ver_stride,
           frame_size);
    printf("[INFO] mpp encoder init success: %dx%d fps=%d bitrate=%d gop=%d\n",
           enc->width, enc->height, enc->fps, enc->bitrate, enc->gop);
    return 0;
}

/**
 * @description: 编码一帧原始图像为 H264 数据
 * @param {MppEncoderCtx *} enc
 * @param {const uint8_t *} nv12_data
 * @param {size_t} nv12_len
 * @param {uint64_t} frame_id
 * @param {uint8_t **} h264_data
 * @param {size_t *} h264_len
 * @param {int *} is_key_frame
 * @param {uint64_t *} encode_start_ts_us
 * @param {uint64_t *} encode_done_ts_us
 * @return {int}
 */
int mpp_encoder_encode_frame(MppEncoderCtx *enc,
                             const uint8_t *nv12_data,
                             size_t nv12_len,
                             uint64_t frame_id,
                             uint8_t **h264_data,
                             size_t *h264_len,
                             int *is_key_frame,
                             uint64_t *encode_start_ts_us,
                             uint64_t *encode_done_ts_us,
                             MppEncoderTiming *timing) {
    uint64_t total_start_us = get_now_us();
    uint64_t stage_start_us;
    uint64_t stage_end_us;

    if (timing) {
        memset(timing, 0, sizeof(*timing));
    }

    if (!enc || !enc->ctx || !nv12_data || !h264_data || !h264_len) {
        LOG_ERROR("mpp_encoder_encode_frame failed: invalid args enc=%p ctx=%p nv12=%p h264_data=%p h264_len=%p",
                  (void *)enc,
                  enc ? (void *)enc->ctx : NULL,
                  (const void *)nv12_data,
                  (void *)h264_data,
                  (void *)h264_len);
        return -1;
    }

    // 采集侧通常给紧凑 NV12（width*height*1.5），这里按有效图像大小做校验。
    size_t valid_nv12_size = (size_t)enc->width * enc->height * 3 / 2;
    if (nv12_len < valid_nv12_size) {
        LOG_ERROR("mpp_encoder_encode_frame failed: input NV12 len too small got=%zu need=%zu",
                  nv12_len,
                  valid_nv12_size);
        return -1;
    }

    void *frame_ptr = mpp_buffer_get_ptr(enc->frame_buffer);
    if (!frame_ptr) {
        LOG_ERROR("mpp_encoder_encode_frame failed: mpp_buffer_get_ptr");
        return -1;
    }

    /*
     * copy 输入路径的上游 nv12_data 是紧凑排布：
     * - Y 平面每行只有 width 字节；
     * - UV 平面紧跟在 width * height 后面；
     * - 行尾和高度方向都没有 MPP 对齐后产生的 padding。
     *
     * MPP 编码器初始化时给 MppFrame 配置的是 enc->hor_stride / enc->ver_stride，
     * 输入 buffer 也按这个 stride 大小申请。若直接把紧凑 NV12 当成带 stride 的
     * MPP 输入，UV 起始位置和每行步进都会与 MPP 的解释方式不一致。因此 copy 路径
     * 必须逐行把有效像素搬到带 stride 的 MPP 内部 buffer，同时把对齐区域清零。
     *
     * DMA-BUF 零拷贝路径不会经过这里。它不再生成一份重新排布后的 MPP 输入副本，
     * 而是让 MPP 直接读取外部 DMA-BUF；外部 buffer 的实际排布必须已经与
     * MppFrame 上配置的格式和 stride 一致。
     */
    stage_start_us = get_now_us();
    copy_nv12_to_mpp_buffer(enc, (uint8_t *)frame_ptr, nv12_data);
    stage_end_us = get_now_us();
    if (timing) {
        timing->encoder_input_buffer_copy_us = stage_end_us - stage_start_us;
    }

    // 投喂一帧并拉取对应编码包（部分情况下可能暂时取不到 packet）。
    // 低延时思路：
    // 周期性强制 IDR，确保播放器不会长时间“等关键帧”，
    // 尤其是客户端中途接入或网络抖动后的恢复速度会明显更快。
    if (enc->gop > 0 && enc->pts > 0 && (enc->pts % enc->gop) == 0) {
        if (mpp_encoder_request_idr(enc) != 0) {
            LOG_WARN("mpp_encoder_encode_frame: periodic IDR request failed");
        }
    }


    if (encode_start_ts_us) {
        *encode_start_ts_us = get_now_us();;
    }
    // printf("[TRACE] frame=%" PRIu64 " step=before_encode_put_frame ts_us=%" PRIu64 "\n",
    //        frame_id, ts);


    /* 这里是设置该帧的PTS吗，为啥每次加一呢，PTS不是显示时间吗 */
    mpp_frame_set_pts(enc->frame, enc->pts++);
    stage_start_us = get_now_us();
    MPP_RET ret = enc->mpi->encode_put_frame(enc->ctx, enc->frame);
    stage_end_us = get_now_us();
    if (timing) {
        // 这里消耗13ms？
        timing->encoder_submit_frame_call_us = stage_end_us - stage_start_us;
    }
    if (ret != MPP_OK) {
        mpp_log_error("encode_put_frame failed", ret);
        return -1;
    }

    MppPacket packet = NULL;
    stage_start_us = get_now_us();
    ret = enc->mpi->encode_get_packet(enc->ctx, &packet);
    stage_end_us = get_now_us();
    if (timing) {
        timing->encoder_poll_packet_call_us = stage_end_us - stage_start_us;
    }
    {
        uint64_t ts = get_now_us();
        if (encode_done_ts_us) {
            *encode_done_ts_us = ts;
        }
        // printf("[TRACE] frame=%" PRIu64 " step=after_encode_get_packet ts_us=%" PRIu64 " ret=%d has_packet=%d\n",
        //        frame_id, ts, ret, packet ? 1 : 0);
    }
    if (ret != MPP_OK) {
        mpp_log_error("encode_get_packet failed", ret);
        return -1;
    }

    if (!packet) {
        // 编码器还未产出数据（例如缓存阶段），不是硬错误。
        *h264_data = NULL;
        *h264_len = 0;
        if (is_key_frame) {
            *is_key_frame = 0;
        }
        if (timing) {
            timing->encode_frame_total_us = get_now_us() - total_start_us;
        }
        return 0;
    }

    size_t packet_len = (size_t)mpp_packet_get_length(packet);
    void *packet_pos = mpp_packet_get_pos(packet);
    if (!packet_pos || packet_len == 0) {
        mpp_packet_deinit(&packet);
        *h264_data = NULL;
        *h264_len = 0;
        if (is_key_frame) {
            *is_key_frame = 0;
        }
        return 0;
    }

    if (ensure_packet_cache(enc, packet_len) != 0) {
        LOG_ERROR("mpp_encoder_encode_frame failed: ensure packet cache len=%zu", packet_len);
        mpp_packet_deinit(&packet);
        return -1;
    }

    // 把 MPP packet 拷贝到可复用缓存，返回给上层写文件/推流。
    stage_start_us = get_now_us();
    memcpy(enc->packet_cache, packet_pos, packet_len);
    stage_end_us = get_now_us();
    if (timing) {
        timing->encoder_packet_copy_us = stage_end_us - stage_start_us;
    }
    *h264_data = enc->packet_cache;
    *h264_len = packet_len;

    if (is_key_frame) {
        RK_S32 intra = 0;
        MppMeta meta = mpp_packet_get_meta(packet);
        if (meta && mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &intra) == MPP_OK) {
            *is_key_frame = (intra != 0) ? 1 : 0;
        } else {
            *is_key_frame = 0;
        }
    }

    mpp_packet_deinit(&packet);
    if (timing) {
        timing->encode_frame_total_us = get_now_us() - total_start_us; // 包括输入拷贝、编码处理、输出拷贝的整帧耗时
    }
    return 0;
}

/**
 * @description: 使用外部 DMA-BUF 作为 MPP NV12 输入，编码一帧 Annex-B H264 数据。
 *
 * 逻辑说明：
 * 1. 校验外部 DMA-BUF 至少能覆盖当前 MPP frame 按 stride 计算出的输入尺寸。
 * 2. 通过 MPP_BUFFER_TYPE_EXT_DMA 把上游导出的 fd 包装成 MppBuffer。
 * 3. 临时把该外部 MppBuffer 绑定到复用的 enc->frame，复用编码宽高、格式和 stride 元数据。
 * 4. 提交 frame 并立即轮询本帧输出 packet，输出包仍拷贝到 encoder packet_cache，
 *    让上层拿到的码流指针在 MPP packet 释放后继续有效。
 * 5. 编码完成后恢复 enc->frame 的内部 copy-path buffer，释放本次导入的 MppBuffer。
 *
 * stride 前提：
 * - copy 路径会把紧凑 NV12 重排到 enc->hor_stride / enc->ver_stride 对应的 MPP 输入 buffer；
 * - DMA-BUF 路径为保持零拷贝，不能在本函数内再做这次重排；
 * - MPP 会按照 enc->frame 当前配置的 MPP_FMT_YUV420SP、hor_stride、ver_stride
 *   去解释外部 DMA-BUF，所以导入的 V4L2 DMA-BUF 内存布局必须满足这些元数据；
 * - 若采集驱动给出的 bytesperline/plane layout 与 MPP stride 不一致，正确做法是
 *   在采集格式协商、RGA 等硬件转换环节统一布局，或回退 copy 路径，不能仅靠 fd 导入。
 *
 * @param {MppEncoderCtx *} enc MPP 编码器上下文。
 * @param {int} dmabuf_fd 上游导出的 DMA-BUF fd。
 * @param {size_t} dmabuf_size 外部 DMA-BUF 的完整容量。
 * @param {uint64_t} frame_id 当前采集帧号，仅用于错误日志和链路定位。
 * @param {uint8_t **} h264_data 输出 Annex-B H264 缓存地址，由 encoder 内部持有。
 * @param {size_t *} h264_len 输出 H264 数据长度。
 * @param {int *} is_key_frame 输出是否关键帧；可传 NULL。
 * @param {uint64_t *} encode_start_ts_us 输出提交编码前的单调时钟时间；可传 NULL。
 * @param {uint64_t *} encode_done_ts_us 输出取包完成后的单调时钟时间；可传 NULL。
 * @param {MppEncoderTiming *} timing 输出当前帧编码阶段耗时；可传 NULL。
 * @return {int} 0 成功，-1 失败。
 */
int mpp_encoder_encode_dmabuf(MppEncoderCtx *enc,
                              int dmabuf_fd,
                              size_t dmabuf_size,
                              uint64_t frame_id,
                              uint8_t **h264_data,
                              size_t *h264_len,
                              int *is_key_frame,
                              uint64_t *encode_start_ts_us,
                              uint64_t *encode_done_ts_us,
                              MppEncoderTiming *timing) {
    uint64_t total_start_us = get_now_us();
    uint64_t stage_start_us;
    uint64_t stage_end_us;
    size_t frame_size;
    MppBufferInfo buffer_info;
    MppBuffer imported_buffer = NULL;
    MppPacket packet = NULL;
    MPP_RET ret;

    if (timing) memset(timing, 0, sizeof(*timing));
    if (!enc || !enc->ctx || dmabuf_fd < 0 || !h264_data || !h264_len) {
        LOG_ERROR("mpp_encoder_encode_dmabuf failed: invalid args enc=%p ctx=%p fd=%d",
                  (void *)enc,
                  enc ? (void *)enc->ctx : NULL,
                  dmabuf_fd);
        return -1;
    }
    /*
     * 这里按 MPP frame 的 stride 计算可读输入范围，而不是按 width * height。
     * 零拷贝下 MPP 将直接按 stride 从外部 fd 读数据；容量不足时继续导入会导致
     * MPP 访问超出 buffer 的有效范围。
     */
    frame_size = (size_t)enc->hor_stride * enc->ver_stride * 3 / 2;
    if (dmabuf_size < frame_size) {
        LOG_ERROR("mpp_encoder_encode_dmabuf failed: buffer too small got=%zu need=%zu", dmabuf_size, frame_size);
        return -1;
    }

    /*
     * 把 V4L2 等上游导出的 DMA-BUF fd 包装成 MppBuffer。
     * 这里没有 mpp_buffer_get() 申请新的输入 buffer，也没有 memcpy 原始 NV12；
     * imported_buffer 只是让 MPP 认识并引用外部物理 buffer。
     */
    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.type = MPP_BUFFER_TYPE_EXT_DMA;
    buffer_info.fd = dmabuf_fd;
    buffer_info.size = dmabuf_size;
    ret = mpp_buffer_import(&imported_buffer, &buffer_info);
    if (ret != MPP_OK || !imported_buffer) {
        mpp_log_error("mpp_buffer_import external dmabuf failed", ret);
        return -1;
    }
    /*
     * enc->frame 在 init 时已设置 width/height/format/stride。这里仅把本帧
     * 使用的 buffer 从内部 copy-path frame_buffer 暂时切换成外部 DMA-BUF。
     */
    mpp_frame_set_buffer(enc->frame, imported_buffer);

    /* 保持与 copy 编码路径一致的周期性 IDR 策略。 */
    if (enc->gop > 0 && enc->pts > 0 && (enc->pts % enc->gop) == 0) {
        if (mpp_encoder_request_idr(enc) != 0)
            LOG_WARN("mpp_encoder_encode_dmabuf: periodic IDR request failed");
    }
    if (encode_start_ts_us) *encode_start_ts_us = get_now_us();
    mpp_frame_set_pts(enc->frame, enc->pts++);
    /* 提交外部 DMA-BUF frame；该阶段不再统计 encoder_input_buffer_copy_us。 */
    stage_start_us = get_now_us();
    ret = enc->mpi->encode_put_frame(enc->ctx, enc->frame);
    stage_end_us = get_now_us();
    if (timing) timing->encoder_submit_frame_call_us = stage_end_us - stage_start_us;
    if (ret != MPP_OK) {
        mpp_log_error("encode_put_frame dmabuf failed", ret);
        goto fail;
    }

    /* 拉取编码包，和 copy 路径一样允许编码器暂时还未产出 packet。 */
    stage_start_us = get_now_us();
    ret = enc->mpi->encode_get_packet(enc->ctx, &packet);
    stage_end_us = get_now_us();
    if (timing) timing->encoder_poll_packet_call_us = stage_end_us - stage_start_us;
    if (encode_done_ts_us) *encode_done_ts_us = get_now_us();
    if (ret != MPP_OK) {
        mpp_log_error("encode_get_packet dmabuf failed", ret);
        goto fail;
    }
    if (!packet) {
        *h264_data = NULL;
        *h264_len = 0;
        if (is_key_frame) *is_key_frame = 0;
        if (timing) timing->encode_frame_total_us = get_now_us() - total_start_us;
        mpp_frame_set_buffer(enc->frame, enc->frame_buffer);
        mpp_buffer_put(imported_buffer);
        return 0;
    }

    {
        size_t packet_len = (size_t)mpp_packet_get_length(packet);
        void *packet_pos = mpp_packet_get_pos(packet);
        if (!packet_pos || packet_len == 0) {
            *h264_data = NULL;
            *h264_len = 0;
            if (is_key_frame) *is_key_frame = 0;
            goto done;
        }
        /*
         * packet 由 MPP 管理，函数返回前会 deinit。把编码后的码流拷到
         * enc->packet_cache，保证 h264_data 返回给上层后仍有效。
         * 这不是原始 NV12 输入帧拷贝，不影响 V4L2 -> MPP 的零拷贝目标。
         */
        if (ensure_packet_cache(enc, packet_len) != 0)
            goto fail;
        stage_start_us = get_now_us();
        memcpy(enc->packet_cache, packet_pos, packet_len);
        stage_end_us = get_now_us();
        if (timing) timing->encoder_packet_copy_us = stage_end_us - stage_start_us;
        *h264_data = enc->packet_cache;
        *h264_len = packet_len;
    }
    if (is_key_frame) {
        RK_S32 intra = 0;
        MppMeta meta = mpp_packet_get_meta(packet);
        *is_key_frame = (meta && mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &intra) == MPP_OK && intra != 0) ? 1 : 0;
    }

done:
    if (packet) mpp_packet_deinit(&packet);
    if (timing) timing->encode_frame_total_us = get_now_us() - total_start_us;
    /* 恢复 copy 路径内部 buffer，避免下一次 copy 编码继续引用本次外部 fd。 */
    mpp_frame_set_buffer(enc->frame, enc->frame_buffer);
    mpp_buffer_put(imported_buffer);
    return 0;

fail:
    if (packet) mpp_packet_deinit(&packet);
    mpp_frame_set_buffer(enc->frame, enc->frame_buffer);
    mpp_buffer_put(imported_buffer);
    LOG_ERROR("mpp_encoder_encode_dmabuf failed frame=%" PRIu64, frame_id);
    return -1;
}

int mpp_encoder_request_idr(MppEncoderCtx *enc) {
    MPP_RET ret;
    if (!enc || !enc->ctx || !enc->mpi) {
        LOG_ERROR("mpp_encoder_request_idr failed: invalid args enc=%p ctx=%p mpi=%p",
                  (void *)enc,
                  enc ? (void *)enc->ctx : NULL,
                  enc ? (void *)enc->mpi : NULL);
        return -1;
    }
    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_IDR_FRAME, NULL);
    if (ret != MPP_OK) {
        mpp_log_error("MPP_ENC_SET_IDR_FRAME failed", ret);
        return -1;
    }
    return 0;
}

/**
 * @description: 释放 MPP 编码器资源
 * @param {MppEncoderCtx *} enc
 * @return {void}
 */
void mpp_encoder_deinit(MppEncoderCtx *enc) {
    if (!enc) {
        return;
    }

    // 释放顺序按依赖关系逆序进行，避免悬挂引用。
    if (enc->frame) {
        mpp_frame_deinit(&enc->frame);
    }

    if (enc->frame_buffer) {
        mpp_buffer_put(enc->frame_buffer);
        enc->frame_buffer = NULL;
    }

    if (enc->frame_group) {
        mpp_buffer_group_put(enc->frame_group);
        enc->frame_group = NULL;
    }

    if (enc->cfg) {
        mpp_enc_cfg_deinit(enc->cfg);
        enc->cfg = NULL;
    }

    if (enc->ctx) {
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
    }

    if (enc->packet_cache) {
        free(enc->packet_cache);
        enc->packet_cache = NULL;
    }
    enc->packet_cache_size = 0;
}
