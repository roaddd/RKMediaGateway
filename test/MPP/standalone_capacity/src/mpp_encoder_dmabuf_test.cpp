/**
 * @file mpp_encoder_dmabuf_test.cpp
 * @brief 使用真实V4L2采集buffer验证RK3568 DMA-BUF编码容量。
 *
 * 每路测试线程独占一个V4L2节点和MPP编码上下文。采集buffer通过
 * VIDIOC_EXPBUF导出的DMA-BUF fd直接交给MPP，编码完成后才释放引用并重新
 * QBUF，确保测试路径与网关正式DMA-BUF生命周期保持一致。
 */

#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>

#include "latencyStatistics.h"
#include "mppEncoder.h"
#include "systemMetricsMonitor.h"
#include "v4l2Capture.h"

namespace {

const int kDefaultDurationSeconds = 60;
const int kDefaultWarmupSeconds = 2;
const int kDefaultMetricsIntervalMs = 1000;
const char *kDefaultMetricsFile = "mpp_dmabuf_metrics.csv";
const double kMinimumFpsRatio = 0.98;
const int kCapturePollTimeoutMs = 1000;

/** 单路真实采集和编码配置。 */
struct StreamConfig {
    std::string device_path; /* V4L2采集节点。 */
    int width;               /* 采集和编码宽度。 */
    int height;              /* 采集和编码高度。 */
    int fps;                 /* 该路目标编码帧率。 */
    int bitrate;             /* H.264目标码率，单位bit/s。 */
    int gop;                 /* 相邻IDR帧间隔。 */
};

/** 测试程序完整运行参数。 */
struct TestConfig {
    int duration_seconds;              /* 正式统计持续时间，单位秒。 */
    int warmup_seconds;                /* Sensor切换后的预热时间，单位秒。 */
    int sensor_fps;                    /* Sensor及V4L2源目标帧率。 */
    int buffer_count;                  /* 每路申请的V4L2 mmap buffer数量。 */
    bool capture_only;                 /* 是否只采集并归还buffer，不执行MPP编码。 */
    std::vector<StreamConfig> streams; /* 同时运行的采集编码链路。 */
    bool metrics_enabled;              /* 是否采集系统状态。 */
    int metrics_interval_ms;           /* 系统状态采样周期。 */
    std::string metrics_file;          /* 系统状态CSV文件。 */
};

/** 单路预热阶段统计，不参与正式容量测试的PASS/FAIL判定。 */
struct WarmupResult {
    uint64_t captured_frames;  /* 预热期间成功取出并归还的V4L2帧数。 */
    uint64_t sequence_gaps;    /* 预热期间观察到的V4L2序号缺失数。 */
    uint64_t capture_timeouts; /* 预热期间等待V4L2帧超时的次数。 */
    uint64_t failed_frames;    /* 预热期间取帧失败的次数。 */
};

/** 单路DMA-BUF测试统计结果。 */
struct StreamResult {
    uint64_t captured_frames;          /* 从V4L2成功取出的帧数，包括门控跳过帧。 */
    uint64_t gated_frames;             /* 因目标编码帧率较低而主动跳过的帧数。 */
    uint64_t sequence_gaps;            /* V4L2 sequence不连续缺失的帧数。 */
    uint64_t capture_timeouts;         /* 等待V4L2可读超过限定时间的次数。 */
    uint64_t submitted_frames;         /* 提交给MPP的DMA-BUF帧数。 */
    uint64_t encoded_frames;           /* 成功取得非空编码包的帧数。 */
    uint64_t empty_packets;            /* 编码成功但没有输出包的次数。 */
    uint64_t failed_frames;            /* 采集或编码失败次数。 */
    uint64_t key_frames;               /* 输出IDR帧数量。 */
    uint64_t encoded_bytes;            /* H.264输出总字节数。 */
    uint64_t deadline_misses;          /* 编码总耗时超过目标编码周期的次数。 */
    uint64_t consecutive_deadline_misses; /* 当前连续编码超期帧数。 */
    uint64_t max_consecutive_deadline_misses; /* 最长连续编码超期帧数。 */
    uint64_t gap_event_count;          /* sequence发生不连续的事件次数。 */
    uint64_t max_single_gap;           /* 单次sequence跳变最多缺失帧数。 */
    uint64_t first_gap_elapsed_ms;     /* 首次gap相对正式统计起点的时间。 */
    uint64_t last_gap_elapsed_ms;      /* 最后一次gap相对正式统计起点的时间。 */
    double elapsed_seconds;            /* 本路实际运行时间。 */
    std::vector<uint64_t> capture_interval_us; /* 相邻V4L2驱动时间戳间隔。 */
    std::vector<uint64_t> camera_buffer_wait_us; /* 驱动时间戳到DQBUF返回的帧龄。 */
    std::vector<uint64_t> dqbuf_ioctl_us;      /* VIDIOC_DQBUF阻塞调用耗时。 */
    std::vector<uint64_t> dqbuf_to_encode_us;  /* DQBUF返回到开始编码的耗时。 */
    std::vector<uint64_t> dmabuf_import_us;    /* mpp_buffer_import耗时。 */
    std::vector<uint64_t> submit_us;           /* encode_put_frame调用耗时。 */
    std::vector<uint64_t> packet_wait_us;      /* encode_get_packet轮询耗时。 */
    std::vector<uint64_t> packet_copy_us;      /* 编码包复制耗时。 */
    std::vector<uint64_t> other_us;            /* 未归属到已知编码阶段的耗时。 */
    std::vector<uint64_t> total_us;            /* DMA-BUF编码接口总耗时。 */
};

/** 单路采集器、编码器及其生命周期状态。 */
struct StreamRuntime {
    StreamConfig config;       /* 当前码流配置。 */
    V4L2CaptureCtx capture;    /* 该路独占的V4L2采集上下文。 */
    MppEncoderCtx encoder;     /* 该路独占的MPP编码上下文。 */
    bool capture_ready;        /* V4L2是否初始化成功。 */
    bool encoder_ready;        /* MPP是否初始化成功。 */
    WarmupResult warmup;       /* Sensor和ISP稳定期间的采集统计。 */
    StreamResult result;       /* 线程写入的测试结果。 */
};

/** 协调所有采集线程从连续预热阶段原子切换到正式统计阶段。 */
struct TestPhaseControl {
    std::mutex mutex;                              /* 保护以下阶段状态。 */
    std::condition_variable condition;             /* 通知主线程所有worker已完成预热。 */
    size_t ready_workers;                          /* 已达到预热时长的worker数量。 */
    bool formal_started;                           /* 是否已进入正式统计阶段。 */
    bool aborted;                                  /* 任一worker预热失败时终止测试。 */
    std::chrono::steady_clock::time_point formal_start_time; /* 正式统计统一起点。 */
};

/**
 * @brief 通知主线程和其他采集线程终止本次阶段切换。
 * @param phase_control 测试阶段共享状态。
 */
void abortTestPhase(TestPhaseControl *phase_control) {
    if (phase_control == NULL) return;

    std::lock_guard<std::mutex> lock(phase_control->mutex);
    phase_control->aborted = true;
    phase_control->condition.notify_all();
}

/**
 * @brief 查询测试阶段是否已经因错误终止。
 * @param phase_control 测试阶段共享状态。
 * @return 已终止返回true，否则返回false。
 */
bool isTestPhaseAborted(TestPhaseControl *phase_control) {
    if (phase_control == NULL) return true;

    std::lock_guard<std::mutex> lock(phase_control->mutex);
    return phase_control->aborted;
}

/**
 * @brief 打印命令行帮助。
 */
void printUsage(const char *program_name) {
    std::printf(
        "Usage:\n"
        "  %s --preset main30|main60|sub30|sub60|current|target|stress [options]\n"
        "  %s --sensor-fps FPS --stream DEVICE,WIDTHxHEIGHT@FPS:BITRATE:GOP "
        "[--stream ...] [options]\n\n"
        "Options:\n"
        "  --duration SECONDS       Test duration, default 60\n"
        "  --warmup SECONDS         Warm-up duration after FPS switch, default 2\n"
        "  --buffer-count COUNT     V4L2 mmap buffers per stream, range 2..16\n"
        "  --capture-only          Capture and return every V4L2 frame without MPP\n"
        "  --sensor-fps FPS         V4L2 source FPS for custom streams\n"
        "  --metrics-interval MS    Metrics interval, default 1000\n"
        "  --metrics-file PATH      Metrics CSV, default mpp_dmabuf_metrics.csv\n"
        "  --no-metrics             Disable system metrics collection\n\n"
        "Presets:\n"
        "  main30 : /dev/video0 1920x1080@30\n"
        "  main60 : /dev/video0 1920x1080@60\n"
        "  sub30  : /dev/video1 1280x720@30\n"
        "  sub60  : /dev/video1 1280x720@60\n"
        "  current: main30 + sub30\n"
        "  target : main60 + sub30, V4L2 source runs at 60 FPS\n"
        "  stress : main60 + sub60\n",
        program_name,
        program_name);
}

/**
 * @brief 解析带设备节点的自定义码流参数。
 * @param text 格式为DEVICE,WIDTHxHEIGHT@FPS:BITRATE:GOP。
 * @param stream 输出码流配置。
 * @return true表示参数合法。
 */
bool parseStream(const char *text, StreamConfig *stream) {
    char device_path[128] = {0};
    char trailing = '\0';
    int matched = 0;

    if (text == NULL || stream == NULL) {
        std::fprintf(stderr, "[ERROR] parse DMA-BUF stream failed: null argument\n");
        return false;
    }
    matched = std::sscanf(text,
                          "%127[^,],%dx%d@%d:%d:%d%c",
                          device_path,
                          &stream->width,
                          &stream->height,
                          &stream->fps,
                          &stream->bitrate,
                          &stream->gop,
                          &trailing);
    if (matched != 6 || device_path[0] == '\0' || stream->width <= 0 ||
        stream->height <= 0 || stream->fps <= 0 || stream->bitrate <= 0 ||
        stream->gop <= 0 || (stream->width % 2) != 0 ||
        (stream->height % 2) != 0) {
        std::fprintf(stderr, "[ERROR] invalid DMA-BUF stream: %s\n", text);
        return false;
    }
    stream->device_path = device_path;
    return true;
}

/**
 * @brief 按名称装载单路或双路标准测试组合。
 */
bool applyPreset(const std::string &name, TestConfig *config) {
    StreamConfig main_stream = {"/dev/video0", 1920, 1080, 30, 6291456, 30};
    StreamConfig sub_stream = {"/dev/video1", 1280, 720, 30, 1048576, 30};

    if (config == NULL) {
        std::fprintf(stderr, "[ERROR] apply DMA-BUF preset failed: config is null\n");
        return false;
    }
    config->streams.clear();
    if (name == "main30") {
        config->sensor_fps = 30;
        config->streams.push_back(main_stream);
    } else if (name == "main60") {
        config->sensor_fps = 60;
        main_stream.fps = 60;
        main_stream.bitrate = 12582912;
        main_stream.gop = 60;
        config->streams.push_back(main_stream);
    } else if (name == "sub30") {
        config->sensor_fps = 30;
        config->streams.push_back(sub_stream);
    } else if (name == "sub60") {
        config->sensor_fps = 60;
        sub_stream.fps = 60;
        sub_stream.bitrate = 2097152;
        sub_stream.gop = 60;
        config->streams.push_back(sub_stream);
    } else if (name == "current") {
        config->sensor_fps = 30;
        config->streams.push_back(main_stream);
        config->streams.push_back(sub_stream);
    } else if (name == "target") {
        config->sensor_fps = 60;
        main_stream.fps = 60;
        main_stream.bitrate = 12582912;
        main_stream.gop = 60;
        config->streams.push_back(main_stream);
        config->streams.push_back(sub_stream);
    } else if (name == "stress") {
        config->sensor_fps = 60;
        main_stream.fps = 60;
        main_stream.bitrate = 12582912;
        main_stream.gop = 60;
        sub_stream.fps = 60;
        sub_stream.bitrate = 2097152;
        sub_stream.gop = 60;
        config->streams.push_back(main_stream);
        config->streams.push_back(sub_stream);
    } else {
        std::fprintf(stderr, "[ERROR] unknown DMA-BUF preset: %s\n", name.c_str());
        return false;
    }
    return true;
}

/**
 * @brief 解析并严格校验测试参数。
 */
bool parseArguments(int argc, char **argv, TestConfig *config) {
    StreamConfig stream = {};
    char *end = NULL;
    const char *option = NULL;
    long value = 0;
    int index = 0;

    if (argv == NULL || config == NULL) {
        std::fprintf(stderr, "[ERROR] parse DMA-BUF arguments failed: null argument\n");
        return false;
    }
    config->duration_seconds = kDefaultDurationSeconds;
    config->warmup_seconds = kDefaultWarmupSeconds;
    config->sensor_fps = 0;
    config->buffer_count = V4L2_CAPTURE_DEFAULT_BUFFER_COUNT;
    config->capture_only = false;
    config->streams.clear();
    config->metrics_enabled = true;
    config->metrics_interval_ms = kDefaultMetricsIntervalMs;
    config->metrics_file = kDefaultMetricsFile;

    for (index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--preset") == 0) {
            if (++index >= argc) {
                std::fprintf(stderr, "[ERROR] --preset requires a value\n");
                return false;
            }
            if (!applyPreset(argv[index], config)) return false;
        } else if (std::strcmp(argv[index], "--stream") == 0) {
            if (++index >= argc) {
                std::fprintf(stderr, "[ERROR] --stream requires a value\n");
                return false;
            }
            if (!parseStream(argv[index], &stream)) return false;
            config->streams.push_back(stream);
        } else if (std::strcmp(argv[index], "--duration") == 0 ||
                   std::strcmp(argv[index], "--warmup") == 0 ||
                   std::strcmp(argv[index], "--sensor-fps") == 0 ||
                   std::strcmp(argv[index], "--buffer-count") == 0 ||
                   std::strcmp(argv[index], "--metrics-interval") == 0) {
            option = argv[index];
            if (++index >= argc) {
                std::fprintf(stderr, "[ERROR] %s requires a value\n", option);
                return false;
            }
            end = NULL;
            value = std::strtol(argv[index], &end, 10);
            if (end == argv[index] || *end != '\0' || value <= 0) {
                std::fprintf(stderr, "[ERROR] invalid %s value: %s\n", option, argv[index]);
                return false;
            }
            if (std::strcmp(option, "--duration") == 0) {
                if (value > 86400) {
                    std::fprintf(stderr, "[ERROR] duration exceeds 86400 seconds\n");
                    return false;
                }
                config->duration_seconds = static_cast<int>(value);
            } else if (std::strcmp(option, "--warmup") == 0) {
                if (value > 60) {
                    std::fprintf(stderr, "[ERROR] warmup exceeds 60 seconds\n");
                    return false;
                }
                config->warmup_seconds = static_cast<int>(value);
            } else if (std::strcmp(option, "--sensor-fps") == 0) {
                if (value > 240) {
                    std::fprintf(stderr, "[ERROR] sensor fps exceeds 240: %ld\n", value);
                    return false;
                }
                config->sensor_fps = static_cast<int>(value);
            } else if (std::strcmp(option, "--buffer-count") == 0) {
                if (value < 2 || value > V4L2_CAPTURE_MAX_BUFFER_COUNT) {
                    std::fprintf(stderr,
                                 "[ERROR] buffer count must be 2..%d: %ld\n",
                                 V4L2_CAPTURE_MAX_BUFFER_COUNT, value);
                    return false;
                }
                config->buffer_count = static_cast<int>(value);
            } else {
                if (value < 100 || value > 60000) {
                    std::fprintf(stderr,
                                 "[ERROR] metrics interval must be 100..60000 ms: %ld\n",
                                 value);
                    return false;
                }
                config->metrics_interval_ms = static_cast<int>(value);
            }
        } else if (std::strcmp(argv[index], "--metrics-file") == 0) {
            if (++index >= argc || argv[index][0] == '\0') {
                std::fprintf(stderr, "[ERROR] --metrics-file requires a path\n");
                return false;
            }
            config->metrics_file = argv[index];
        } else if (std::strcmp(argv[index], "--no-metrics") == 0) {
            config->metrics_enabled = false;
        } else if (std::strcmp(argv[index], "--capture-only") == 0) {
            config->capture_only = true;
        } else if (std::strcmp(argv[index], "--help") == 0 ||
                   std::strcmp(argv[index], "-h") == 0) {
            printUsage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            std::fprintf(stderr, "[ERROR] unknown argument: %s\n", argv[index]);
            return false;
        }
    }
    if (config->streams.empty() || config->sensor_fps <= 0) {
        std::fprintf(stderr, "[ERROR] preset or streams with --sensor-fps are required\n");
        return false;
    }
    for (index = 0; index < static_cast<int>(config->streams.size()); ++index) {
        if (config->sensor_fps < config->streams[index].fps ||
            (config->sensor_fps % config->streams[index].fps) != 0) {
            std::fprintf(stderr,
                         "[ERROR] sensor fps=%d must be an integer multiple of stream[%d] fps=%d\n",
                         config->sensor_fps,
                         index,
                         config->streams[index].fps);
            return false;
        }
    }
    return true;
}

