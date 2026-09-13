/**
 * @file main_audio_decoder_test.cpp
 * @brief Opus 普通解码、带内 FEC、PLC 与 ALSA Playback 联合测试程序。
 *
 * 测试方法：
 *
 * 1. 根据实际硬件选择耳机或其他播放通路：
 *    amixer -c 0 cset name='Playback Path' HP
 *
 * 2. 分别执行三种模式，命令格式为：
 *    ./audio_decoder_test [device] [duration_seconds] [normal|fec|plc] [loss_interval]
 *
 *    示例：
 *    ./audio_decoder_test hw:0,0 10 normal
 *    ./audio_decoder_test hw:0,0 10 fec 50
 *    ./audio_decoder_test hw:0,0 10 plc 50
 *
 *    normal：所有生成的 Opus 包都执行普通解码。
 *    fec：暂存前一包；仅当当前包确实携带前一帧的 LBRR，且达到 loss_interval
 *         指定的最小间隔时，才模拟前一包丢失并执行带内 FEC 恢复。
 *    plc：每隔 loss_interval 个包固定模拟一次丢失，不向解码器提供后续包，验证
 *         opus_decode() 的本地丢包隐藏能力。
 *
 * 通过标准：程序退出码为 0、decode_errors=0、playback_xruns=0，并且：
 *    normal：inband_recovered=0，concealed=0；
 *    fec：inband_recovered=simulated_losses，concealed=0；
 *    plc：inband_recovered=0，concealed=simulated_losses。
 *
 * 说明：normal/PLC 使用连续 1 kHz 正弦波；FEC 使用带基频、谐波和幅度包络的
 * 语音型测试波形。FEC 波形的目的只是让 Opus SILK 更稳定地产生 LBRR，以确定性
 * 覆盖真正的带内 FEC 分支，不用于评价 Opus 的主观音质。
 */

#include "audioDecoder.h"
#include "audioPlayback.h"
#include "logger.h"
#include "opus.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

/** 测试采用 WebRTC 常用的 48 kHz、20 ms 单声道 Opus 帧。 */
static const int AUDIO_DECODER_TEST_SAMPLE_RATE = 48000;
static const int AUDIO_DECODER_TEST_OPUS_CHANNELS = 1;
static const int AUDIO_DECODER_TEST_FRAME_SAMPLES = 960;
static const int AUDIO_DECODER_TEST_MAX_FRAME_SAMPLES = 5760;
static const int AUDIO_DECODER_TEST_MAX_PACKET_BYTES = 4000;
static const int AUDIO_DECODER_TEST_BITRATE = 24000;
static const int AUDIO_DECODER_TEST_DEFAULT_LOSS_INTERVAL = 50;
static const int AUDIO_DECODER_TEST_FEC_LOSS_PERCENT = 15;

/** 测试音参数使用较低幅度，避免耳机播放时音量过大。 */
static const double AUDIO_DECODER_TEST_TONE_HZ = 1000.0;
static const double AUDIO_DECODER_TEST_AMPLITUDE = 0.08;
static const double AUDIO_DECODER_TEST_PI = 3.14159265358979323846;

/** @brief 音频解码测试运行模式。 */
enum AudioDecoderTestMode {
    AUDIO_DECODER_TEST_MODE_NORMAL = 0, /* 所有 Opus 包均执行普通解码。 */
    AUDIO_DECODER_TEST_MODE_FEC,        /* 周期性丢包，并用下一包的 LBRR 恢复。 */
    AUDIO_DECODER_TEST_MODE_PLC         /* 周期性丢包，并在无后续包输入时执行 PLC。 */
};

/** @description: 返回测试模式对应的日志名称。 */
static const char *audio_decoder_test_mode_name(AudioDecoderTestMode mode)
{
    switch (mode) {
    case AUDIO_DECODER_TEST_MODE_NORMAL:
        return "normal";
    case AUDIO_DECODER_TEST_MODE_FEC:
        return "fec";
    case AUDIO_DECODER_TEST_MODE_PLC:
        return "plc";
    default:
        return "unknown";
    }
}

/** @description: 将命令行文本严格解析为音频解码测试模式。 */
static MediaResult parse_test_mode(const char *text, AudioDecoderTestMode *mode)
{
    if (text == NULL || mode == NULL) {
        LOG_ERROR("audio decoder test parse mode failed: text=%p output=%p",
                  (const void *)text,
                  (void *)mode);
        return MEDIA_ERR_INVALID_PARAM;
    }

    if (std::strcmp(text, "normal") == 0) {
        *mode = AUDIO_DECODER_TEST_MODE_NORMAL;
        return MEDIA_OK;
    }
    if (std::strcmp(text, "fec") == 0) {
        *mode = AUDIO_DECODER_TEST_MODE_FEC;
        return MEDIA_OK;
    }
    if (std::strcmp(text, "plc") == 0) {
        *mode = AUDIO_DECODER_TEST_MODE_PLC;
        return MEDIA_OK;
    }

    LOG_ERROR("audio decoder test parse mode failed: mode=%s expected normal/fec/plc",
              text);
    return MEDIA_ERR_INVALID_CONFIG;
}

/**
 * @description: 解析丢包间隔；PLC 使用固定间隔，FEC 将其作为两次恢复的最小间隔。
 */
static MediaResult parse_loss_interval(const char *text, int *loss_interval)
{
    char *end = NULL;
    long value = 0;

    if (text == NULL || loss_interval == NULL) {
        LOG_ERROR("audio decoder test parse loss interval failed: text=%p output=%p",
                  (const void *)text,
                  (void *)loss_interval);
        return MEDIA_ERR_INVALID_PARAM;
    }

    value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 2 || value > 10000) {
        LOG_ERROR("audio decoder test parse loss interval failed: interval=%s expected 2..10000",
                  text);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    *loss_interval = static_cast<int>(value);
    return MEDIA_OK;
}

