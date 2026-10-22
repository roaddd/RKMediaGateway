/**
 * @file mpp_encoder_capacity_test.cpp
 * @brief RK3568 MPP H.264 多实例并发编码能力测试程序。
 *
 * 本程序不接入摄像头、协议输出和音频链路，只使用预生成的动态 NV12
 * 测试图案驱动多个 MPP 编码器实例，用于测量编码模块本身在指定组合下
 * 能否持续达到目标帧率。完整产品能力仍需在本测试通过后使用 rkmgw 验证。
 */

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "latencyStatistics.h"
#include "mppEncoder.h"
#include "systemMetricsMonitor.h"

namespace {

const int kDefaultDurationSeconds = 60;
const int kPatternCount = 8;
const double kMinimumFpsRatio = 0.98;
const int kDefaultMetricsIntervalMs = 1000;
const char *kDefaultMetricsFile = "mpp_capacity_metrics.csv";

/** 单路编码器的测试配置。 */
struct StreamConfig {
    int width;       /* NV12图像宽度。 */
    int height;      /* NV12图像高度。 */
    int fps;         /* 目标送帧及编码帧率。 */
    int bitrate;     /* H.264目标码率，单位bit/s。 */
    int gop;         /* 相邻IDR之间的目标帧数。 */
};

/** 测试程序的完整运行参数。 */
struct TestConfig {
    int duration_seconds;              /* 正式统计持续时间，单位秒。 */
    std::vector<StreamConfig> streams; /* 同时运行的编码实例配置。 */
    bool metrics_enabled;              /* 是否记录测试期间的系统状态。 */
    int metrics_interval_ms;           /* 系统状态采样周期，单位毫秒。 */
    std::string metrics_file;          /* 系统状态CSV输出路径。 */
};

/** 单路编码线程收集的原始统计数据。 */
struct StreamResult {
    uint64_t submitted_frames;       /* 已调用编码接口的输入帧数。 */
    uint64_t encoded_frames;         /* 实际取得非空编码包的帧数。 */
    uint64_t empty_packets;          /* 编码成功但本次未取到输出包的次数。 */
    uint64_t failed_frames;          /* 编码接口返回失败的帧数。 */
    uint64_t key_frames;             /* 输出关键帧数量。 */
    uint64_t encoded_bytes;          /* 编码输出总字节数。 */
    uint64_t deadline_misses;        /* 编码完成时间晚于该帧计划周期的次数。 */
    double elapsed_seconds;          /* 本线程正式测试的实际持续时间。 */
    std::vector<uint64_t> input_copy_latency_us; /* 输入NV12复制和stride重排耗时。 */
    std::vector<uint64_t> submit_latency_us;     /* MPP提交输入帧调用耗时。 */
    std::vector<uint64_t> packet_wait_latency_us; /* 等待MPP输出编码包耗时。 */
    std::vector<uint64_t> packet_copy_latency_us; /* 编码包复制到复用缓存耗时。 */
    std::vector<uint64_t> other_latency_us;      /* 总耗时中未归属上述阶段的耗时。 */
    std::vector<uint64_t> total_latency_us;      /* 每帧编码接口总耗时。 */
};

/** 多线程统一起跑屏障，避免先启动的编码器提前消耗统计时间。 */
class StartBarrier {
public:
    explicit StartBarrier(size_t worker_count)
        : worker_count_(worker_count), ready_count_(0), start_(false), abort_(false) {
    }

    /**
     * @brief 工作线程报告初始化结果，并等待主线程统一发令。
     * @param init_success 当前线程的编码器是否初始化成功。
     * @return true表示开始测试，false表示任一线程初始化失败，应当退出。
     */
    bool arriveAndWait(bool init_success) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (!init_success) {
            abort_ = true;
        }
        ++ready_count_;
        condition_.notify_all();
        condition_.wait(lock, [this]() { return start_ || abort_; });
        return start_ && !abort_;
    }

    /**
     * @brief 等待所有工作线程完成初始化，然后统一启动或终止测试。
     * @return true表示所有线程初始化成功。
     */
    bool releaseWorkers() {
        std::unique_lock<std::mutex> lock(mutex_);

        condition_.wait(lock, [this]() { return ready_count_ == worker_count_; });
        if (abort_) {
            condition_.notify_all();
            return false;
        }
        start_ = true;
        condition_.notify_all();
        return true;
    }