/**
 * @brief 初始化真实V4L2采集器和匹配其内存布局的MPP编码器。
 */
bool initializeStream(const StreamConfig &config,
                      int buffer_count,
                      bool capture_only,
                      StreamRuntime *runtime) {
    V4L2CaptureConfig capture_config = {};
    MppEncoderOptions encoder_options = {};
    const V4L2CaptureFormat *format = NULL;
    size_t required_size = 0;
    int ret = -1;
    int buffer_index = 0;

    if (runtime == NULL) {
        std::fprintf(stderr, "[ERROR] initialize DMA-BUF stream failed: runtime is null\n");
        return false;
    }
    std::memset(&runtime->capture, 0, sizeof(runtime->capture));
    std::memset(&runtime->encoder, 0, sizeof(runtime->encoder));
    runtime->capture_ready = false;
    runtime->encoder_ready = false;
    runtime->config = config;

    capture_config.device_path = runtime->config.device_path.c_str();
    capture_config.width = config.width;
    capture_config.height = config.height;
    capture_config.pixelformat = V4L2_PIX_FMT_NV12;
    capture_config.buffer_count = buffer_count;
    ret = v4l2_capture_init_with_config(&runtime->capture, &capture_config);
    if (ret != 0) {
        std::fprintf(stderr, "[ERROR] init V4L2 failed: device=%s ret=%d\n",
                     config.device_path.c_str(), ret);
        return false;
    }
    runtime->capture_ready = true;
    format = &runtime->capture.format;

    /* buffer数量是本轮对照实验的唯一变量之一，驱动不足额时禁止静默继续。 */
    if (runtime->capture.buf_count != buffer_count) {
        std::fprintf(stderr,
                     "[ERROR] V4L2 buffer count mismatch: device=%s request=%d actual=%d\n",
                     config.device_path.c_str(), buffer_count,
                     runtime->capture.buf_count);
        return false;
    }

    /* DMA-BUF测试禁止静默回退：协商布局不匹配时直接终止。 */
    if (format->width != config.width || format->height != config.height ||
        format->pixelformat != V4L2_PIX_FMT_NV12 || format->num_planes != 1 ||
        format->planes[0].bytesperline == 0 || format->planes[0].sizeimage == 0) {
        std::fprintf(stderr,
                     "[ERROR] unsupported DMA-BUF layout: device=%s size=%dx%d "
                     "format=0x%x planes=%u bytesperline=%u sizeimage=%u\n",
                     config.device_path.c_str(), format->width, format->height,
                     format->pixelformat, format->num_planes,
                     format->planes[0].bytesperline, format->planes[0].sizeimage);
        return false;
    }
    for (buffer_index = 0; buffer_index < runtime->capture.buf_count; ++buffer_index) {
        if (runtime->capture.buf_dmabuf_fd[buffer_index] < 0) {
            std::fprintf(stderr,
                         "[ERROR] V4L2 buffer has no exported DMA-BUF: device=%s index=%d\n",
                         config.device_path.c_str(), buffer_index);
            return false;
        }
    }

    encoder_options.rc_mode = MPP_ENCODER_RC_MODE_CBR;
    encoder_options.h264_profile = MPP_ENCODER_H264_PROFILE_HIGH;
    encoder_options.h264_level = config.fps > 30
        ? MPP_ENCODER_H264_LEVEL_42 : MPP_ENCODER_H264_LEVEL_40;
    encoder_options.h264_cabac_en = MPP_ENCODER_CABAC_ENABLED;
    encoder_options.input_hor_stride = static_cast<int>(format->planes[0].bytesperline);
    encoder_options.input_ver_stride = format->height;
    required_size = static_cast<size_t>(encoder_options.input_hor_stride) *
                    encoder_options.input_ver_stride * 3U / 2U;
    if (format->planes[0].sizeimage < required_size) {
        std::fprintf(stderr,
                     "[ERROR] V4L2 sizeimage is smaller than MPP layout: got=%u need=%zu\n",
                     format->planes[0].sizeimage, required_size);
        return false;
    }
    if (!capture_only) {
        ret = mpp_encoder_init(&runtime->encoder, config.width, config.height,
                               config.fps, config.bitrate, config.gop,
                               &encoder_options);
        if (ret != 0) {
            std::fprintf(stderr, "[ERROR] init MPP failed: device=%s ret=%d\n",
                         config.device_path.c_str(), ret);
            return false;
        }
        runtime->encoder_ready = true;
    }
    std::printf("[INFO] DMA-BUF stream ready: mode=%s device=%s size=%dx%d "
                "encode_fps=%d stride=%ux%d sizeimage=%u buffers=%d\n",
                capture_only ? "capture-only" : "encode",
                config.device_path.c_str(), config.width, config.height, config.fps,
                format->planes[0].bytesperline, format->height,
                format->planes[0].sizeimage, runtime->capture.buf_count);
    return true;
}