/** @description: 解析测试时长并限制为 1 到 3600 秒。 */
static MediaResult parse_duration_seconds(const char *text, int *duration_seconds)
{
    char *end = NULL;
    long value = 0;

    if (text == NULL || duration_seconds == NULL) {
        LOG_ERROR("audio decoder test parse duration failed: text=%p output=%p",
                  (const void *)text,
                  (void *)duration_seconds);
        return MEDIA_ERR_INVALID_PARAM;
    }

    value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 3600) {
        LOG_ERROR("audio decoder test parse duration failed: duration=%s expected 1..3600",
                  text);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    *duration_seconds = static_cast<int>(value);
    return MEDIA_OK;
}

/** @description: 生成一帧连续相位的单声道 S16_LE 正弦波。 */
static MediaResult fill_mono_sine_frame(int16_t *pcm,
                                        int samples_per_channel,
                                        uint64_t first_sample_index)
{
    double phase = 0.0;
    double normalized_sample = 0.0;
    int sample_index = 0;

    if (pcm == NULL || samples_per_channel <= 0) {
        LOG_ERROR("audio decoder test fill sine failed: pcm=%p samples=%d",
                  (void *)pcm,
                  samples_per_channel);
        return MEDIA_ERR_INVALID_PARAM;
    }

    for (sample_index = 0; sample_index < samples_per_channel; ++sample_index) {
        phase = 2.0 * AUDIO_DECODER_TEST_PI * AUDIO_DECODER_TEST_TONE_HZ *
                static_cast<double>(first_sample_index +
                                    static_cast<uint64_t>(sample_index)) /
                static_cast<double>(AUDIO_DECODER_TEST_SAMPLE_RATE);
        normalized_sample = std::sin(phase) * AUDIO_DECODER_TEST_AMPLITUDE;
        pcm[sample_index] = static_cast<int16_t>(normalized_sample * 32767.0);
    }
    return MEDIA_OK;
}

/**
 * @description: 生成带幅度变化和谐波的语音型测试帧，持续触发 SILK 活动语音判断。
 */
