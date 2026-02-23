#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "v4l2Capture.h"

/**
 * @brief v4l2采集测试程序，使用命令 ffplay -f rawvideo -pixel_format nv12 -video_size 2112x1568 capture_nv12.yuv 验证
 * 
 * @return int 
 */

int main() {
    V4L2CaptureCtx ctx;
    FILE *fp = NULL;
    uint8_t *frame_data = NULL;
    int frame_len = 0;
    int frame_count = 0;

    // 1. 初始化V4L2采集
    if (v4l2_capture_init(&ctx) < 0) {
        fprintf(stderr, "[ERROR] v4l2 init failed\n");
        return -1;
    }

    // 2. 打开输出文件（保存原始YUV数据）
    fp = fopen(OUTPUT_FILE, "wb");
    if (!fp) {
        perror("[ERROR] open output file failed");
        v4l2_capture_deinit(&ctx);
        return -1;
    }

    // 3. 循环采集帧数据
    printf("[INFO] start capture %d frames...\n", SAVE_FRAME_COUNT);
    while (frame_count < SAVE_FRAME_COUNT) {
        int ret = v4l2_capture_frame(&ctx, &frame_data, &frame_len);
        if (ret == -1) {
            // 真正的错误，退出
            fprintf(stderr, "[ERROR] capture frame failed\n");
            break;
        } else if (ret == -2) {
            // 暂无数据，短暂休眠后重试
            usleep(1000);
            continue;
        }

        // 写入文件
        fwrite(frame_data, 1, frame_len, fp);
        fflush(fp);

        frame_count++;
        if (frame_count % 30 == 0) {
            printf("[INFO] captured %d frames (%.1f sec)\n", frame_count, frame_count / 30.0);
        }

        // 控制采集帧率（约30fps）
        usleep(1000000 / 30);
    }

    // 4. 释放资源
    if (fp) {
        fclose(fp);
        printf("[INFO] save %d frames to %s success\n", frame_count, OUTPUT_FILE);
    }
    v4l2_capture_deinit(&ctx);

    return 0;
}
