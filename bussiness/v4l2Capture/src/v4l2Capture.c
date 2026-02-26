#include <linux/videodev2.h>

#include "v4l2Capture.h"

// ==================== 工具函数：打印V4L2错误信息 ====================
void print_v4l2_error(const char *msg, int ret) {
    fprintf(stderr, "[ERROR] %s: %s (errno=%d)\n", msg, strerror(-ret), ret);
}

// ==================== 初始化V4L2采集 ====================
int v4l2_capture_init(V4L2CaptureCtx *ctx) {
    if (!ctx) {
        fprintf(stderr, "[ERROR] ctx is NULL\n");
        return -1;
    }
    memset(ctx, 0, sizeof(V4L2CaptureCtx));

    // 1. 打开摄像头设备
    ctx->fd = open(CAM_DEV_PATH, O_RDWR, 0);
    if (ctx->fd < 0) {
        perror("[ERROR] open camera dev failed");
        return -1;
    }
    printf("[INFO] open camera %s success\n", CAM_DEV_PATH);

    // 2. 检查设备是否支持视频采集
    struct v4l2_capability cap;
    int ret = ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_QUERYCAP failed", ret);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        fprintf(stderr, "[ERROR] device not support video capture\n");
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "[ERROR] device not support streaming capture\n");
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    printf("[INFO] camera support video capture and streaming\n");

    // 3. 设置采集格式（NV12，1920x1080）
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = CAPTURE_WIDTH;
    fmt.fmt.pix_mp.height = CAPTURE_HEIGHT;
    fmt.fmt.pix_mp.pixelformat = CAPTURE_FORMAT;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE; // 逐行扫描

    ret = ioctl(ctx->fd, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_S_FMT failed", ret);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    // 验证实际设置的格式（摄像头可能自动调整分辨率）
    printf("[INFO] capture format set: %dx%d, format=NV12 (0x%x)\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.pixelformat);

    // 4. 请求缓冲区（用于mmap映射）
    struct v4l2_requestbuffers req = {0};
    req.count = 4;                // 申请4个缓冲区（经验值）
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP; // 内存映射方式（避免数据拷贝）

    ret = ioctl(ctx->fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_REQBUFS failed", ret);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    ctx->buf_count = req.count;
    printf("[INFO] request %d buffers success\n", ctx->buf_count);

    // 5. 映射缓冲区到用户空间
    for (int i = 0; i < ctx->buf_count; i++) 
    {
        // 多平面模式必须用v4l2_buffer + v4l2_plane
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES]; // RKISP通常只有1个平面（NV12）
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = VIDEO_MAX_PLANES; // 平面数量（RKISP NV12为1）
        buf.m.planes = planes; // 指向平面数组

        // 多平面模式的VIDIOC_QUERYBUF
        if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[ERROR] query buffer %d failed: %s (errno=%d)\n", i, strerror(errno), errno);
            // 清理已映射的缓冲区
            for (int j = 0; j < i; j++) {
                if (ctx->buf[j]) munmap(ctx->buf[j], ctx->buf_len[j]);
            }
            close(ctx->fd);
            ctx->fd = -1;
            return -1;
        }

        // 映射缓冲区（用平面的offset和length）
        ctx->buf[i] = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, planes[0].m.mem_offset);
        if (ctx->buf[i] == MAP_FAILED) {
            fprintf(stderr, "[ERROR] mmap buffer %d failed: %s (errno=%d)\n", i, strerror(errno), errno);
            for (int j = 0; j < i; j++) {
                if (ctx->buf[j]) munmap(ctx->buf[j], ctx->buf_len[j]);
            }
            close(ctx->fd);
            ctx->fd = -1;
            return -1;
        }
        ctx->buf_len[i] = planes[0].length;
        printf("[INFO] buffer %d mapped: addr=%p, len=%d\n", i, ctx->buf[i], ctx->buf_len[i]);

        // 入队缓冲区（多平面模式）
        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[ERROR] qbuf buffer %d failed: %s (errno=%d)\n", i, strerror(errno), errno);
            for (int j = 0; j <= i; j++) {
                if (ctx->buf[j]) munmap(ctx->buf[j], ctx->buf_len[j]);
            }
            close(ctx->fd);
            ctx->fd = -1;
            return -1;
        }
    }

    // 6. 启动流采集
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ret = ioctl(ctx->fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_STREAMON failed", ret);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    printf("[INFO] start streaming capture success\n");

    return 0;
}

// ==================== 采集一帧数据 ====================
int v4l2_capture_frame(V4L2CaptureCtx *ctx, uint8_t **frame_data, int *frame_len) {
    if (ctx->fd < 0 || !frame_data || !frame_len) {
        return -1;
    }

    // 多平面模式出队缓冲区
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = VIDEO_MAX_PLANES;
    buf.m.planes = planes;

    if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        fprintf(stderr, "[ERROR] dqbuf failed: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }

    // 返回帧数据（多平面模式取第一个平面）
    *frame_data = (uint8_t *)ctx->buf[buf.index];
    *frame_len = planes[0].bytesused;
    printf("[INFO] capture frame %d: len=%d\n", buf.index, *frame_len);

    // 重新入队缓冲区
    if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "[ERROR] re-qbuf failed: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }

    return 0;
}

// ==================== 释放V4L2资源 ====================
void v4l2_capture_deinit(V4L2CaptureCtx *ctx) {
    if (!ctx) return;

    // 停止流采集
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);

    // 释放mmap映射
    for (int i = 0; i < ctx->buf_count; i++) {
        if (ctx->buf[i]) {
            munmap(ctx->buf[i], ctx->buf_len[i]);
        }
    }

    // 关闭设备
    close(ctx->fd);
    memset(ctx, 0, sizeof(V4L2CaptureCtx));
    printf("[INFO] v4l2 capture deinit success\n");
}