static MediaResult fill_mono_fec_test_frame(int16_t *pcm,
                                            int samples_per_channel,
                                            uint64_t first_sample_index)
{
    double time_seconds = 0.0;
    double envelope = 0.0;
    double normalized_sample = 0.0;
    int sample_index = 0;

    if (pcm == NULL || samples_per_channel <= 0) {
        LOG_ERROR("audio decoder test fill FEC signal failed: pcm=%p samples=%d",
                  (void *)pcm,
                  samples_per_channel);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /*
     * 恒定单频正弦波会在启动阶段后被 SILK 判断为非活动语音，编码器因而停止
     * 生成 LBRR。这里用 130 Hz 基频、两个谐波和 3 Hz 包络模拟持续变化的
     * 浊音，仅用于稳定覆盖 FEC 分支，不用于评价 Opus 的主观音质。
     */
    for (sample_index = 0; sample_index < samples_per_channel; ++sample_index) {
        time_seconds = static_cast<double>(first_sample_index +
                                           static_cast<uint64_t>(sample_index)) /
                       static_cast<double>(AUDIO_DECODER_TEST_SAMPLE_RATE);
        envelope = 0.35 + 0.65 *
                              std::fabs(std::sin(2.0 * AUDIO_DECODER_TEST_PI *
                                                  3.0 * time_seconds));
        normalized_sample = envelope *
                            (std::sin(2.0 * AUDIO_DECODER_TEST_PI *
                                      130.0 * time_seconds) +
                             0.45 * std::sin(2.0 * AUDIO_DECODER_TEST_PI *
                                             260.0 * time_seconds) +
                             0.20 * std::sin(2.0 * AUDIO_DECODER_TEST_PI *
                                             390.0 * time_seconds));
        normalized_sample = normalized_sample / 1.65 * 0.15;
        pcm[sample_index] = static_cast<int16_t>(normalized_sample * 32767.0);
    }
    return MEDIA_OK;
}

/**
 * @description: 将解码 PCM 显式适配为 ALSA Playback 使用的声道布局。
 */
static MediaResult prepare_playback_frame(const AudioDecoderOutput *decoded,
                                          int playback_channels,
                                          std::vector<int16_t> *playback_pcm)
{
    size_t required_samples = 0;
    int frame_index = 0;
    int output_channel = 0;
    int source_channel = 0;

    if (decoded == NULL || playback_pcm == NULL || decoded->data == NULL ||
        decoded->samples_per_channel <= 0 || decoded->channels <= 0 ||
        playback_channels <= 0) {
        LOG_ERROR("audio decoder test prepare playback failed: decoded=%p data=%p "
                  "samples=%d decoded_channels=%d playback_channels=%d output=%p",
                  (const void *)decoded,
                  decoded != NULL ? (const void *)decoded->data : NULL,
                  decoded != NULL ? decoded->samples_per_channel : 0,
                  decoded != NULL ? decoded->channels : 0,
                  playback_channels,
                  (void *)playback_pcm);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (decoded->channels != 1 && decoded->channels != playback_channels) {
        LOG_ERROR("audio decoder test prepare playback failed: cannot map input_channels=%d "
                  "to output_channels=%d",
                  decoded->channels,
                  playback_channels);
        return MEDIA_ERR_UNSUPPORTED;
    }

    required_samples = static_cast<size_t>(decoded->samples_per_channel) *
                       static_cast<size_t>(playback_channels);
    try {
        playback_pcm->resize(required_samples);
    } catch (const std::bad_alloc &) {
        LOG_ERROR("audio decoder test prepare playback failed: allocate samples=%zu",
                  required_samples);
        return MEDIA_ERR_NO_MEMORY;
    }

    /*
     * RK809 数字播放侧要求双声道，而当前语音 Opus 按单声道解码；这里将每个单声道
     * 样本复制到左右声道。若输入输出声道数已相同，则逐声道保持原始布局。
     */
    for (frame_index = 0; frame_index < decoded->samples_per_channel; ++frame_index) {
        for (output_channel = 0; output_channel < playback_channels; ++output_channel) {
            if (decoded->channels == 1) {
                source_channel = 0;
            } else {
                source_channel = output_channel;
            }
            (*playback_pcm)[static_cast<size_t>(frame_index) *
                                static_cast<size_t>(playback_channels) +
                            static_cast<size_t>(output_channel)] =
                decoded->data[static_cast<size_t>(frame_index) *
                                  static_cast<size_t>(decoded->channels) +
                              static_cast<size_t>(source_channel)];
        }
    }
    return MEDIA_OK;
}

/**
 * @description: 校验一帧解码输出，完成播放声道适配并写入 ALSA Playback。
 */
static MediaResult write_decoder_output(const AudioDecoderOutput *decoded,
                                        rkmedia::AudioPlayback *playback,
                                        int playback_channels,
                                        std::vector<int16_t> *playback_pcm,
                                        const char *operation,
                                        uint64_t packet_index)
{
    MediaResult result = MEDIA_OK;

    if (decoded == NULL || playback == NULL || playback_pcm == NULL ||
        operation == NULL) {
        LOG_ERROR("audio decoder test write output failed: decoded=%p playback=%p "
                  "buffer=%p operation=%p packet=%llu",
                  (const void *)decoded,
                  (void *)playback,
                  (void *)playback_pcm,
                  (const void *)operation,
                  (unsigned long long)packet_index);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (decoded->samples_per_channel != AUDIO_DECODER_TEST_FRAME_SAMPLES) {
        LOG_ERROR("audio decoder test write output failed: operation=%s samples=%d "
                  "expected=%d packet=%llu",
                  operation,
                  decoded->samples_per_channel,
                  AUDIO_DECODER_TEST_FRAME_SAMPLES,
                  (unsigned long long)packet_index);
        return MEDIA_ERR;
    }

    result = prepare_playback_frame(decoded, playback_channels, playback_pcm);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio decoder test write output failed: prepare operation=%s "
                  "packet=%llu result=%d",
                  operation,
                  (unsigned long long)packet_index,
                  (int)result);
        return result;
    }

    result = playback->write(playback_pcm->data(),
                             static_cast<size_t>(decoded->samples_per_channel));
    if (result != MEDIA_OK) {
        LOG_ERROR("audio decoder test write output failed: playback operation=%s "
                  "packet=%llu result=%d",
                  operation,
                  (unsigned long long)packet_index,
                  (int)result);
        return result;
    }
    return MEDIA_OK;
}

/** @brief 命令行解析后的测试配置。 */
struct AudioDecoderTestOptions {
    const char *device_name;       /* ALSA Playback 设备名称。 */
    int duration_seconds;          /* 测试持续时间，单位秒。 */
    int loss_interval;             /* PLC 丢包间隔，或 FEC 两次恢复间的最小包数。 */
    AudioDecoderTestMode mode;     /* 普通解码、FEC 或 PLC 测试模式。 */
};

/** @brief 一个位于测试流水线中的裸 Opus 包。 */
struct AudioDecoderTestPacket {
    std::vector<uint8_t> data; /* 存放裸 Opus 包的固定容量缓冲。 */
    size_t size;               /* 当前包的有效字节数；0 表示没有暂存包。 */
    uint64_t index;            /* 从 0 开始的包序号。 */
    uint64_t pts_us;           /* 该包对应 PCM 帧的起始时间戳。 */

    AudioDecoderTestPacket()
        : data(), size(0), index(0), pts_us(0)
    {
    }
};

/**
 * @description: 解析音频解码测试程序的全部命令行参数。
 */
static MediaResult parse_test_options(int argc,
                                      char **argv,
                                      AudioDecoderTestOptions *options)
{
    MediaResult result = MEDIA_OK;

    if (argv == NULL || options == NULL || argc <= 0) {
        LOG_ERROR("audio decoder test parse options failed: argc=%d argv=%p options=%p",
                  argc,
                  (void *)argv,
                  (void *)options);
        return MEDIA_ERR_INVALID_PARAM;
    }

    options->device_name = AUDIO_PLAYBACK_DEFAULT_DEVICE;
    options->duration_seconds = 10;
    options->loss_interval = AUDIO_DECODER_TEST_DEFAULT_LOSS_INTERVAL;
    options->mode = AUDIO_DECODER_TEST_MODE_NORMAL;

    if (argc > 5) {
        LOG_ERROR("audio decoder test parse options failed: usage: %s [device] "
                  "[duration] [normal|fec|plc] [loss_interval]",
                  argv[0]);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (argc > 1) {
        options->device_name = argv[1];
    }
    if (argc > 2) {
        result = parse_duration_seconds(argv[2], &options->duration_seconds);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test parse options failed: duration result=%d",
                      (int)result);
            return result;
        }
    }
    if (argc > 3) {
        result = parse_test_mode(argv[3], &options->mode);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test parse options failed: mode result=%d",
                      (int)result);
            return result;
        }
    }
    if (argc > 4) {
        if (options->mode == AUDIO_DECODER_TEST_MODE_NORMAL) {
            LOG_ERROR("audio decoder test parse options failed: loss_interval is only "
                      "valid in fec/plc mode");
            return MEDIA_ERR_INVALID_CONFIG;
        }
        result = parse_loss_interval(argv[4], &options->loss_interval);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test parse options failed: loss interval result=%d",
                      (int)result);
            return result;
        }
    }
    return MEDIA_OK;
}

/**
 * @description: 管理一次完整的 Opus 解码与 ALSA Playback 测试。
 *
 * 该类把测试资源和运行状态集中在一起，对外只暴露 run()。普通解码/PLC 与 FEC
 * 分别由独立流水线处理，避免 main() 同时维护大量局部变量和交叉状态。
 */
