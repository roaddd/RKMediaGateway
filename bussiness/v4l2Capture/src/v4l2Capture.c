#include "v4l2Capture.h"

#include <inttypes.h>
#include <time.h>

/**
 * @description: 输出 V4L2 接口错误日志
 * @param {const char *} msg
 * @param {int} ret
 * @return {static void}
 */
static void print_v4l2_error(const char *msg, int ret) {
    fprintf(stderr, "[ERROR] %s: %s (errno=%d)\n", msg, strerror(-ret), ret);
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
 * @description: 获取当前实时时钟时间，单位微秒
 * @return {static uint64_t}
 */
static uint64_t get_realtime_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 获取字符对应的 5x7 点阵数据
 * @param {char} c
 * @param {uint8_t} rows
 * @return {static int}
 */
static int glyph5x7(char c, uint8_t rows[7]) {
    switch (c) {
        case '0': { uint8_t r[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; memcpy(rows, r, 7); return 1; }
        case '1': { uint8_t r[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows, r, 7); return 1; }
        case '2': { uint8_t r[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; memcpy(rows, r, 7); return 1; }
        case '3': { uint8_t r[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; memcpy(rows, r, 7); return 1; }
        case '4': { uint8_t r[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; memcpy(rows, r, 7); return 1; }
        case '5': { uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; memcpy(rows, r, 7); return 1; }
        case '6': { uint8_t r[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; memcpy(rows, r, 7); return 1; }
        case '7': { uint8_t r[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; memcpy(rows, r, 7); return 1; }
        case '8': { uint8_t r[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; memcpy(rows, r, 7); return 1; }
        case '9': { uint8_t r[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; memcpy(rows, r, 7); return 1; }
        case 't': { uint8_t r[7] = {0x04,0x04,0x1F,0x04,0x04,0x04,0x03}; memcpy(rows, r, 7); return 1; }
        case 'f': { uint8_t r[7] = {0x06,0x08,0x08,0x1E,0x08,0x08,0x08}; memcpy(rows, r, 7); return 1; }
        case '=': { uint8_t r[7] = {0x00,0x1F,0x00,0x00,0x1F,0x00,0x00}; memcpy(rows, r, 7); return 1; }
        case ' ': { uint8_t r[7] = {0,0,0,0,0,0,0}; memcpy(rows, r, 7); return 1; }
        default: return 0;
    }
}

/**
 * @description: 在 NV12 图像亮度平面绘制文本
 * @param {uint8_t *} nv12
 * @param {int} width
 * @param {int} height
 * @param {int} x
 * @param {int} y
 * @param {const char *} text
 * @param {int} scale
 * @return {static void}
 */
static void draw_text_nv12_y(uint8_t *nv12, int width, int height, int x, int y, const char *text, int scale) {
    int cursor_x = x;
    uint8_t glyph[7];
    uint8_t *y_plane = nv12;
    int glyph_w = 5 * scale;
    int glyph_h = 7 * scale;
    int gap = scale;
    int total_h = glyph_h + 2 * scale;

    if (!nv12 || !text || width <= 0 || height <= 0 || scale <= 0) {
        return;
    }

    for (const char *p = text; *p; ++p) {
        if (!glyph5x7(*p, glyph)) {
            cursor_x += glyph_w + gap;
            continue;
        }

        // black background for readability
        for (int yy = y - scale; yy < y + total_h - scale; ++yy) {
            if (yy < 0 || yy >= height) continue;
            for (int xx = cursor_x - scale; xx < cursor_x + glyph_w + scale; ++xx) {
                if (xx < 0 || xx >= width) continue;
                y_plane[yy * width + xx] = 16;
            }
        }

        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[row] >> (4 - col)) & 0x01) {
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            int px = cursor_x + col * scale + sx;
                            int py = y + row * scale + sy;
                            if (px < 0 || py < 0 || px >= width || py >= height) {
                                continue;
                            }
                            y_plane[py * width + px] = 235;
                        }
                    }
                }
            }
        }
        cursor_x += glyph_w + gap;
    }
}

/**
 * @description: 初始化 V4L2 采集模块
 * @param {V4L2CaptureCtx *} ctx
 * @return {int}
 */
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
        struct v4l2_plane planes[V4L2_CAPTURE_MAX_PLANES];

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = V4L2_CAPTURE_MAX_PLANES;
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

/**
 * @description: 采集一帧图像数据
 * @param {V4L2CaptureCtx *} ctx
 * @param {uint8_t **} frame_data
 * @param {int *} frame_len
 * @param {uint64_t *} frame_id
 * @param {uint64_t *} dqbuf_ts_us
 * @param {uint64_t *} driver_to_dqbuf_us
 * @return {int}
 */
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
    struct v4l2_plane planes[V4L2_CAPTURE_MAX_PLANES];

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = V4L2_CAPTURE_MAX_PLANES;
    buf.m.planes = planes;

    if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        fprintf(stderr, "[ERROR] dqbuf failed: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }

    *dqbuf_ts_us = get_now_us();
    {
        uint64_t driver_ts_us = (uint64_t)buf.timestamp.tv_sec * 1000000ULL + (uint64_t)buf.timestamp.tv_usec;
        *driver_to_dqbuf_us = (*dqbuf_ts_us >= driver_ts_us) ? (*dqbuf_ts_us - driver_ts_us) : 0;
        // printf("[TRACE] step=driver_timestamp driver_ts_us=%" PRIu64
        //        " driver_to_dqbuf_us=%" PRIu64
        //        " ts_flags=0x%x\n",
        //        driver_ts_us,
        //        *driver_to_dqbuf_us,
        //        (unsigned)(buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK));
    }

    ctx->frame_id++;
    *frame_id = ctx->frame_id;
    // printf("[TRACE] frame=%" PRIu64 " step=after_vidioc_dqbuf ts_us=%" PRIu64 "\n",
    //        *frame_id, *dqbuf_ts_us);

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
    {
        char line1[64];
        char line2[64];
        uint64_t realtime_us = get_realtime_us();
        snprintf(line1, sizeof(line1), "rt=%" PRIu64, realtime_us/1000);
        snprintf(line2, sizeof(line2), "f=%" PRIu64, *frame_id);
        draw_text_nv12_y(ctx->frame_cache, CAPTURE_WIDTH, CAPTURE_HEIGHT, 24, 24, line1, 3);
        draw_text_nv12_y(ctx->frame_cache, CAPTURE_WIDTH, CAPTURE_HEIGHT, 24, 24 + 30, line2, 3);
    }
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

/**
 * @description: 释放 V4L2 采集模块资源
 * @param {V4L2CaptureCtx *} ctx
 * @return {void}
 */
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