    /** @brief 线程创建异常时唤醒已经进入屏障的工作线程。 */
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);

        abort_ = true;
        condition_.notify_all();
    }

private:
    const size_t worker_count_;       /* 本轮应到达屏障的工作线程总数。 */
    size_t ready_count_;              /* 已完成编码器初始化的线程数。 */
    bool start_;                      /* 主线程是否已经发出统一启动信号。 */
    bool abort_;                      /* 是否因任一线程初始化失败而终止。 */
    std::mutex mutex_;                /* 保护屏障状态。 */
    std::condition_variable condition_; /* 等待线程就绪和统一启动。 */
};

/**
 * @brief 打印命令行帮助。
 * @param program_name 当前程序名称。
 */
void printUsage(const char *program_name) {
    std::printf(
        "Usage:\n"
        "  %s [options] --stream WIDTHxHEIGHT@FPS:BITRATE:GOP [--stream ...]\n"
        "  %s [options] --preset current|target|main60|stress\n\n"
        "Options:\n"
        "  --duration SECONDS       Test duration, default 60\n"
        "  --metrics-interval MS    Metrics interval, default 1000\n"
        "  --metrics-file PATH      Metrics CSV, default mpp_capacity_metrics.csv\n"
        "  --no-metrics             Disable system metrics collection\n\n"
        "Presets:\n"
        "  current : 1920x1080@30 + 1280x720@30\n"
        "  target  : 1920x1080@60 + 1280x720@30\n"
        "  main60  : 1920x1080@60\n"
        "  stress  : 1920x1080@60 + 1280x720@60\n\n"
        "Examples:\n"
        "  %s --preset current --duration 600\n"
        "  %s --duration 600 --stream 1920x1080@60:12582912:60 "
        "--stream 1280x720@30:2097152:30\n",
        program_name,
        program_name,
        program_name,
        program_name);
}

/**
 * @brief 解析一个自定义码流描述。
 * @param text 格式为WIDTHxHEIGHT@FPS:BITRATE:GOP的字符串。
 * @param config 输出码流配置。
 * @return true表示解析及范围校验成功。
 */
bool parseStreamConfig(const char *text, StreamConfig *config) {
    int matched = 0;
    char trailing = '\0';

    if (text == NULL || config == NULL) {
        std::fprintf(stderr, "[ERROR] parse stream failed: null argument\n");
        return false;
    }

    matched = std::sscanf(text,
                          "%dx%d@%d:%d:%d%c",
                          &config->width,
                          &config->height,
                          &config->fps,
                          &config->bitrate,
                          &config->gop,
                          &trailing);
    if (matched != 5) {
        std::fprintf(stderr, "[ERROR] invalid stream format: %s\n", text);
        return false;
    }
    if (config->width <= 0 || config->height <= 0 ||
        (config->width % 2) != 0 || (config->height % 2) != 0 ||
        config->fps <= 0 || config->bitrate <= 0 || config->gop <= 0) {
        std::fprintf(stderr,
                     "[ERROR] invalid stream values: width=%d height=%d fps=%d "
                     "bitrate=%d gop=%d\n",
                     config->width,
                     config->height,
                     config->fps,
                     config->bitrate,
                     config->gop);
        return false;
    }
    return true;
}

/**
 * @brief 按名称填充已知测试组合。
 * @param name 预设名称。
 * @param streams 输出码流配置集合。
 * @return true表示预设存在。
 */
