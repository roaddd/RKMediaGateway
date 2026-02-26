#include "mppEncoder.h"
#include "mpp_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPP_ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))

static void mpp_log_error(const char *msg, MPP_RET ret) {
    fprintf(stderr, "[ERROR] %s: ret=%d\n", msg, ret);
}

static int ensure_packet_cache(MppEncoderCtx *enc, size_t need_size) {
    // 输出码流长度会波动，按需扩容缓存，避免每帧都 malloc/free。
    if (enc->packet_cache_size >= need_size) {
        return 0;
    }

    uint8_t *new_buf = (uint8_t *)realloc(enc->packet_cache, need_size);
    if (!new_buf) {
        fprintf(stderr, "[ERROR] realloc packet cache failed\n");
        return -1;
    }

    enc->packet_cache = new_buf;
    enc->packet_cache_size = need_size;
    return 0;
}

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

int mpp_encoder_init(MppEncoderCtx *enc, int width, int height, int fps, int bitrate, int gop) {
    if (!enc || width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0 || gop <= 0) {
        fprintf(stderr, "[ERROR] invalid encoder init parameters\n");
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
    mpp_enc_cfg_set_s32(enc->cfg, "prep:format", MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(enc->cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:gop", enc->gop);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_num", enc->fps);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_num", enc->fps);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_target", enc->bitrate);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_max", enc->bitrate * 17 / 16);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_min", enc->bitrate * 15 / 16);

    mpp_enc_cfg_set_s32(enc->cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(enc->cfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(enc->cfg, "h264:level", 40);
    mpp_enc_cfg_set_s32(enc->cfg, "h264:cabac_en", 1);

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, enc->cfg);
    if (ret != MPP_OK) {
        mpp_log_error("MPP_ENC_SET_CFG failed", ret);
        mpp_encoder_deinit(enc);
        return -1;
    }

    // 每个 IDR 前输出 SPS/PPS，便于后续 RTSP/网络场景中间加入观看端。
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret != MPP_OK) {
        mpp_log_error("MPP_ENC_SET_HEADER_MODE failed", ret);
        mpp_encoder_deinit(enc);
        return -1;
    }

    // 3) 申请输入帧缓冲。优先 DRM，失败回退 ION（不同系统配置兼容）。
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

    printf("[INFO] mpp encoder init success: %dx%d fps=%d bitrate=%d gop=%d\n",
           enc->width, enc->height, enc->fps, enc->bitrate, enc->gop);
    return 0;
}

int mpp_encoder_encode_frame(MppEncoderCtx *enc,
                             const uint8_t *nv12_data,
                             size_t nv12_len,
                             uint8_t **h264_data,
                             size_t *h264_len,
                             int *is_key_frame) {
    if (!enc || !enc->ctx || !nv12_data || !h264_data || !h264_len) {
        return -1;
    }

    // 采集侧通常给紧凑 NV12（width*height*1.5），这里按有效图像大小做校验。
    size_t valid_nv12_size = (size_t)enc->width * enc->height * 3 / 2;
    if (nv12_len < valid_nv12_size) {
        fprintf(stderr, "[ERROR] input NV12 len too small: got=%zu need=%zu\n", nv12_len, valid_nv12_size);
        return -1;
    }

    void *frame_ptr = mpp_buffer_get_ptr(enc->frame_buffer);
    if (!frame_ptr) {
        fprintf(stderr, "[ERROR] mpp_buffer_get_ptr failed\n");
        return -1;
    }

    // 把紧凑 NV12 拷贝到带 stride 的 MPP 输入缓冲。
    copy_nv12_to_mpp_buffer(enc, (uint8_t *)frame_ptr, nv12_data);

    // 投喂一帧并拉取对应编码包（部分情况下可能暂时取不到 packet）。
    mpp_frame_set_pts(enc->frame, enc->pts++);
    MPP_RET ret = enc->mpi->encode_put_frame(enc->ctx, enc->frame);
    if (ret != MPP_OK) {
        mpp_log_error("encode_put_frame failed", ret);
        return -1;
    }

    MppPacket packet = NULL;
    ret = enc->mpi->encode_get_packet(enc->ctx, &packet);
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
        mpp_packet_deinit(&packet);
        return -1;
    }

    // 把 MPP packet 拷贝到可复用缓存，返回给上层写文件/推流。
    memcpy(enc->packet_cache, packet_pos, packet_len);
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
    return 0;
}

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