/**
 * @brief 释放一路测试持有的MPP和V4L2资源。
 */
void deinitializeStream(StreamRuntime *runtime) {
    if (runtime == NULL) return;
    if (runtime->encoder_ready) {
        mpp_encoder_deinit(&runtime->encoder);
        runtime->encoder_ready = false;
    }
    if (runtime->capture_ready) {
        v4l2_capture_deinit(&runtime->capture);
        runtime->capture_ready = false;
    }
}

/**
 * @brief 在正式统计前持续取出并归还V4L2帧，等待Sensor和ISP稳定。
 *
 * 预热线程不向MPP提交帧。多个采集节点并行预热，避免双路测试中先后预热
 * 导致较早完成的节点重新积压旧帧。预热期间的序号跳变单独记录，不参与
 * 正式容量测试的PASS/FAIL判定。
 */
bool drainWarmupUntilFormalStart(size_t stream_index,
                                 int warmup_seconds,
                                 TestPhaseControl *phase_control,
                                 StreamRuntime *runtime,
                                 std::chrono::steady_clock::time_point *formal_start_time) {
    std::chrono::steady_clock::time_point warmup_end_time;
    uint8_t *frame_data = NULL;
    V4L2CaptureBufferRef *buffer_ref = NULL;
    int frame_len = 0;
    uint64_t frame_id = 0;
    uint64_t dqbuf_ts_us = 0;
    uint64_t camera_wait_us = 0;
    uint64_t dqbuf_duration_us = 0;
    uint32_t sequence = 0;
    uint32_t previous_sequence = 0;
    bool have_previous_frame = false;
    bool ready_reported = false;
    bool formal_started = false;
    struct pollfd capture_poll_fd = {};
    int poll_ret = -1;
    int ret = -1;

    if (phase_control == NULL || runtime == NULL || !runtime->capture_ready ||
        formal_start_time == NULL || warmup_seconds <= 0) {
        std::fprintf(stderr,
                     "[ERROR] DMA-BUF warmup[%zu] has invalid runtime or duration\n",
                     stream_index);
        abortTestPhase(phase_control);
        return false;
    }

    runtime->warmup = WarmupResult();
    capture_poll_fd.fd = runtime->capture.fd;
    capture_poll_fd.events = POLLIN | POLLPRI;
    warmup_end_time = std::chrono::steady_clock::now() +
                      std::chrono::seconds(warmup_seconds);
    while (!formal_started) {
        capture_poll_fd.revents = 0;
        do {
            poll_ret = poll(&capture_poll_fd, 1, kCapturePollTimeoutMs);
        } while (poll_ret < 0 && errno == EINTR &&
                 !formal_started);
        if (poll_ret == 0) {
            ++runtime->warmup.capture_timeouts;
            std::fprintf(stderr,
                         "[ERROR] DMA-BUF warmup[%zu] capture poll timeout: "
                         "device=%s count=%llu\n",
                         stream_index, runtime->config.device_path.c_str(),
                         static_cast<unsigned long long>(runtime->warmup.capture_timeouts));
            abortTestPhase(phase_control);
            return false;
        }
        if (poll_ret < 0 ||
            (capture_poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ++runtime->warmup.failed_frames;
            std::fprintf(stderr,
                         "[ERROR] DMA-BUF warmup[%zu] capture poll failed: device=%s "
                         "ret=%d revents=0x%x errno=%d\n",
                         stream_index, runtime->config.device_path.c_str(), poll_ret,
                         capture_poll_fd.revents, errno);
            abortTestPhase(phase_control);
            return false;
        }

        buffer_ref = NULL;
        ret = v4l2_capture_acquire_frame(&runtime->capture, &frame_data, &frame_len,
                                         &frame_id, &dqbuf_ts_us, &camera_wait_us,
                                         &dqbuf_duration_us, &buffer_ref);
        if (ret != 0 || buffer_ref == NULL) {
            ++runtime->warmup.failed_frames;
            std::fprintf(stderr,
                         "[ERROR] DMA-BUF warmup[%zu] acquire frame failed ret=%d\n",
                         stream_index, ret);
            abortTestPhase(phase_control);
            return false;
        }

        sequence = v4l2_capture_buffer_sequence(buffer_ref);
        if (have_previous_frame && sequence > previous_sequence + 1U) {
            runtime->warmup.sequence_gaps += sequence - previous_sequence - 1U;
        }
        previous_sequence = sequence;
        have_previous_frame = true;
        ++runtime->warmup.captured_frames;

        /* 预热只用于排空切帧率前后的旧buffer，取出后立即归还驱动。 */
        v4l2_capture_buffer_unref(buffer_ref);
        buffer_ref = NULL;

        /*
         * 达到预热时长后只上报ready，仍持续DQBUF/QBUF。主线程可在此期间启动
         * metrics；直到主线程发布正式起点前，采集队列始终有人消费。
         */
        if (!ready_reported && std::chrono::steady_clock::now() >= warmup_end_time) {
            std::lock_guard<std::mutex> lock(phase_control->mutex);
            ++phase_control->ready_workers;
            ready_reported = true;
            phase_control->condition.notify_all();
        }
        if (ready_reported) {
            std::lock_guard<std::mutex> lock(phase_control->mutex);
            if (phase_control->aborted) return false;
            formal_started = phase_control->formal_started;
            if (formal_started) {
                *formal_start_time = phase_control->formal_start_time;
            }
        }
    }
    return true;
}

/**
 * @brief 执行一路V4L2采集、帧率门控和DMA-BUF编码。
 */
void runStreamWorker(size_t stream_index,
                     int sensor_fps,
                     int duration_seconds,
                     int warmup_seconds,
                     bool capture_only,
                     TestPhaseControl *phase_control,
                     StreamRuntime *runtime) {
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    std::chrono::steady_clock::time_point completed_time;
    StreamResult *result = NULL;
    uint8_t *frame_data = NULL;
    uint8_t *encoded_data = NULL;
    V4L2CaptureBufferRef *buffer_ref = NULL;
    int frame_len = 0;
    int dmabuf_fd = -1;
    size_t dmabuf_size = 0;
    size_t encoded_size = 0;
    int key_frame = 0;
    int ret = -1;
    int gate_divisor = 1;
    uint64_t frame_id = 0;
    uint64_t dqbuf_ts_us = 0;
    uint64_t camera_wait_us = 0;
    uint64_t dqbuf_duration_us = 0;
    uint64_t encode_start_us = 0;
    uint64_t encode_done_us = 0;
    uint64_t driver_timestamp_us = 0;
    uint64_t previous_timestamp_us = 0;
    uint64_t known_stage_us = 0;
    uint64_t frame_period_us = 0;
    uint64_t gap_frames = 0;
    uint64_t gap_elapsed_ms = 0;
    uint64_t timestamp_delta_us = 0;
    uint64_t previous_encode_total_us = 0;
    uint64_t previous_submit_us = 0;
    uint32_t sequence = 0;
    uint32_t previous_sequence = 0;
    bool have_previous_frame = false;
    MppEncoderTiming timing = {};
    struct pollfd capture_poll_fd = {};
    int poll_ret = -1;

    if (runtime == NULL || !runtime->capture_ready ||
        (!capture_only && !runtime->encoder_ready) || phase_control == NULL) {
        std::fprintf(stderr, "[ERROR] DMA-BUF worker[%zu] has invalid runtime\n", stream_index);
        abortTestPhase(phase_control);
        return;
    }
    if (!drainWarmupUntilFormalStart(stream_index, warmup_seconds,
                                     phase_control, runtime, &start_time)) {
        std::fprintf(stderr, "[ERROR] DMA-BUF worker[%zu] warmup failed\n",
                     stream_index);
        return;
    }
    result = &runtime->result;
    *result = StreamResult();
    gate_divisor = sensor_fps / runtime->config.fps;
    frame_period_us = 1000000ULL / static_cast<uint64_t>(runtime->config.fps);
    capture_poll_fd.fd = runtime->capture.fd;
    capture_poll_fd.events = POLLIN | POLLPRI;

    end_time = start_time + std::chrono::seconds(duration_seconds);
    while (std::chrono::steady_clock::now() < end_time) {
        /*
         * V4L2 fd默认是阻塞模式。先用poll等待帧就绪，既避免正常路径忙轮询，
         * 也确保Sensor或某个ISP输出节点停止产帧时worker能够超时退出并打印结果。
         */
        capture_poll_fd.revents = 0;
        do {
            poll_ret = poll(&capture_poll_fd, 1, kCapturePollTimeoutMs);
        } while (poll_ret < 0 && errno == EINTR &&
                 std::chrono::steady_clock::now() < end_time);
        if (std::chrono::steady_clock::now() >= end_time) break;
        if (poll_ret == 0) {
            ++result->capture_timeouts;
            std::fprintf(stderr,
                         "[ERROR] DMA-BUF worker[%zu] capture poll timeout: device=%s count=%llu\n",
                         stream_index, runtime->config.device_path.c_str(),
                         static_cast<unsigned long long>(result->capture_timeouts));
            continue;
        }
        if (poll_ret < 0 ||
            (capture_poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ++result->failed_frames;
            std::fprintf(stderr,
                         "[ERROR] DMA-BUF worker[%zu] capture poll failed: device=%s "
                         "ret=%d revents=0x%x errno=%d\n",
                         stream_index, runtime->config.device_path.c_str(), poll_ret,
                         capture_poll_fd.revents, errno);
            break;
        }

        buffer_ref = NULL;
        ret = v4l2_capture_acquire_frame(&runtime->capture, &frame_data, &frame_len,
                                         &frame_id, &dqbuf_ts_us, &camera_wait_us,
                                         &dqbuf_duration_us, &buffer_ref);
        if (ret != 0 || buffer_ref == NULL) {
            ++result->failed_frames;
            std::fprintf(stderr,
                         "[ERROR] DMA-BUF worker[%zu] acquire frame failed ret=%d\n",
                         stream_index, ret);
            break;
        }
        if (std::chrono::steady_clock::now() >= end_time) {
            v4l2_capture_buffer_unref(buffer_ref);
            break;
        }

        ++result->captured_frames;
        sequence = v4l2_capture_buffer_sequence(buffer_ref);
        driver_timestamp_us = v4l2_capture_buffer_timestamp_us(buffer_ref);
        result->dqbuf_ioctl_us.push_back(dqbuf_duration_us);
        result->camera_buffer_wait_us.push_back(camera_wait_us);
        if (have_previous_frame) {
            if (sequence > previous_sequence + 1U) {
                gap_frames = sequence - previous_sequence - 1U;
                gap_elapsed_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count());
                timestamp_delta_us = driver_timestamp_us > previous_timestamp_us
                    ? driver_timestamp_us - previous_timestamp_us : 0;
                result->sequence_gaps += gap_frames;
                ++result->gap_event_count;
                if (gap_frames > result->max_single_gap) {
                    result->max_single_gap = gap_frames;
                }
                if (result->gap_event_count == 1U) {
                    result->first_gap_elapsed_ms = gap_elapsed_ms;
                }
                result->last_gap_elapsed_ms = gap_elapsed_ms;
                std::fprintf(stderr,
                             "[GAP] stream=%zu device=%s elapsed_ms=%llu "
                             "previous_sequence=%u current_sequence=%u gap_frames=%llu "
                             "previous_driver_timestamp_us=%llu "
                             "current_driver_timestamp_us=%llu timestamp_delta_us=%llu "
                             "camera_buffer_wait_us=%llu last_encode_total_us=%llu "
                             "last_submit_us=%llu "
                             "captured=%llu deadline_misses=%llu\n",
                             stream_index, runtime->config.device_path.c_str(),
                             static_cast<unsigned long long>(gap_elapsed_ms),
                             previous_sequence, sequence,
                             static_cast<unsigned long long>(gap_frames),
                             static_cast<unsigned long long>(previous_timestamp_us),
                             static_cast<unsigned long long>(driver_timestamp_us),
                             static_cast<unsigned long long>(timestamp_delta_us),
                             static_cast<unsigned long long>(camera_wait_us),
                             static_cast<unsigned long long>(previous_encode_total_us),
                             static_cast<unsigned long long>(previous_submit_us),
                             static_cast<unsigned long long>(result->captured_frames),
                             static_cast<unsigned long long>(result->deadline_misses));
            }
            if (driver_timestamp_us > previous_timestamp_us) {
                result->capture_interval_us.push_back(
                    driver_timestamp_us - previous_timestamp_us);
            }
        }
        previous_sequence = sequence;
        previous_timestamp_us = driver_timestamp_us;
        have_previous_frame = true;

        /* 纯采集模式保持相同DQBUF/QBUF路径，但不持有buffer等待MPP编码。 */
        if (capture_only) {
            v4l2_capture_buffer_unref(buffer_ref);
            buffer_ref = NULL;
            continue;
        }

        /* 低帧率码流仍及时归还不编码的采集buffer，避免拖空V4L2队列。 */
        if (((result->captured_frames - 1U) % static_cast<uint64_t>(gate_divisor)) != 0U) {
            ++result->gated_frames;
            v4l2_capture_buffer_unref(buffer_ref);
            continue;
        }

        dmabuf_fd = v4l2_capture_buffer_dmabuf_fd(buffer_ref);
        dmabuf_size = v4l2_capture_buffer_size(buffer_ref);
        encoded_data = NULL;
        encoded_size = 0;
        key_frame = 0;
        encode_start_us = 0;
        encode_done_us = 0;
        std::memset(&timing, 0, sizeof(timing));
        ret = mpp_encoder_encode_dmabuf(&runtime->encoder, dmabuf_fd, dmabuf_size,
                                        frame_id, &encoded_data, &encoded_size,
                                        &key_frame, &encode_start_us, &encode_done_us,
                                        &timing);
        v4l2_capture_buffer_unref(buffer_ref);
        buffer_ref = NULL;

        ++result->submitted_frames;
        if (ret != 0) {
            ++result->failed_frames;
            std::fprintf(stderr,
                         "[ERROR] DMA-BUF worker[%zu] encode failed frame=%llu ret=%d\n",
                         stream_index,
                         static_cast<unsigned long long>(frame_id), ret);
        } else if (encoded_data == NULL || encoded_size == 0) {
            ++result->empty_packets;
        } else {
            ++result->encoded_frames;
            result->encoded_bytes += encoded_size;
            result->key_frames += key_frame != 0 ? 1U : 0U;
        }

        result->dqbuf_to_encode_us.push_back(
            encode_start_us >= dqbuf_ts_us ? encode_start_us - dqbuf_ts_us : 0);
        result->dmabuf_import_us.push_back(timing.encoder_input_buffer_import_us);
        result->submit_us.push_back(timing.encoder_submit_frame_call_us);
        result->packet_wait_us.push_back(timing.encoder_poll_packet_call_us);
        result->packet_copy_us.push_back(timing.encoder_packet_copy_us);
        known_stage_us = timing.encoder_input_buffer_import_us +
                         timing.encoder_submit_frame_call_us +
                         timing.encoder_poll_packet_call_us +
                         timing.encoder_packet_copy_us;
        result->other_us.push_back(
            calculateUnaccountedLatency(timing.encode_frame_total_us, known_stage_us));
        result->total_us.push_back(timing.encode_frame_total_us);
        if (timing.encode_frame_total_us > frame_period_us) {
            ++result->deadline_misses;
            ++result->consecutive_deadline_misses;
            if (result->consecutive_deadline_misses >
                result->max_consecutive_deadline_misses) {
                result->max_consecutive_deadline_misses =
                    result->consecutive_deadline_misses;
            }
        } else {
            result->consecutive_deadline_misses = 0;
        }
        previous_encode_total_us = timing.encode_frame_total_us;
        previous_submit_us = timing.encoder_submit_frame_call_us;
    }
    completed_time = std::chrono::steady_clock::now();
    result->elapsed_seconds =
        std::chrono::duration<double>(completed_time - start_time).count();
}

/**
 * @brief 以统一格式打印一组延迟样本。
 */
void printLatency(const char *name, const std::vector<uint64_t> &samples) {
    LatencyStatistics statistics = {};

    if (name == NULL) {
        std::fprintf(stderr, "[ERROR] print DMA-BUF latency failed: name is null\n");
        return;
    }
    if (!calculateLatencyStatistics(samples, &statistics)) {
        std::printf("    %-16s samples=0 unavailable\n", name);
        return;
    }
    std::printf("    %-16s samples=%zu avg=%llu p50=%llu p95=%llu p99=%llu max=%llu\n",
                name, statistics.sample_count,
                static_cast<unsigned long long>(statistics.average_us),
                static_cast<unsigned long long>(statistics.p50_us),
                static_cast<unsigned long long>(statistics.p95_us),
                static_cast<unsigned long long>(statistics.p99_us),
                static_cast<unsigned long long>(statistics.max_us));
}

/**
 * @brief 打印单路结果并执行通过条件判断。
 */
bool printStreamResult(size_t index,
                       int sensor_fps,
                       bool capture_only,
                       const StreamRuntime &runtime) {
    const StreamResult &result = runtime.result;
    double capture_fps = 0.0;
    double encoded_fps = 0.0;
    double bitrate = 0.0;
    double fps_ratio = 0.0;
    bool passed = false;

    if (result.elapsed_seconds > 0.0) {
        capture_fps = result.captured_frames / result.elapsed_seconds;
        encoded_fps = result.encoded_frames / result.elapsed_seconds;
        bitrate = result.encoded_bytes * 8.0 / result.elapsed_seconds;
    }
    fps_ratio = capture_only ? 0.0 : encoded_fps / runtime.config.fps;
    passed = result.failed_frames == 0 && result.sequence_gaps == 0 &&
             result.capture_timeouts == 0 &&
             capture_fps / sensor_fps >= kMinimumFpsRatio;
    if (!capture_only) {
        passed = passed && result.empty_packets == 0 &&
                 fps_ratio >= kMinimumFpsRatio;
    }

    std::printf("\nstream[%zu] mode=%s device=%s %dx%d encode_fps=%d source_fps=%d\n",
                index, capture_only ? "capture-only" : "encode",
                runtime.config.device_path.c_str(), runtime.config.width,
                runtime.config.height, runtime.config.fps, sensor_fps);
    std::printf("  elapsed=%.3fs captured=%llu gated=%llu sequence_gaps=%llu "
                "capture_timeouts=%llu "
                "submitted=%llu encoded=%llu empty=%llu failed=%llu key=%llu\n",
                result.elapsed_seconds,
                static_cast<unsigned long long>(result.captured_frames),
                static_cast<unsigned long long>(result.gated_frames),
                static_cast<unsigned long long>(result.sequence_gaps),
                static_cast<unsigned long long>(result.capture_timeouts),
                static_cast<unsigned long long>(result.submitted_frames),
                static_cast<unsigned long long>(result.encoded_frames),
                static_cast<unsigned long long>(result.empty_packets),
                static_cast<unsigned long long>(result.failed_frames),
                static_cast<unsigned long long>(result.key_frames));
    std::printf("  gap_events=%llu max_single_gap=%llu first_gap_ms=%llu "
                "last_gap_ms=%llu\n",
                static_cast<unsigned long long>(result.gap_event_count),
                static_cast<unsigned long long>(result.max_single_gap),
                static_cast<unsigned long long>(result.first_gap_elapsed_ms),
                static_cast<unsigned long long>(result.last_gap_elapsed_ms));
    if (capture_only) {
        std::printf("  capture_fps=%.2f source_ratio=%.2f%%\n",
                    capture_fps, capture_fps / sensor_fps * 100.0);
    } else {
        std::printf("  capture_fps=%.2f encoded_fps=%.2f target_ratio=%.2f%% "
                    "actual_bitrate=%.0fbit/s deadline_misses=%llu "
                    "max_consecutive_deadline_misses=%llu\n",
                    capture_fps, encoded_fps, fps_ratio * 100.0, bitrate,
                    static_cast<unsigned long long>(result.deadline_misses),
                    static_cast<unsigned long long>(
                        result.max_consecutive_deadline_misses));
    }
    std::printf("  capture_latency_us:\n");
    printLatency("frame_interval", result.capture_interval_us);
    printLatency("buffer_wait", result.camera_buffer_wait_us);
    printLatency("dqbuf_ioctl", result.dqbuf_ioctl_us);
    if (!capture_only) {
        printLatency("dqbuf_to_encode", result.dqbuf_to_encode_us);
        std::printf("  encode_latency_us:\n");
        printLatency("dmabuf_import", result.dmabuf_import_us);
        printLatency("submit", result.submit_us);
        printLatency("packet_wait", result.packet_wait_us);
        printLatency("packet_copy", result.packet_copy_us);
        printLatency("other", result.other_us);
        printLatency("total", result.total_us);
    }
    std::printf("  result=%s\n", passed ? "PASS" : "FAIL");
    return passed;
}

} // namespace