bool appendPreset(const std::string &name, std::vector<StreamConfig> *streams) {
    StreamConfig main_stream = {1920, 1080, 30, 6291456, 30};
    StreamConfig sub_stream = {1280, 720, 30, 1048576, 30};

    if (streams == NULL) {
        std::fprintf(stderr, "[ERROR] append preset failed: streams is null\n");
        return false;
    }
    streams->clear();
    if (name == "current") {
        streams->push_back(main_stream);
        streams->push_back(sub_stream);
    } else if (name == "target") {
        main_stream.fps = 60;
        main_stream.bitrate = 12582912;
        main_stream.gop = 60;
        streams->push_back(main_stream);
        streams->push_back(sub_stream);
    } else if (name == "main60") {
        main_stream.fps = 60;
        main_stream.bitrate = 12582912;
        main_stream.gop = 60;
        streams->push_back(main_stream);
    } else if (name == "stress") {
        main_stream.fps = 60;
        main_stream.bitrate = 12582912;
        main_stream.gop = 60;
        sub_stream.fps = 60;
        sub_stream.bitrate = 2097152;
        sub_stream.gop = 60;
        streams->push_back(main_stream);
        streams->push_back(sub_stream);
    } else {
        std::fprintf(stderr, "[ERROR] unknown preset: %s\n", name.c_str());
        return false;
    }
    return true;
}

/**
 * @brief 解析测试程序命令行。
 * @return true表示参数完整且合法。
 */
bool parseArguments(int argc, char **argv, TestConfig *config) {
    int index = 0;
    long duration = 0;
    char *end = NULL;
    StreamConfig stream = {};

    if (argv == NULL || config == NULL) {
        std::fprintf(stderr, "[ERROR] parse arguments failed: null argument\n");
        return false;
    }

    config->duration_seconds = kDefaultDurationSeconds;
    config->streams.clear();
    config->metrics_enabled = true;
    config->metrics_interval_ms = kDefaultMetricsIntervalMs;
    config->metrics_file = kDefaultMetricsFile;
    for (index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--duration") == 0) {
            if (++index >= argc) {
                std::fprintf(stderr, "[ERROR] --duration requires a value\n");
                return false;
            }
            end = NULL;
            duration = std::strtol(argv[index], &end, 10);
            if (end == argv[index] || *end != '\0' || duration <= 0 || duration > 86400) {
                std::fprintf(stderr, "[ERROR] invalid duration: %s\n", argv[index]);
                return false;
            }
            config->duration_seconds = static_cast<int>(duration);
        } else if (std::strcmp(argv[index], "--metrics-interval") == 0) {
            if (++index >= argc) {
                std::fprintf(stderr, "[ERROR] --metrics-interval requires a value\n");
                return false;
            }
            end = NULL;
            duration = std::strtol(argv[index], &end, 10);
            if (end == argv[index] || *end != '\0' || duration < 100 || duration > 60000) {
                std::fprintf(stderr, "[ERROR] invalid metrics interval: %s\n", argv[index]);
                return false;
            }
            config->metrics_interval_ms = static_cast<int>(duration);
        } else if (std::strcmp(argv[index], "--metrics-file") == 0) {
            if (++index >= argc || argv[index][0] == '\0') {
                std::fprintf(stderr, "[ERROR] --metrics-file requires a path\n");
                return false;
            }
            config->metrics_file = argv[index];
        } else if (std::strcmp(argv[index], "--no-metrics") == 0) {
            config->metrics_enabled = false;
        } else if (std::strcmp(argv[index], "--stream") == 0) {
            if (++index >= argc || !parseStreamConfig(argv[index], &stream)) {
                return false;
            }
            config->streams.push_back(stream);
        } else if (std::strcmp(argv[index], "--preset") == 0) {
            if (++index >= argc || !appendPreset(argv[index], &config->streams)) {
                return false;
            }
        } else if (std::strcmp(argv[index], "--help") == 0 ||
                   std::strcmp(argv[index], "-h") == 0) {
            printUsage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            std::fprintf(stderr, "[ERROR] unknown argument: %s\n", argv[index]);
            return false;
        }
    }

    if (config->streams.empty()) {
        std::fprintf(stderr, "[ERROR] at least one --stream or --preset is required\n");
        return false;
    }
    return true;
}

/**
 * @brief 生成多幅具有空间细节和帧间变化的紧凑NV12测试图案。
 *
 * 图案只在测试开始前生成，编码期间循环使用，从而避免逐帧绘图的CPU开销
 * 干扰MPP容量结果；不同图案中的亮度块和色度值发生位移，可持续产生帧间变化。
 */
