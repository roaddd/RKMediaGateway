#include "v4l2Capture.h"
#include <inttypes.h>
#include <time.h>

static void print_v4l2_error(const char *msg, int ret) {
    fprintf(stderr, "[ERROR] %s: %s (errno=%d)\n", msg, strerror(-ret), ret);
}

static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int v4l2_capture_init(V4L2CaptureCtx *ctx) {
    if (!ctx) {
        fprintf(stderr, "[ERROR] ctx is NULL\n");
        return -1;
    }
    memset(ctx, 0, sizeof(V4L2CaptureCtx));
    ctx->fd = -1;

    ctx->fd = open(CAM_DEV_PATH, O_RDWR, 0);
    if (ctx->fd < 0) {
        perror("[ERROR] open camera dev failed");
        return -1;
    }
    printf("[INFO] open camera %s success\n", CAM_DEV_PATH);

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

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = CAPTURE_WIDTH;
    fmt.fmt.pix_mp.height = CAPTURE_HEIGHT;
    fmt.fmt.pix_mp.pixelformat = CAPTURE_FORMAT;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

    ret = ioctl(ctx->fd, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_S_FMT failed", ret);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    printf("[INFO] capture format set: %dx%d, format=0x%x\n",
           fmt.fmt.pix_mp.width,
           fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.pixelformat);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    // 低延迟思路：
    // V4L2 缓冲越多，采集到应用层的帧可能越“旧”。
    // 这里把队列深度从 4 降到 2，减少采集侧排队时延（代价是抗抖动能力略降）。
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(ctx->fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_REQBUFS failed", ret);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    ctx->buf_count = req.count;
    printf("[INFO] request %d buffers success\n", ctx->buf_count);

    for (int i = 0; i < ctx->buf_count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = VIDEO_MAX_PLANES;
        buf.m.planes = planes;

        if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[ERROR] query buffer %d failed: %s (errno=%d)\n", i, strerror(errno), errno);
            v4l2_capture_deinit(ctx);
            return -1;
        }

        ctx->buf[i] = mmap(NULL,
                           planes[0].length,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           ctx->fd,
                           planes[0].m.mem_offset);
        if (ctx->buf[i] == MAP_FAILED) {
            fprintf(stderr, "[ERROR] mmap buffer %d failed: %s (errno=%d)\n", i, strerror(errno), errno);
            ctx->buf[i] = NULL;
            v4l2_capture_deinit(ctx);
            return -1;
        }
        ctx->buf_len[i] = (int)planes[0].length;
        printf("[INFO] buffer %d mapped: addr=%p, len=%d\n", i, ctx->buf[i], ctx->buf_len[i]);

        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[ERROR] qbuf buffer %d failed: %s (errno=%d)\n", i, strerror(errno), errno);
            v4l2_capture_deinit(ctx);
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ret = ioctl(ctx->fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_STREAMON failed", ret);
        v4l2_capture_deinit(ctx);
        return -1;
    }
    printf("[INFO] start streaming capture success\n");

    // 预分配一块用户态缓存。
    // 后续每次取帧都会先把 DQBUF 得到的数据拷贝到这里，再把原始缓冲 QBUF 回驱动。
    // 这样上层拿到的 frame_data 在本次函数返回后仍然有效，不会因为驱动复用底层缓冲而被覆盖。
    ctx->frame_cache_len = CAPTURE_WIDTH * CAPTURE_HEIGHT * 3 / 2;
    ctx->frame_cache = (uint8_t *)malloc((size_t)ctx->frame_cache_len);
    if (!ctx->frame_cache) {
        fprintf(stderr, "[ERROR] malloc frame cache failed\n");
        v4l2_capture_deinit(ctx);
        return -1;
    }

    return 0;
}

int v4l2_capture_frame(V4L2CaptureCtx *ctx,
                       uint8_t **frame_data,
                       int *frame_len,
                       uint64_t *frame_id,
                       uint64_t *dqbuf_ts_us,
                       uint64_t *driver_to_dqbuf_us) {
    if (!ctx || ctx->fd < 0 || !frame_data || !frame_len || !frame_id || !dqbuf_ts_us || !driver_to_dqbuf_us) {
        return -1;
    }

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

    *dqbuf_ts_us = get_now_us();
    {
        uint64_t driver_ts_us = (uint64_t)buf.timestamp.tv_sec * 1000000ULL + (uint64_t)buf.timestamp.tv_usec;
        *driver_to_dqbuf_us = (*dqbuf_ts_us >= driver_ts_us) ? (*dqbuf_ts_us - driver_ts_us) : 0;
        printf("[TRACE] step=driver_timestamp driver_ts_us=%" PRIu64
               " driver_to_dqbuf_us=%" PRIu64
               " ts_flags=0x%x\n",
               driver_ts_us,
               *driver_to_dqbuf_us,
               (unsigned)(buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK));
    }

    ctx->frame_id++;
    *frame_id = ctx->frame_id;
    printf("[TRACE] frame=%" PRIu64 " step=after_vidioc_dqbuf ts_us=%" PRIu64 "\n",
           *frame_id, *dqbuf_ts_us);

    // 某些驱动上 bytesused 可能大于初始预估值，这里按需扩容，避免越界。
    if ((int)planes[0].bytesused > ctx->frame_cache_len) {
        uint8_t *new_cache = (uint8_t *)realloc(ctx->frame_cache, planes[0].bytesused);
        if (!new_cache) {
            fprintf(stderr, "[ERROR] realloc frame cache failed\n");
            if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
                fprintf(stderr, "[ERROR] re-qbuf after realloc failed: %s (errno=%d)\n", strerror(errno), errno);
            }
            return -1;
        }
        ctx->frame_cache = new_cache;
        ctx->frame_cache_len = (int)planes[0].bytesused;
    }

    // 关键修复点：
    // 旧实现直接把 mmap 缓冲地址返回给上层，然后马上执行 QBUF。
    // 这样一旦驱动重新使用这块缓冲，调用方手里的指针就可能在编码前被新帧覆盖。
    // 现在先拷贝到 frame_cache，再 QBUF，保证上层在下一次取帧前看到的是稳定数据。
    memcpy(ctx->frame_cache, ctx->buf[buf.index], planes[0].bytesused);
    *frame_data = ctx->frame_cache;
    *frame_len = (int)planes[0].bytesused;
    // 低延迟思路：
    // 避免每帧打印日志，串口/控制台 IO 会显著拖慢实时链路。

    // 原始驱动缓冲在数据复制完成后即可立即回队，继续参与下一轮采集。
    if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "[ERROR] re-qbuf failed: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }

    return 0;
}

void v4l2_capture_deinit(V4L2CaptureCtx *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    }

    for (int i = 0; i < ctx->buf_count; i++) {
        if (ctx->buf[i]) {
            munmap(ctx->buf[i], (size_t)ctx->buf_len[i]);
            ctx->buf[i] = NULL;
        }
    }

    if (ctx->frame_cache) {
        free(ctx->frame_cache);
        ctx->frame_cache = NULL;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
    }

    memset(ctx, 0, sizeof(V4L2CaptureCtx));
    ctx->fd = -1;
    printf("[INFO] v4l2 capture deinit success\n");
}