/**
 * @brief 初始化真实采集链路、统一设置Sensor帧率并执行DMA-BUF容量测试。
 */
int main(int argc, char **argv) {
    TestConfig config = {};
    std::vector<StreamRuntime> runtimes;
    std::vector<std::thread> workers;
    TestPhaseControl phase_control = {};
    SystemMetricsMonitor metrics_monitor;
    SystemMetricsConfig metrics_config = {};
    bool metrics_started = false;
    bool initialized = true;
    bool all_passed = true;
    size_t index = 0;
    int ret = -1;

    /* 设备终端和重定向场景都按行刷新，避免长测期间初始化信息滞留在stdio缓存。 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (!parseArguments(argc, argv, &config)) {
        printUsage(argv != NULL && argv[0] != NULL ? argv[0] : "mpp_encoder_dmabuf_test");
        return EXIT_FAILURE;
    }
    std::printf("[INFO] V4L2 DMA-BUF capacity test: mode=%s duration=%ds "
                "warmup=%ds sensor_fps=%d buffer_count=%d streams=%zu\n",
                config.capture_only ? "capture-only" : "encode",
                config.duration_seconds, config.warmup_seconds,
                config.sensor_fps, config.buffer_count, config.streams.size());

    runtimes.resize(config.streams.size());
    for (index = 0; index < runtimes.size(); ++index) {
        if (!initializeStream(config.streams[index], config.buffer_count,
                              config.capture_only, &runtimes[index])) {
            initialized = false;
            break;
        }
    }
    if (initialized) {
        /* 同一Sensor派生的多个V4L2节点只设置一次最高源帧率。 */
        ret = v4l2_capture_set_fps(&runtimes[0].capture, config.sensor_fps);
        if (ret != 0) {
            std::fprintf(stderr, "[ERROR] set sensor fps failed: fps=%d ret=%d\n",
                         config.sensor_fps, ret);
            initialized = false;
        }
    }
    if (!initialized) {
        for (index = 0; index < runtimes.size(); ++index) deinitializeStream(&runtimes[index]);
        return EXIT_FAILURE;
    }

    /*
     * Sensor帧率切换后，并行排空各V4L2节点中的过渡帧。正式统计从预热完成后
     * 重新开始，避免把启动阶段允许出现的序号跳变误判成稳态丢帧。
     */
    /*
     * 每路只创建一个worker。worker先连续排空预热帧，达到预热时长后报告ready，但仍继续
     * DQBUF/QBUF。这样主线程启动系统指标监控时，V4L2队列不会因无人消费而积满。
     */
    try {
        workers.reserve(runtimes.size());
        for (index = 0; index < runtimes.size(); ++index) {
            workers.push_back(std::thread(runStreamWorker, index, config.sensor_fps,
                                          config.duration_seconds,
                                          config.warmup_seconds,
                                          config.capture_only, &phase_control,
                                          &runtimes[index]));
        }
    } catch (...) {
        std::fprintf(stderr, "[ERROR] create DMA-BUF worker failed\n");
        abortTestPhase(&phase_control);
        all_passed = false;
    }

    if (workers.size() == runtimes.size()) {
        std::unique_lock<std::mutex> lock(phase_control.mutex);
        phase_control.condition.wait(lock, [&phase_control, &runtimes]() {
            return phase_control.aborted ||
                   phase_control.ready_workers == runtimes.size();
        });
    } else {
        abortTestPhase(&phase_control);
    }

    if (!isTestPhaseAborted(&phase_control) && config.metrics_enabled) {
        metrics_config.sample_interval_ms = config.metrics_interval_ms;
        metrics_config.csv_path = config.metrics_file;
        metrics_started = metrics_monitor.start(metrics_config);
        if (!metrics_started) {
            std::fprintf(stderr, "[WARN] system metrics unavailable; test continues\n");
        }
    }

    /*
     * 发布所有worker共用的正式统计起点。发布之前，各worker一直消费并归还预热帧，
     * 因此预热、指标监控初始化和正式统计之间不存在采集空窗。
     */
    {
        std::lock_guard<std::mutex> lock(phase_control.mutex);
        if (!phase_control.aborted) {
            phase_control.formal_start_time = std::chrono::steady_clock::now();
            phase_control.formal_started = true;
        } else {
            all_passed = false;
        }
        phase_control.condition.notify_all();
    }
    for (index = 0; index < workers.size(); ++index) {
        if (workers[index].joinable()) workers[index].join();
    }
    if (metrics_started) metrics_monitor.stop();

    for (index = 0; index < runtimes.size(); ++index) {
        std::printf("[INFO] DMA-BUF warmup complete: stream=%zu device=%s "
                    "captured=%llu sequence_gaps=%llu timeouts=%llu failed=%llu\n",
                    index, runtimes[index].config.device_path.c_str(),
                    static_cast<unsigned long long>(runtimes[index].warmup.captured_frames),
                    static_cast<unsigned long long>(runtimes[index].warmup.sequence_gaps),
                    static_cast<unsigned long long>(runtimes[index].warmup.capture_timeouts),
                    static_cast<unsigned long long>(runtimes[index].warmup.failed_frames));
        if (runtimes[index].warmup.captured_frames == 0 ||
            runtimes[index].warmup.capture_timeouts != 0 ||
            runtimes[index].warmup.failed_frames != 0) {
            all_passed = false;
        }
    }

    std::printf("\n========== V4L2 DMA-BUF CAPACITY RESULT ==========\n");
    if (workers.size() != runtimes.size()) all_passed = false;
    for (index = 0; index < runtimes.size(); ++index) {
        if (!printStreamResult(index, config.sensor_fps, config.capture_only,
                               runtimes[index])) {
            all_passed = false;
        }
    }
    std::printf("\noverall=%s\n", all_passed ? "PASS" : "FAIL");
    if (metrics_started) metrics_monitor.printSummary();

    for (index = 0; index < runtimes.size(); ++index) deinitializeStream(&runtimes[index]);
    return all_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