bool createNv12Patterns(const StreamConfig &config,
                        std::vector<std::vector<uint8_t> > *patterns) {
    size_t frame_size = 0;
    size_t y_offset = 0;
    size_t uv_offset = 0;
    int pattern_index = 0;
    int x = 0;
    int y = 0;
    uint8_t luma = 0;

    if (patterns == NULL) {
        std::fprintf(stderr, "[ERROR] create NV12 patterns failed: output is null\n");
        return false;
    }

    frame_size = static_cast<size_t>(config.width) * config.height * 3U / 2U;
    try {
        patterns->assign(kPatternCount, std::vector<uint8_t>(frame_size, 0));
    } catch (...) {
        std::fprintf(stderr,
                     "[ERROR] allocate NV12 patterns failed: size=%zu count=%d\n",
                     frame_size,
                     kPatternCount);
        return false;
    }

    for (pattern_index = 0; pattern_index < kPatternCount; ++pattern_index) {
        for (y = 0; y < config.height; ++y) {
            for (x = 0; x < config.width; ++x) {
                y_offset = static_cast<size_t>(y) * config.width + x;
                luma = static_cast<uint8_t>(32 +
                    ((x / 16 + y / 16 + pattern_index * 3) % 12) * 16);
                if (((x + pattern_index * 48) % config.width) < config.width / 6 &&
                    ((y + pattern_index * 24) % config.height) < config.height / 4) {
                    luma = 235;
                }
                (*patterns)[pattern_index][y_offset] = luma;
            }
        }
        uv_offset = static_cast<size_t>(config.width) * config.height;
        for (y = 0; y < config.height / 2; ++y) {
            for (x = 0; x < config.width; x += 2) {
                (*patterns)[pattern_index][uv_offset +
                    static_cast<size_t>(y) * config.width + x] =
                    static_cast<uint8_t>(80 + pattern_index * 12);
                (*patterns)[pattern_index][uv_offset +
                    static_cast<size_t>(y) * config.width + x + 1] =
                    static_cast<uint8_t>(176 - pattern_index * 10);
            }
        }
    }
    return true;
}

/**
 * @brief 执行单路实时节拍编码测试。
 *
 * 每个线程持有独立MPP上下文和测试图案。所有实例初始化完成后由屏障统一
 * 起跑；线程按目标帧率计算绝对发送时刻，落后时不补睡眠，以便真实暴露
 * 编码器无法跟上目标节拍产生的deadline miss和最终FPS下降。
 */