class AudioDecoderTestRunner {
public:
    explicit AudioDecoderTestRunner(const AudioDecoderTestOptions &options)
        : options_(options),
          decoder_(NULL),
          packet_encoder_(NULL),
          playback_(),
          playback_initialized_(false),
          actual_playback_config_(),
          source_pcm_(),
          playback_pcm_(),
          current_packet_(),
          buffered_packet_(),
          total_samples_(0),
          generated_samples_(0),
          encoded_packets_(0),
          simulated_losses_(0),
          last_fec_loss_packet_index_(0)
    {
        std::memset(&actual_playback_config_, 0, sizeof(actual_playback_config_));
    }

    ~AudioDecoderTestRunner()
    {
        (void)release();
    }

    /** @description: 初始化测试链路、执行指定模式并校验最终统计。 */
    MediaResult run()
    {
        MediaResult result = MEDIA_OK;

        result = initialize();
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test run failed: initialize result=%d", (int)result);
            return result;
        }

        logStart();
        if (options_.mode == AUDIO_DECODER_TEST_MODE_FEC) {
            result = runFecPipeline();
        } else {
            result = runNormalOrPlcPipeline();
        }
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test run failed: pipeline mode=%s result=%d",
                      modeName(),
                      (int)result);
            return result;
        }

        result = finishAndValidate();
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test run failed: validate mode=%s result=%d",
                      modeName(),
                      (int)result);
            return result;
        }
        result = release();
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test run failed: release result=%d", (int)result);
            return result;
        }
        return MEDIA_OK;
    }

