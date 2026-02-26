#include <stdio.h>
#include <stdlib.h>

#include "mppEncoder.h"
#include "v4l2Capture.h"

#define ENCODE_OUTPUT_FILE "capture.h264"
#define ENCODE_FRAME_COUNT 300
#define ENCODE_FPS 30
#define ENCODE_BITRATE (2 * 1024 * 1024)
#define ENCODE_GOP 60

/**
 * @brief MPP编码测试程序，使用命令 ffplay -f h264 capture.h264 验证
 * 
 * @return int 
 */

int main() {
    V4L2CaptureCtx cap_ctx;
    MppEncoderCtx enc_ctx;
    FILE *fp = NULL;

    uint8_t *raw_frame = NULL;
    int raw_frame_len = 0;
    int frame_count = 0;

    // 1) 初始化采集端（V4L2）与编码端（MPP H264）。
    if (v4l2_capture_init(&cap_ctx) < 0) {
        fprintf(stderr, "[ERROR] v4l2_capture_init failed\n");
        return -1;
    }

    if (mpp_encoder_init(&enc_ctx, CAPTURE_WIDTH, CAPTURE_HEIGHT, ENCODE_FPS, ENCODE_BITRATE, ENCODE_GOP) < 0) {
        fprintf(stderr, "[ERROR] mpp_encoder_init failed\n");
        v4l2_capture_deinit(&cap_ctx);
        return -1;
    }

    // 2) 输出裸 H264 文件（Annex-B），用于 ffplay/ffmpeg 验证。
    fp = fopen(ENCODE_OUTPUT_FILE, "wb");
    if (!fp) {
        perror("[ERROR] open h264 output file failed");
        mpp_encoder_deinit(&enc_ctx);
        v4l2_capture_deinit(&cap_ctx);
        return -1;
    }

    // 3) 主循环：采集 NV12 帧 -> MPP 编码 -> 写入 H264 码流。
    printf("[INFO] start capture + encode %d frames\n", ENCODE_FRAME_COUNT);
    while (frame_count < ENCODE_FRAME_COUNT) {
        if (v4l2_capture_frame(&cap_ctx, &raw_frame, &raw_frame_len) < 0) {
            fprintf(stderr, "[ERROR] v4l2_capture_frame failed\n");
            break;
        }

        uint8_t *h264_data = NULL;
        size_t h264_len = 0;
        int is_key = 0;

        if (mpp_encoder_encode_frame(&enc_ctx,
                                     raw_frame,
                                     (size_t)raw_frame_len,
                                     &h264_data,
                                     &h264_len,
                                     &is_key) < 0) {
            fprintf(stderr, "[ERROR] mpp_encoder_encode_frame failed\n");
            break;
        }

        if (h264_data && h264_len > 0) {
            fwrite(h264_data, 1, h264_len, fp);
            if (frame_count % ENCODE_FPS == 0) {
                printf("[INFO] frame=%d encoded=%zu bytes key=%d\n", frame_count, h264_len, is_key);
            }
        }

        frame_count++;
    }

    // 4) 释放资源。
    if (fp) {
        fclose(fp);
        printf("[INFO] encoded %d frames, output=%s\n", frame_count, ENCODE_OUTPUT_FILE);
    }

    mpp_encoder_deinit(&enc_ctx);
    v4l2_capture_deinit(&cap_ctx);
    return 0;
}