void runStreamWorker(size_t stream_index,
                     const StreamConfig &config,
                     int duration_seconds,
                     StartBarrier *barrier,
                     StreamResult *result) {
    MppEncoderCtx encoder = {};
    MppEncoderOptions options = {};
    std::vector<std::vector<uint8_t> > patterns;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    std::chrono::steady_clock::time_point scheduled_time;
    std::chrono::steady_clock::time_point completed_time;
    std::chrono::nanoseconds frame_period(0);
    uint8_t *encoded_data = NULL;
    size_t encoded_size = 0;
    int key_frame = 0;
    int encode_ret = 0;
    bool init_success = false;
    uint64_t frame_id = 0;
    uint64_t known_stage_us = 0;
    MppEncoderTiming timing = {};

    if (barrier == NULL || result == NULL) {
        std::fprintf(stderr, "[ERROR] stream[%zu] worker failed: null argument\n", stream_index);
        return;
    }
    *result = StreamResult();

    try {
        result->input_copy_latency_us.reserve(
            static_cast<size_t>(duration_seconds) * config.fps + 1U);
        result->submit_latency_us.reserve(
            static_cast<size_t>(duration_seconds) * config.fps + 1U);
        result->packet_wait_latency_us.reserve(
            static_cast<size_t>(duration_seconds) * config.fps + 1U);
        result->packet_copy_latency_us.reserve(
            static_cast<size_t>(duration_seconds) * config.fps + 1U);
        result->other_latency_us.reserve(
            static_cast<size_t>(duration_seconds) * config.fps + 1U);
        result->total_latency_us.reserve(
            static_cast<size_t>(duration_seconds) * config.fps + 1U);
    } catch (...) {
        std::fprintf(stderr,
                     "[ERROR] stream[%zu] allocate latency samples failed: duration=%d fps=%d\n",
                     stream_index,
                     duration_seconds,
                     config.fps);
        barrier->arriveAndWait(false);
        return;
    }

    options.rc_mode = MPP_ENCODER_RC_MODE_CBR;
    options.h264_profile = MPP_ENCODER_H264_PROFILE_HIGH;
    options.h264_level = config.fps > 30
        ? MPP_ENCODER_H264_LEVEL_42 : MPP_ENCODER_H264_LEVEL_40;
    options.h264_cabac_en = MPP_ENCODER_CABAC_ENABLED;

    if (!createNv12Patterns(config, &patterns)) {
        std::fprintf(stderr, "[ERROR] stream[%zu] create patterns failed\n", stream_index);
        barrier->arriveAndWait(false);
        return;
    }
    encode_ret = mpp_encoder_init(&encoder,
                                  config.width,
                                  config.height,
                                  config.fps,
                                  config.bitrate,
                                  config.gop,
                                  &options);
    init_success = (encode_ret == 0);
    if (!init_success) {
        std::fprintf(stderr,
                     "[ERROR] stream[%zu] MPP init failed: %dx%d@%d bitrate=%d gop=%d ret=%d\n",
                     stream_index,
                     config.width,
                     config.height,
                     config.fps,
                     config.bitrate,
                     config.gop,
                     encode_ret);
    }
    if (!barrier->arriveAndWait(init_success)) {
        if (init_success) {
            mpp_encoder_deinit(&encoder);
        }
        return;
    }

    frame_period = std::chrono::nanoseconds(1000000000LL / config.fps);
    start_time = std::chrono::steady_clock::now();
    end_time = start_time + std::chrono::seconds(duration_seconds);
    scheduled_time = start_time;

    while (std::chrono::steady_clock::now() < end_time) {
        if (std::chrono::steady_clock::now() < scheduled_time) {
            std::this_thread::sleep_until(scheduled_time);
        }

        encoded_data = NULL;
        encoded_size = 0;
        key_frame = 0;
        std::memset(&timing, 0, sizeof(timing));
        encode_ret = mpp_encoder_encode_frame(
            &encoder,
            patterns[frame_id % patterns.size()].data(),
            patterns[frame_id % patterns.size()].size(),
            frame_id,
            &encoded_data,
            &encoded_size,
            &key_frame,
            NULL,
            NULL,
            &timing);

        ++result->submitted_frames;
        if (encode_ret < 0) {
            ++result->failed_frames;
            std::fprintf(stderr,
                         "[ERROR] stream[%zu] encode failed: frame=%llu ret=%d\n",
                         stream_index,
                         static_cast<unsigned long long>(frame_id),
                         encode_ret);
        } else if (encoded_data == NULL || encoded_size == 0) {
            ++result->empty_packets;
        } else {
            ++result->encoded_frames;
            result->encoded_bytes += encoded_size;
            result->key_frames += key_frame != 0 ? 1U : 0U;
        }
        /*
         * 所有阶段样本必须在同一帧上同步入列，保证各统计数组的样本数量一致。
         * other使用饱和减法，避免微秒级时钟采样误差导致无符号下溢。
         */
        known_stage_us = timing.encoder_input_buffer_copy_us +
                         timing.encoder_submit_frame_call_us +
                         timing.encoder_poll_packet_call_us +
                         timing.encoder_packet_copy_us;
        result->input_copy_latency_us.push_back(timing.encoder_input_buffer_copy_us);
        result->submit_latency_us.push_back(timing.encoder_submit_frame_call_us);
        result->packet_wait_latency_us.push_back(timing.encoder_poll_packet_call_us);
        result->packet_copy_latency_us.push_back(timing.encoder_packet_copy_us);
        result->other_latency_us.push_back(
            calculateUnaccountedLatency(timing.encode_frame_total_us, known_stage_us));
        result->total_latency_us.push_back(timing.encode_frame_total_us);

        ++frame_id;
        scheduled_time = start_time + frame_period * frame_id;
        completed_time = std::chrono::steady_clock::now();
        if (completed_time > scheduled_time) {
            ++result->deadline_misses;
        }
    }

    completed_time = std::chrono::steady_clock::now();
    result->elapsed_seconds =
        std::chrono::duration<double>(completed_time - start_time).count();
    mpp_encoder_deinit(&encoder);
}