private:
    AudioDecoderTestOptions options_;
    AudioDecoderHandle *decoder_;
    OpusEncoder *packet_encoder_;
    rkmedia::AudioPlayback playback_;
    bool playback_initialized_;
    rkmedia::AudioPlaybackConfig actual_playback_config_;
    std::vector<int16_t> source_pcm_;
    std::vector<int16_t> playback_pcm_;
    AudioDecoderTestPacket current_packet_;
    AudioDecoderTestPacket buffered_packet_;
    uint64_t total_samples_;
    uint64_t generated_samples_;
    uint64_t encoded_packets_;
    uint64_t simulated_losses_;
    uint64_t last_fec_loss_packet_index_;

    AudioDecoderTestRunner(const AudioDecoderTestRunner &) = delete;
    AudioDecoderTestRunner &operator=(const AudioDecoderTestRunner &) = delete;

    /** @description: 返回当前测试模式的日志名称。 */
    const char *modeName() const
    {
        return audio_decoder_test_mode_name(options_.mode);
    }

    /** @description: 按“缓冲、包生成器、解码器、播放设备”的顺序初始化测试链路。 */
    MediaResult initialize()
    {
        MediaResult result = MEDIA_OK;

        total_samples_ = static_cast<uint64_t>(options_.duration_seconds) *
                         static_cast<uint64_t>(AUDIO_DECODER_TEST_SAMPLE_RATE);
        if (options_.mode != AUDIO_DECODER_TEST_MODE_NORMAL &&
            total_samples_ / AUDIO_DECODER_TEST_FRAME_SAMPLES <=
                static_cast<uint64_t>(options_.loss_interval)) {
            LOG_ERROR("audio decoder test initialize failed: mode=%s duration=%d "
                      "produces too few packets for loss_interval=%d",
                      modeName(),
                      options_.duration_seconds,
                      options_.loss_interval);
            return MEDIA_ERR_INVALID_CONFIG;
        }

        result = allocateBuffers();
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test initialize failed: allocate buffers result=%d",
                      (int)result);
            return result;
        }
        result = createPacketEncoder();
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test initialize failed: packet encoder result=%d",
                      (int)result);
            return result;
        }
        result = createDecoder();
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test initialize failed: decoder result=%d",
                      (int)result);
            return result;
        }
        result = initializePlayback();
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test initialize failed: playback result=%d",
                      (int)result);
            return result;
        }
        return MEDIA_OK;
    }

    /** @description: 分配测试信号、Opus 包和播放声道转换所需缓冲。 */
    MediaResult allocateBuffers()
    {
        try {
            source_pcm_.resize(AUDIO_DECODER_TEST_FRAME_SAMPLES);
            current_packet_.data.resize(AUDIO_DECODER_TEST_MAX_PACKET_BYTES);
            buffered_packet_.data.resize(AUDIO_DECODER_TEST_MAX_PACKET_BYTES);
        } catch (const std::bad_alloc &) {
            LOG_ERROR("audio decoder test allocate buffers failed");
            return MEDIA_ERR_NO_MEMORY;
        }
        return MEDIA_OK;
    }

    /** @description: 创建只用于在内存中产生确定输入包的 libopus 编码器。 */
    MediaResult createPacketEncoder()
    {
        int opus_error = OPUS_OK;

        packet_encoder_ = opus_encoder_create(AUDIO_DECODER_TEST_SAMPLE_RATE,
                                              AUDIO_DECODER_TEST_OPUS_CHANNELS,
                                              OPUS_APPLICATION_VOIP,
                                              &opus_error);
        if (packet_encoder_ == NULL || opus_error != OPUS_OK) {
            LOG_ERROR("audio decoder test create packet encoder failed: err=%s",
                      opus_strerror(opus_error));
            return MEDIA_ERR;
        }

        opus_error = opus_encoder_ctl(packet_encoder_,
                                      OPUS_SET_BITRATE(AUDIO_DECODER_TEST_BITRATE));
        if (opus_error != OPUS_OK) {
            LOG_ERROR("audio decoder test set packet encoder bitrate failed: err=%s",
                      opus_strerror(opus_error));
            return MEDIA_ERR;
        }
        if (options_.mode != AUDIO_DECODER_TEST_MODE_FEC) {
            return MEDIA_OK;
        }

        /*
         * FEC 模式提示输入为语音并声明预期丢包率，使编码器可以生成 SILK LBRR。
         * 这些设置只允许产生带内冗余，具体哪些包携带 LBRR 仍由 libopus 决定。
         */
        opus_error = opus_encoder_ctl(packet_encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        if (opus_error != OPUS_OK) {
            LOG_ERROR("audio decoder test set FEC signal failed: err=%s",
                      opus_strerror(opus_error));
            return MEDIA_ERR;
        }
        opus_error = opus_encoder_ctl(packet_encoder_, OPUS_SET_INBAND_FEC(1));
        if (opus_error != OPUS_OK) {
            LOG_ERROR("audio decoder test enable FEC failed: err=%s",
                      opus_strerror(opus_error));
            return MEDIA_ERR;
        }
        opus_error = opus_encoder_ctl(
            packet_encoder_,
            OPUS_SET_PACKET_LOSS_PERC(AUDIO_DECODER_TEST_FEC_LOSS_PERCENT));
        if (opus_error != OPUS_OK) {
            LOG_ERROR("audio decoder test set FEC loss percent=%d failed: err=%s",
                      AUDIO_DECODER_TEST_FEC_LOSS_PERCENT,
                      opus_strerror(opus_error));
            return MEDIA_ERR;
        }
        return MEDIA_OK;
    }

    /** @description: 创建被测 AudioDecoder 实例。 */
    MediaResult createDecoder()
    {
        AudioDecoderConfig config;
        MediaResult result = MEDIA_OK;

        std::memset(&config, 0, sizeof(config));
        config.codec = MEDIA_CODEC_OPUS;
        config.sample_rate = AUDIO_DECODER_TEST_SAMPLE_RATE;
        config.output_channels = AUDIO_DECODER_TEST_OPUS_CHANNELS;
        config.max_frame_samples_per_channel = AUDIO_DECODER_TEST_MAX_FRAME_SAMPLES;
        result = audio_decoder_create(&config, &decoder_);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test create decoder failed: result=%d", (int)result);
            return result;
        }
        return MEDIA_OK;
    }

    /** @description: 初始化 ALSA Playback 并确认实际采样率与解码器一致。 */
    MediaResult initializePlayback()
    {
        rkmedia::AudioPlaybackConfig config;
        MediaResult result = MEDIA_OK;

        std::memset(&config, 0, sizeof(config));
        config.device_name = options_.device_name;
        config.sample_rate = AUDIO_PLAYBACK_DEFAULT_SAMPLE_RATE;
        config.channels = AUDIO_PLAYBACK_DEFAULT_CHANNELS;
        config.format = rkmedia::AUDIO_PLAYBACK_SAMPLE_FORMAT_S16_LE;
        config.period_frames = AUDIO_PLAYBACK_DEFAULT_PERIOD_FRAMES;
        config.buffer_periods = AUDIO_PLAYBACK_DEFAULT_BUFFER_PERIODS;
        config.start_threshold_periods = AUDIO_PLAYBACK_DEFAULT_START_THRESHOLD_PERIODS;

        result = playback_.init(&config);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test initialize playback failed: device=%s result=%d",
                      options_.device_name,
                      (int)result);
            return result;
        }
        playback_initialized_ = true;

        result = playback_.getConfig(&actual_playback_config_);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test get playback config failed: result=%d",
                      (int)result);
            return result;
        }
        if (actual_playback_config_.sample_rate != AUDIO_DECODER_TEST_SAMPLE_RATE) {
            LOG_ERROR("audio decoder test playback rate mismatch: actual=%d expected=%d",
                      actual_playback_config_.sample_rate,
                      AUDIO_DECODER_TEST_SAMPLE_RATE);
            return MEDIA_ERR_INVALID_CONFIG;
        }
        return MEDIA_OK;
    }

    /** @description: 生成当前时间线上的一帧 PCM，并编码为一个裸 Opus 包。 */
    MediaResult generateCurrentPacket()
    {
        MediaResult result = MEDIA_OK;
        int encoded_bytes = 0;

        if (options_.mode == AUDIO_DECODER_TEST_MODE_FEC) {
            result = fill_mono_fec_test_frame(source_pcm_.data(),
                                              AUDIO_DECODER_TEST_FRAME_SAMPLES,
                                              generated_samples_);
        } else {
            result = fill_mono_sine_frame(source_pcm_.data(),
                                          AUDIO_DECODER_TEST_FRAME_SAMPLES,
                                          generated_samples_);
        }
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test generate packet failed: PCM offset=%llu result=%d",
                      (unsigned long long)generated_samples_,
                      (int)result);
            return result;
        }

        encoded_bytes = opus_encode(
            packet_encoder_,
            reinterpret_cast<const opus_int16 *>(source_pcm_.data()),
            AUDIO_DECODER_TEST_FRAME_SAMPLES,
            current_packet_.data.data(),
            static_cast<opus_int32>(current_packet_.data.size()));
        if (encoded_bytes < 0) {
            LOG_ERROR("audio decoder test generate packet failed: packet=%llu err=%s",
                      (unsigned long long)encoded_packets_,
                      opus_strerror(encoded_bytes));
            return MEDIA_ERR;
        }

        current_packet_.size = static_cast<size_t>(encoded_bytes);
        current_packet_.index = encoded_packets_;
        current_packet_.pts_us = generated_samples_ * 1000000ULL /
                                 static_cast<uint64_t>(AUDIO_DECODER_TEST_SAMPLE_RATE);
        return MEDIA_OK;
    }

    /** @description: 普通解码一个裸 Opus 包，并立即将 PCM 写入 Playback。 */
    MediaResult decodeAndPlay(const AudioDecoderTestPacket &packet,
                              const char *operation)
    {
        AudioDecoderInput input;
        AudioDecoderOutput output;
        MediaResult result = MEDIA_OK;

        std::memset(&input, 0, sizeof(input));
        std::memset(&output, 0, sizeof(output));
        input.data = packet.data.data();
        input.size = packet.size;
        input.pts_us = packet.pts_us;

        result = audio_decoder_decode(decoder_, &input, &output);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test decode failed: operation=%s packet=%llu "
                      "size=%zu result=%d",
                      operation,
                      (unsigned long long)packet.index,
                      packet.size,
                      (int)result);
            return result;
        }
        result = write_decoder_output(&output,
                                      &playback_,
                                      actual_playback_config_.channels,
                                      &playback_pcm_,
                                      operation,
                                      packet.index);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test decode failed: write operation=%s packet=%llu "
                      "result=%d",
                      operation,
                      (unsigned long long)packet.index,
                      (int)result);
            return result;
        }
        return MEDIA_OK;
    }

    /** @description: 恢复一帧丢失 PCM，并立即写入 Playback。 */
    MediaResult recoverAndPlay(const AudioDecoderTestPacket *following_packet,
                               uint64_t lost_packet_index,
                               uint64_t lost_packet_pts_us,
                               const char *operation)
    {
        AudioDecoderLossInput input;
        AudioDecoderOutput output;
        MediaResult result = MEDIA_OK;

        std::memset(&input, 0, sizeof(input));
        std::memset(&output, 0, sizeof(output));
        if (following_packet != NULL) {
            input.following_packet_data = following_packet->data.data();
            input.following_packet_size = following_packet->size;
        }
        input.lost_samples_per_channel = AUDIO_DECODER_TEST_FRAME_SAMPLES;
        input.pts_us = lost_packet_pts_us;

        result = audio_decoder_recover_loss(decoder_, &input, &output);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test recover failed: operation=%s lost_packet=%llu "
                      "following_packet=%llu result=%d",
                      operation,
                      (unsigned long long)lost_packet_index,
                      (unsigned long long)(following_packet != NULL
                                               ? following_packet->index
                                               : 0),
                      (int)result);
            return result;
        }
        result = write_decoder_output(&output,
                                      &playback_,
                                      actual_playback_config_.channels,
                                      &playback_pcm_,
                                      operation,
                                      lost_packet_index);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test recover failed: write operation=%s packet=%llu "
                      "result=%d",
                      operation,
                      (unsigned long long)lost_packet_index,
                      (int)result);
            return result;
        }
        return MEDIA_OK;
    }

    /** @description: 将当前 Opus 包复制到 FEC 单包延迟缓冲中。 */
    MediaResult bufferCurrentPacket()
    {
        if (current_packet_.size > buffered_packet_.data.size()) {
            LOG_ERROR("audio decoder test buffer packet failed: size=%zu capacity=%zu",
                      current_packet_.size,
                      buffered_packet_.data.size());
            return MEDIA_ERR_INVALID_PARAM;
        }

        std::memcpy(buffered_packet_.data.data(),
                    current_packet_.data.data(),
                    current_packet_.size);
        buffered_packet_.size = current_packet_.size;
        buffered_packet_.index = current_packet_.index;
        buffered_packet_.pts_us = current_packet_.pts_us;
        return MEDIA_OK;
    }

    /** @description: 推进一帧测试输入的采样位置和包序号。 */
    void advanceInputTimeline()
    {
        generated_samples_ += AUDIO_DECODER_TEST_FRAME_SAMPLES;
        ++encoded_packets_;
    }

    /**
     * @description: 执行普通解码或固定周期 PLC 测试流水线。
     *
     * normal 模式逐包解码。PLC 模式周期性跳过一个包，并在下一包到来前请求
     * 解码器在没有后续冗余包的条件下补齐丢失帧，然后继续解码当前包。
     */
    MediaResult runNormalOrPlcPipeline()
    {
        MediaResult result = MEDIA_OK;
        uint64_t pending_loss_index = 0;
        uint64_t pending_loss_pts_us = 0;
        bool pending_loss = false;
        bool should_drop_current = false;

        while (generated_samples_ < total_samples_) {
            result = generateCurrentPacket();
            if (result != MEDIA_OK) {
                LOG_ERROR("audio decoder test normal/PLC pipeline failed: generate result=%d",
                          (int)result);
                return result;
            }

            /* 上一轮留下缺口时，先按时间顺序输出 PLC 帧，再处理当前真实包。 */
            if (pending_loss) {
                result = recoverAndPlay(NULL,
                                        pending_loss_index,
                                        pending_loss_pts_us,
                                        "plc_recover");
                if (result != MEDIA_OK) {
                    LOG_ERROR("audio decoder test PLC pipeline failed: recover packet=%llu "
                              "result=%d",
                              (unsigned long long)pending_loss_index,
                              (int)result);
                    return result;
                }
                pending_loss = false;
            }

            /* 最后一包不模拟丢失，保证测试结束时不会留下待恢复缺口。 */
            should_drop_current =
                options_.mode == AUDIO_DECODER_TEST_MODE_PLC &&
                ((encoded_packets_ + 1U) % static_cast<uint64_t>(options_.loss_interval) == 0U) &&
                generated_samples_ + AUDIO_DECODER_TEST_FRAME_SAMPLES < total_samples_;
            if (should_drop_current) {
                pending_loss = true;
                pending_loss_index = current_packet_.index;
                pending_loss_pts_us = current_packet_.pts_us;
                ++simulated_losses_;
                advanceInputTimeline();
                continue;
            }

            result = decodeAndPlay(current_packet_, "decode");
            if (result != MEDIA_OK) {
                LOG_ERROR("audio decoder test normal/PLC pipeline failed: decode packet=%llu "
                          "result=%d",
                          (unsigned long long)current_packet_.index,
                          (int)result);
                return result;
            }
            advanceInputTimeline();
        }

        if (pending_loss) {
            LOG_ERROR("audio decoder test PLC pipeline failed: pending loss packet=%llu",
                      (unsigned long long)pending_loss_index);
            return MEDIA_ERR;
        }
        return MEDIA_OK;
    }

    /**
     * @description: 处理 FEC 流水线中的当前包和暂存前一包。
     *
     * Opus 的前一帧冗余位于后一包中，因此必须将前一包延迟一轮。只有当前包实际
     * 携带 LBRR 且达到模拟丢包间隔时，才丢弃前一包并用当前包恢复；否则正常解码
     * 前一包，并将当前包接替为新的暂存包。
     */
    MediaResult processFecPacket()
    {
        MediaResult result = MEDIA_OK;
        int has_inband_redundancy = 0;
        bool should_recover_buffered = false;

        if (buffered_packet_.size == 0) {
            result = bufferCurrentPacket();
            if (result != MEDIA_OK) {
                LOG_ERROR("audio decoder test FEC packet failed: initial buffer result=%d",
                          (int)result);
            }
            return result;
        }

        has_inband_redundancy = opus_packet_has_lbrr(
            current_packet_.data.data(),
            static_cast<opus_int32>(current_packet_.size));
        if (has_inband_redundancy < 0) {
            LOG_ERROR("audio decoder test FEC packet failed: inspect packet=%llu size=%zu "
                      "err=%s",
                      (unsigned long long)current_packet_.index,
                      current_packet_.size,
                      opus_strerror(has_inband_redundancy));
            return MEDIA_ERR;
        }

        should_recover_buffered = has_inband_redundancy > 0 &&
            buffered_packet_.index + 1U >= static_cast<uint64_t>(options_.loss_interval) &&
            (simulated_losses_ == 0 || buffered_packet_.index - last_fec_loss_packet_index_ >= static_cast<uint64_t>(options_.loss_interval));
        if (should_recover_buffered) {
            result = recoverAndPlay(&current_packet_,
                                    buffered_packet_.index,
                                    buffered_packet_.pts_us,
                                    "fec_recover");
            if (result != MEDIA_OK) {
                LOG_ERROR("audio decoder test FEC packet failed: recover packet=%llu "
                          "result=%d",
                          (unsigned long long)buffered_packet_.index,
                          (int)result);
                return result;
            }
            last_fec_loss_packet_index_ = buffered_packet_.index;
            buffered_packet_.size = 0;
            ++simulated_losses_;

            /* 恢复前一帧不会消费当前包，当前包还必须执行一次普通解码。 */
            result = decodeAndPlay(current_packet_, "decode_after_fec");
            if (result != MEDIA_OK) {
                LOG_ERROR("audio decoder test FEC packet failed: decode following packet=%llu "
                          "result=%d",
                          (unsigned long long)current_packet_.index,
                          (int)result);
                return result;
            }
            return MEDIA_OK;
        }

        result = decodeAndPlay(buffered_packet_, "decode_buffered");
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test FEC packet failed: decode buffered packet=%llu "
                      "result=%d",
                      (unsigned long long)buffered_packet_.index,
                      (int)result);
            return result;
        }
        result = bufferCurrentPacket();
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test FEC packet failed: replace buffer result=%d",
                      (int)result);
            return result;
        }
        return MEDIA_OK;
    }

    /** @description: 执行需要单包延迟的 Opus 带内 FEC 测试流水线。 */
    MediaResult runFecPipeline()
    {
        MediaResult result = MEDIA_OK;

        while (generated_samples_ < total_samples_) {
            result = generateCurrentPacket();
            if (result != MEDIA_OK) {
                LOG_ERROR("audio decoder test FEC pipeline failed: generate result=%d",
                          (int)result);
                return result;
            }
            result = processFecPacket();
            if (result != MEDIA_OK) {
                LOG_ERROR("audio decoder test FEC pipeline failed: process packet=%llu "
                          "result=%d",
                          (unsigned long long)current_packet_.index,
                          (int)result);
                return result;
            }
            advanceInputTimeline();
        }

        /* 流水线末尾没有后续包，最后一个暂存包只能按普通方式解码。 */
        if (buffered_packet_.size > 0) {
            result = decodeAndPlay(buffered_packet_, "decode_final_buffered");
            if (result != MEDIA_OK) {
                LOG_ERROR("audio decoder test FEC pipeline failed: flush packet=%llu "
                          "result=%d",
                          (unsigned long long)buffered_packet_.index,
                          (int)result);
                return result;
            }
            buffered_packet_.size = 0;
        }
        return MEDIA_OK;
    }

    /** @description: 校验公共统计和各模式专属的恢复分支统计。 */
    MediaResult validateStats(const AudioDecoderStats &decoder_stats,
                              const rkmedia::AudioPlaybackStats &playback_stats) const
    {
        uint64_t expected_decoded_packets = 0;

        expected_decoded_packets = encoded_packets_ - simulated_losses_;
        if (decoder_stats.decoded_packets != expected_decoded_packets ||
            decoder_stats.decoded_samples != total_samples_ ||
            decoder_stats.decode_errors != 0 ||
            playback_stats.written_frames != total_samples_ ||
            playback_stats.xrun_count != 0) {
            LOG_ERROR("audio decoder test stats invalid: mode=%s packets=%llu losses=%llu "
                      "decoded_packets=%llu expected_packets=%llu decoded_samples=%llu "
                      "expected_samples=%llu decode_errors=%llu written_frames=%llu "
                      "playback_xruns=%llu",
                      modeName(),
                      (unsigned long long)encoded_packets_,
                      (unsigned long long)simulated_losses_,
                      (unsigned long long)decoder_stats.decoded_packets,
                      (unsigned long long)expected_decoded_packets,
                      (unsigned long long)decoder_stats.decoded_samples,
                      (unsigned long long)total_samples_,
                      (unsigned long long)decoder_stats.decode_errors,
                      (unsigned long long)playback_stats.written_frames,
                      (unsigned long long)playback_stats.xrun_count);
            return MEDIA_ERR;
        }

        if (options_.mode == AUDIO_DECODER_TEST_MODE_NORMAL &&
            (simulated_losses_ != 0 || decoder_stats.inband_recovered_frames != 0 ||
             decoder_stats.concealed_frames != 0)) {
            LOG_ERROR("audio decoder test normal stats invalid: losses=%llu inband=%llu "
                      "concealed=%llu",
                      (unsigned long long)simulated_losses_,
                      (unsigned long long)decoder_stats.inband_recovered_frames,
                      (unsigned long long)decoder_stats.concealed_frames);
            return MEDIA_ERR;
        }
        if (options_.mode == AUDIO_DECODER_TEST_MODE_FEC &&
            (simulated_losses_ == 0 ||
             decoder_stats.inband_recovered_frames != simulated_losses_ ||
             decoder_stats.concealed_frames != 0)) {
            LOG_ERROR("audio decoder test FEC stats invalid: losses=%llu inband=%llu "
                      "concealed=%llu",
                      (unsigned long long)simulated_losses_,
                      (unsigned long long)decoder_stats.inband_recovered_frames,
                      (unsigned long long)decoder_stats.concealed_frames);
            return MEDIA_ERR;
        }
        if (options_.mode == AUDIO_DECODER_TEST_MODE_PLC &&
            (simulated_losses_ == 0 || decoder_stats.inband_recovered_frames != 0 ||
             decoder_stats.concealed_frames != simulated_losses_)) {
            LOG_ERROR("audio decoder test PLC stats invalid: losses=%llu inband=%llu "
                      "concealed=%llu",
                      (unsigned long long)simulated_losses_,
                      (unsigned long long)decoder_stats.inband_recovered_frames,
                      (unsigned long long)decoder_stats.concealed_frames);
            return MEDIA_ERR;
        }
        return MEDIA_OK;
    }

    /** @description: 排空声卡、读取统计、执行断言并打印测试完成摘要。 */
    MediaResult finishAndValidate()
    {
        AudioDecoderStats decoder_stats;
        rkmedia::AudioPlaybackStats playback_stats;
        MediaResult result = MEDIA_OK;

        std::memset(&decoder_stats, 0, sizeof(decoder_stats));
        std::memset(&playback_stats, 0, sizeof(playback_stats));
        result = playback_.drain();
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test finish failed: playback drain result=%d",
                      (int)result);
            return result;
        }
        result = audio_decoder_get_stats(decoder_, &decoder_stats);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test finish failed: decoder stats result=%d",
                      (int)result);
            return result;
        }
        result = playback_.getStats(&playback_stats);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test finish failed: playback stats result=%d",
                      (int)result);
            return result;
        }
        result = validateStats(decoder_stats, playback_stats);
        if (result != MEDIA_OK) {
            LOG_ERROR("audio decoder test finish failed: validate stats result=%d",
                      (int)result);
            return result;
        }

        LOG_INFO("audio decoder test complete: mode=%s packets=%llu simulated_losses=%llu "
                 "decoded_packets=%llu decoded_samples=%llu inband_recovered=%llu "
                 "concealed=%llu decode_errors=%llu written_frames=%llu playback_xruns=%llu",
                 modeName(),
                 (unsigned long long)encoded_packets_,
                 (unsigned long long)simulated_losses_,
                 (unsigned long long)decoder_stats.decoded_packets,
                 (unsigned long long)decoder_stats.decoded_samples,
                 (unsigned long long)decoder_stats.inband_recovered_frames,
                 (unsigned long long)decoder_stats.concealed_frames,
                 (unsigned long long)decoder_stats.decode_errors,
                 (unsigned long long)playback_stats.written_frames,
                 (unsigned long long)playback_stats.xrun_count);
        return MEDIA_OK;
    }

    /** @description: 打印测试配置摘要，便于核对实际 Playback 参数。 */
    void logStart() const
    {
        LOG_INFO("audio decoder test start: device=%s duration=%d mode=%s "
                 "loss_interval=%d rate=%d opus_channels=%d playback_channels=%d "
                 "frame_samples=%d total_samples=%llu",
                 actual_playback_config_.device_name,
                 options_.duration_seconds,
                 modeName(),
                 options_.loss_interval,
                 AUDIO_DECODER_TEST_SAMPLE_RATE,
                 AUDIO_DECODER_TEST_OPUS_CHANNELS,
                 actual_playback_config_.channels,
                 AUDIO_DECODER_TEST_FRAME_SAMPLES,
                 (unsigned long long)total_samples_);
    }

    /** @description: 释放已经初始化的 Playback、解码器和测试包生成器。 */
    MediaResult release()
    {
        MediaResult result = MEDIA_OK;
        MediaResult release_result = MEDIA_OK;

        if (playback_initialized_) {
            release_result = playback_.deinit();
            if (release_result != MEDIA_OK) {
                LOG_ERROR("audio decoder test release failed: playback result=%d",
                          (int)release_result);
                result = release_result;
            }
            playback_initialized_ = false;
        }
        if (decoder_ != NULL) {
            audio_decoder_destroy(decoder_);
            decoder_ = NULL;
        }
        if (packet_encoder_ != NULL) {
            opus_encoder_destroy(packet_encoder_);
            packet_encoder_ = NULL;
        }
        return result;
    }
};

