#include <errno.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "v4l2Capture.h"

typedef struct {
    void *addr;
    size_t len;
} MmapBuffer;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int xioctl(int fd, unsigned long req, void *arg, const char *name) {
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        fprintf(stderr, "[VIDEO0_FPS][ERROR] %s failed: errno=%d(%s)\n", name, errno, strerror(errno));
    }
    return ret;
}

int main(int argc, char **argv) {
    int frame_limit = SAVE_FRAME_COUNT;
    int fd = -1;
    MmapBuffer buffers[4];
    int buffer_count = 0;
    uint64_t start_us;
    uint64_t last_report_us;
    uint64_t last_driver_ts_us = 0;
    uint64_t total_driver_gap_us = 0;
    uint64_t max_driver_gap_us = 0;
    uint64_t frame_count = 0;
    uint64_t report_frames = 0;
    int ret = 1;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    memset(buffers, 0, sizeof(buffers));
    memset(&cap, 0, sizeof(cap));
    memset(&fmt, 0, sizeof(fmt));
    memset(&req, 0, sizeof(req));
    if (argc > 1) {
        frame_limit = atoi(argv[1]);
        if (frame_limit <= 0) frame_limit = SAVE_FRAME_COUNT;
    }

    fd = open(CAM_DEV_PATH, O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr, "[VIDEO0_FPS][ERROR] open %s failed: errno=%d(%s)\n", CAM_DEV_PATH, errno, strerror(errno));
        return 1;
    }

    if (xioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP") < 0) goto out;
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "[VIDEO0_FPS][ERROR] device does not support mplane streaming capture\n");
        goto out;
    }

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = CAPTURE_WIDTH;
    fmt.fmt.pix_mp.height = CAPTURE_HEIGHT;
    fmt.fmt.pix_mp.pixelformat = CAPTURE_FORMAT;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT") < 0) goto out;

    printf("[VIDEO0_FPS] device=%s size=%ux%u pixfmt=0x%x count=%d\n",
           CAM_DEV_PATH,
           fmt.fmt.pix_mp.width,
           fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.pixelformat,
           frame_limit);

    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS") < 0) goto out;
    if (req.count <= 0 || req.count > 4) {
        fprintf(stderr, "[VIDEO0_FPS][ERROR] invalid buffer count=%u\n", req.count);
        goto out;
    }
    buffer_count = (int)req.count;

    for (int i = 0; i < buffer_count; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[V4L2_CAPTURE_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = V4L2_CAPTURE_MAX_PLANES;
        buf.m.planes = planes;

        if (xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF") < 0) goto out;
        buffers[i].len = planes[0].length;
        buffers[i].addr = mmap(NULL, buffers[i].len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[0].m.mem_offset);
        if (buffers[i].addr == MAP_FAILED) {
            fprintf(stderr, "[VIDEO0_FPS][ERROR] mmap buffer=%d failed: errno=%d(%s)\n", i, errno, strerror(errno));
            buffers[i].addr = NULL;
            goto out;
        }
        if (xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF") < 0) goto out;
    }

    if (xioctl(fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON") < 0) goto out;

    start_us = now_us();
    last_report_us = start_us;
    while (frame_count < (uint64_t)frame_limit) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[V4L2_CAPTURE_MAX_PLANES];
        uint64_t driver_ts_us;
        uint64_t cur_us;

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = V4L2_CAPTURE_MAX_PLANES;
        buf.m.planes = planes;

        if (xioctl(fd, VIDIOC_DQBUF, &buf, "VIDIOC_DQBUF") < 0) goto stop_stream;
        frame_count++;
        report_frames++;

        driver_ts_us = (uint64_t)buf.timestamp.tv_sec * 1000000ULL + (uint64_t)buf.timestamp.tv_usec;
        if (last_driver_ts_us > 0 && driver_ts_us >= last_driver_ts_us) {
            uint64_t gap = driver_ts_us - last_driver_ts_us;
            total_driver_gap_us += gap;
            if (gap > max_driver_gap_us) max_driver_gap_us = gap;
        }
        last_driver_ts_us = driver_ts_us;

        if (xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF") < 0) goto stop_stream;

        cur_us = now_us();
        if (cur_us - last_report_us >= 1000000ULL) {
            double span = (double)(cur_us - last_report_us) / 1000000.0;
            double fps = (span > 0.0) ? (double)report_frames / span : 0.0;
            printf("[VIDEO0_FPS] window_fps=%.2f frames=%" PRIu64 "\n", fps, report_frames);
            report_frames = 0;
            last_report_us = cur_us;
        }
    }

    {
        uint64_t end_us = now_us();
        double total_sec = (double)(end_us - start_us) / 1000000.0;
        double avg_fps = (total_sec > 0.0) ? (double)frame_count / total_sec : 0.0;
        double avg_driver_gap_ms = (frame_count > 1) ? (double)total_driver_gap_us / (double)(frame_count - 1) / 1000.0 : 0.0;
        printf("[VIDEO0_FPS] done frames=%" PRIu64 " seconds=%.3f avg_fps=%.2f avg_driver_gap=%.2fms max_driver_gap=%.2fms\n",
               frame_count,
               total_sec,
               avg_fps,
               avg_driver_gap_ms,
               (double)max_driver_gap_us / 1000.0);
    }
    ret = 0;

stop_stream:
    xioctl(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
out:
    for (int i = 0; i < buffer_count; ++i) {
        if (buffers[i].addr) {
            munmap(buffers[i].addr, buffers[i].len);
        }
    }
    if (fd >= 0) close(fd);
    return ret;
}

/**
 * 
 * 经过测试，ov5640 1080p30 模式下，连续采集300帧的平均帧率约为29.75fps，平均驱动间隔约为33.28ms，最大驱动间隔约为33.35ms。
./video0_fps_test
[VIDEO0_FPS] device=/dev/video0 size=1920x1080 pixfmt=0x3231564e count=300
[ 9122.407211] rkisp rkisp-vir0: first params buf queue
[ 9122.410610] rockchip-csi2-dphy0: dphy0, data_rate_mbps 600
[ 9122.410658] rockchip-csi2-dphy csi2-dphy0: csi2_dphy_s_stream stream on:1, dphy0
[ 9122.482165] rkisp-vir0: tx stream:4 lose frame:0, isp state:0x201 frame:397
[VIDEO0_FPS] window_fps=27.13 frames=28
[VIDEO0_FPS] window_fps=30.01 frames=31
[VIDEO0_FPS] window_fps=30.03 frames=31
[VIDEO0_FPS] window_fps=30.07 frames=31
[VIDEO0_FPS] window_fps=30.07 frames=31
[VIDEO0_FPS] window_fps=30.05 frames=31
[VIDEO0_FPS] window_fps=30.02 frames=31
[VIDEO0_FPS] window_fps=30.05 frames=31
[VIDEO0_FPS] window_fps=30.07 frames=31
[VIDEO0_FPS] done frames=300 seconds=10.085 avg_fps=29.75 avg_driver_gap=33.28ms max_driver_gap=33.35ms
[ 9132.546820] rockchip-csi2-dphy csi2-dphy0: csi2_dphy_s_stream_stop stream stop, dphy0
[ 9132.546906] rockchip-csi2-dphy csi2-dphy0: csi2_dphy_s_stream stream on:0, dphy0

 */