/**
 * @brief 按统一格式打印一个编码阶段的延迟统计。
 * @param stage_name 阶段名称。
 * @param samples 该阶段逐帧采集的微秒耗时。
 */
void printLatencyStage(const char *stage_name,
                       const std::vector<uint64_t> &samples) {
    LatencyStatistics statistics = {};
    bool result = false;

    if (stage_name == NULL) {
        std::fprintf(stderr, "[ERROR] print latency stage failed: name is null\n");
        return;
    }
    result = calculateLatencyStatistics(samples, &statistics);
    if (!result) {
        std::printf("    %-12s samples=0 unavailable\n", stage_name);
        return;
    }
    std::printf("    %-12s samples=%zu avg=%llu p50=%llu p95=%llu p99=%llu max=%llu\n",
                stage_name,
                statistics.sample_count,
                static_cast<unsigned long long>(statistics.average_us),
                static_cast<unsigned long long>(statistics.p50_us),
                static_cast<unsigned long long>(statistics.p95_us),
                static_cast<unsigned long long>(statistics.p99_us),
                static_cast<unsigned long long>(statistics.max_us));
}

/**
 * @brief 打印单路测试结果并判断是否达到实时编码目标。
 * @return true表示无编码失败且实际输出FPS达到目标值的98%。
 */
bool printStreamResult(size_t stream_index,
                       const StreamConfig &config,
                       const StreamResult &result) {
    double submitted_fps = 0.0;
    double encoded_fps = 0.0;
    double actual_bitrate = 0.0;
    double fps_ratio = 0.0;
    bool passed = false;
    if (result.elapsed_seconds > 0.0) {
        submitted_fps = result.submitted_frames / result.elapsed_seconds;
        encoded_fps = result.encoded_frames / result.elapsed_seconds;
        actual_bitrate = result.encoded_bytes * 8.0 / result.elapsed_seconds;
    }
    fps_ratio = config.fps > 0 ? encoded_fps / config.fps : 0.0;
    passed = result.failed_frames == 0 &&
             result.empty_packets == 0 &&
             fps_ratio >= kMinimumFpsRatio;

    std::printf("\nstream[%zu] %dx%d@%d bitrate=%d gop=%d\n",
                stream_index,
                config.width,
                config.height,
                config.fps,
                config.bitrate,
                config.gop);
    std::printf("  elapsed=%.3fs submitted=%llu encoded=%llu empty=%llu failed=%llu key=%llu\n",
                result.elapsed_seconds,
                static_cast<unsigned long long>(result.submitted_frames),
                static_cast<unsigned long long>(result.encoded_frames),
                static_cast<unsigned long long>(result.empty_packets),
                static_cast<unsigned long long>(result.failed_frames),
                static_cast<unsigned long long>(result.key_frames));
    std::printf("  submitted_fps=%.2f encoded_fps=%.2f target_ratio=%.2f%% "
                "actual_bitrate=%.0fbit/s\n",
                submitted_fps,
                encoded_fps,
                fps_ratio * 100.0,
                actual_bitrate);
    std::printf("  latency_breakdown_us:\n");
    printLatencyStage("input_copy", result.input_copy_latency_us);
    printLatencyStage("submit", result.submit_latency_us);
    printLatencyStage("packet_wait", result.packet_wait_latency_us);
    printLatencyStage("packet_copy", result.packet_copy_latency_us);
    printLatencyStage("other", result.other_latency_us);
    printLatencyStage("total", result.total_latency_us);
    std::printf("  deadline_misses=%llu\n",
                static_cast<unsigned long long>(result.deadline_misses));
    std::printf("  result=%s\n", passed ? "PASS" : "FAIL");
    return passed;
}

} // namespace

/**
 * @brief 启动所有MPP编码实例并输出容量测试结论。
 * @return EXIT_SUCCESS表示全部码流达到目标，其他值表示参数、初始化或性能失败。
 */