/** @description: 创建测试运行对象并执行完整测试。 */
static MediaResult run_audio_decoder_test(const AudioDecoderTestOptions &options)
{
    AudioDecoderTestRunner runner(options);
    MediaResult result = MEDIA_OK;

    result = runner.run();
    if (result != MEDIA_OK) {
        LOG_ERROR("audio decoder test execute failed: mode=%s result=%d",
                  audio_decoder_test_mode_name(options.mode),
                  (int)result);
        return result;
    }
    return MEDIA_OK;
}

/**
 * @description: Opus 解码与 ALSA Playback 独立测试入口。
 *
 * 测试程序用 libopus 在内存中生成可控的裸 Opus 包，再通过 AudioDecoder 解码，
 * 完成单声道到双声道适配后写入 AudioPlayback，不依赖网络和 WebRTC 信令。
 */
int main(int argc, char **argv)
{
    AudioDecoderTestOptions options;
    MediaResult result = MEDIA_OK;

    log_set_immediate_flush(1);
    std::memset(&options, 0, sizeof(options));
    result = parse_test_options(argc, argv, &options);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio decoder test failed: parse options result=%d", (int)result);
        return EXIT_FAILURE;
    }

    result = run_audio_decoder_test(options);
    if (result != MEDIA_OK) {
        LOG_ERROR("audio decoder test failed: run mode=%s result=%d",
                  audio_decoder_test_mode_name(options.mode),
                  (int)result);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