int main(int argc, char **argv) {
    TestConfig config = {};
    std::vector<StreamResult> results;
    std::vector<std::thread> workers;
    StartBarrier *barrier = NULL;
    bool initialized = false;
    bool all_passed = true;
    size_t stream_index = 0;
    const StreamConfig *stream = NULL;
    uint64_t requested_pixels_per_second = 0;
    double actual_pixels_per_second = 0.0;
    SystemMetricsMonitor metrics_monitor;
    SystemMetricsConfig metrics_config = {};
    bool metrics_started = false;

    if (!parseArguments(argc, argv, &config)) {
        printUsage(argv != NULL && argv[0] != NULL ? argv[0] : "mpp_encoder_capacity_test");
        return EXIT_FAILURE;
    }

    std::printf("[INFO] MPP capacity test: duration=%ds streams=%zu pattern_count=%d\n",
                config.duration_seconds,
                config.streams.size(),
                kPatternCount);
    for (stream_index = 0; stream_index < config.streams.size(); ++stream_index) {
        stream = &config.streams[stream_index];
        std::printf("[INFO] stream[%zu]=%dx%d@%d bitrate=%d gop=%d\n",
                    stream_index,
                    stream->width,
                    stream->height,
                    stream->fps,
                    stream->bitrate,
                    stream->gop);
    }

    results.resize(config.streams.size());
    barrier = new (std::nothrow) StartBarrier(config.streams.size());
    if (barrier == NULL) {
        std::fprintf(stderr, "[ERROR] allocate start barrier failed\n");
        return EXIT_FAILURE;
    }

    try {
        workers.reserve(config.streams.size());
        for (stream_index = 0; stream_index < config.streams.size(); ++stream_index) {
            workers.push_back(std::thread(runStreamWorker,
                                          stream_index,
                                          std::cref(config.streams[stream_index]),
                                          config.duration_seconds,
                                          barrier,
                                          &results[stream_index]));
        }
        initialized = barrier->releaseWorkers();
        if (initialized && config.metrics_enabled) {
            metrics_config.sample_interval_ms = config.metrics_interval_ms;
            metrics_config.csv_path = config.metrics_file;
            metrics_started = metrics_monitor.start(metrics_config);
            if (!metrics_started) {
                std::fprintf(stderr,
                             "[WARN] system metrics monitor unavailable; "
                             "MPP capacity test continues\n");
            }
        }
    } catch (...) {
        std::fprintf(stderr, "[ERROR] create MPP capacity worker failed\n");
        barrier->cancel();
    }

    for (stream_index = 0; stream_index < workers.size(); ++stream_index) {
        if (workers[stream_index].joinable()) {
            workers[stream_index].join();
        }
    }
    if (metrics_started) {
        metrics_monitor.stop();
    }
    delete barrier;
    barrier = NULL;

    if (!initialized || workers.size() != config.streams.size()) {
        std::fprintf(stderr, "[ERROR] MPP capacity test initialization failed\n");
        return EXIT_FAILURE;
    }

    std::printf("\n========== MPP CAPACITY RESULT ==========\n");
    for (stream_index = 0; stream_index < config.streams.size(); ++stream_index) {
        if (!printStreamResult(stream_index, config.streams[stream_index], results[stream_index])) {
            all_passed = false;
        }
        requested_pixels_per_second +=
            static_cast<uint64_t>(config.streams[stream_index].width) *
            config.streams[stream_index].height *
            config.streams[stream_index].fps;
        if (results[stream_index].elapsed_seconds > 0.0) {
            actual_pixels_per_second +=
                static_cast<double>(config.streams[stream_index].width) *
                config.streams[stream_index].height *
                results[stream_index].encoded_frames /
                results[stream_index].elapsed_seconds;
        }
    }
    std::printf("\naggregate requested_pixels_per_second=%llu "
                "actual_pixels_per_second=%.0f\n",
                static_cast<unsigned long long>(requested_pixels_per_second),
                actual_pixels_per_second);
    std::printf("overall=%s\n", all_passed ? "PASS" : "FAIL");
    if (metrics_started) {
        metrics_monitor.printSummary();
    }
    return all_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
