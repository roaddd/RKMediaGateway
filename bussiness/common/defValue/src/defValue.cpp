#include "defValue.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <map>
#include <string>

static std::map<std::string, std::string> g_values;
static std::map<std::string, std::string> g_file_values;
static std::string g_source_path;
static int g_loaded = 0;

static std::string trim_copy(const std::string &s) {
    size_t begin = 0;
    size_t end = s.size();

    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

/*
 * 去掉配置值后面的行内注释。
 * 支持：
 *   key = 1 # comment
 * 被单引号或双引号包裹的 # 认为是字符串内容，不当作注释起点。
 */
static std::string strip_inline_comment_copy(const std::string &s) {
    size_t i = 0;
    char quote = '\0';

    for (i = 0; i < s.size(); ++i) {
        if (quote != '\0') {
            if (s[i] == quote) {
                quote = '\0';
            }
            continue;
        }

        if (s[i] == '"' || s[i] == '\'') {
            quote = s[i];
            continue;
        }

        if (s[i] == '#') {
            return trim_copy(s.substr(0, i));
        }
    }

    return trim_copy(s);
}

static void set_value(const char *key, const char *value) {
    g_values[key] = value ? value : "";
}

static void set_value_int(const char *key, int value) {
    g_values[key] = std::to_string(value);
}

static const char *value_string(const char *key) {
    std::map<std::string, std::string>::const_iterator it = g_values.find(key);
    if (it == g_values.end()) {
        return "";
    }
    return it->second.c_str();
}

static int value_int(const char *key) {
    std::map<std::string, std::string>::const_iterator it = g_values.find(key);
    char *end_ptr = NULL;
    long parsed = 0;

    if (it == g_values.end() || it->second.empty()) {
        return 0;
    }

    parsed = std::strtol(it->second.c_str(), &end_ptr, 10);
    if (!end_ptr || *end_ptr != '\0') {
        return 0;
    }
    return (int)parsed;
}

static float value_float(const char *key) {
    std::map<std::string, std::string>::const_iterator it = g_values.find(key);
    char *end_ptr = NULL;
    float parsed = 0.0f;

    if (it == g_values.end() || it->second.empty()) {
        return 0.0f;
    }

    parsed = std::strtof(it->second.c_str(), &end_ptr);
    if (!end_ptr || *end_ptr != '\0') {
        return 0.0f;
    }
    return parsed;
}

static std::string uppercase_key(std::string value) {
    size_t i;

    for (i = 0; i < value.size(); ++i) {
        if (value[i] == '.' || value[i] == '-') {
            value[i] = '_';
        } else {
            value[i] = (char)std::toupper((unsigned char)value[i]);
        }
    }
    return value;
}

/*
 * 将可读的 TOML 路径映射到现有默认值表使用的内部键名。
 * 内部键名只是一层实现细节，配置文件不再暴露 STREAM_MAIN_* 之类的扁平前缀。
 */
static std::string toml_path_to_internal_key(const std::string &path) {
    static const std::string audio_capture = "audio.capture.";
    static const std::string audio_runtime = "audio.runtime.";
    static const std::string audio_encoder = "audio.encoder.";
    std::string suffix;

    if (path.compare(0, audio_capture.size(), audio_capture) == 0) {
        suffix = path.substr(audio_capture.size());
        if (suffix == "device") suffix = "device";
        if (suffix == "channels") return "AUDIO_CAPTURE_CHANNELS";
        return "AUDIO_" + uppercase_key(suffix);
    }
    if (path.compare(0, audio_runtime.size(), audio_runtime) == 0)
        return "AUDIO_" + uppercase_key(path.substr(audio_runtime.size()));
    if (path.compare(0, audio_encoder.size(), audio_encoder) == 0) {
        suffix = path.substr(audio_encoder.size());
        if (suffix == "channels") return "AUDIO_ENCODER_CHANNELS";
        if (suffix == "input_channel") return "AUDIO_ENCODER_INPUT_CHANNEL";
        return "AUDIO_" + uppercase_key(path.substr(audio_encoder.size()));
    }
    return uppercase_key(path);
}

/* 解析 TOML 基本字符串，并处理配置中常用的转义字符。 */
static int parse_toml_string(const std::string &input, std::string &output) {
    size_t i;
    char quote;

    if (input.size() < 2 || (input[0] != '"' && input[0] != '\''))
        return -1;
    quote = input[0];
    if (input[input.size() - 1] != quote)
        return -1;
    output.clear();
    for (i = 1; i + 1 < input.size(); ++i) {
        if (quote == '"' && input[i] == '\\') {
            if (++i + 1 >= input.size()) return -1;
            switch (input[i]) {
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            default: return -1;
            }
        } else {
            output.push_back(input[i]);
        }
    }
    return 0;
}

/*
 * 加载 TOML 文件。当前配置只需要 TOML 的 table、字符串、布尔值、整数和浮点数；
 * 数组/日期等未使用类型会被明确拒绝，避免静默采用错误配置。
 */
static int load_toml_values(const char *config_path) {
    std::ifstream file;
    std::string line;
    std::string section;
    int line_number = 0;

    g_file_values.clear();
    g_source_path = config_path ? config_path : "";
    g_loaded = 0;

    if (!config_path || config_path[0] == '\0') {
        std::fprintf(stderr, "TOML config path is empty\n");
        return -1;
    }

    file.open(config_path);
    if (!file.is_open()) {
        std::fprintf(stderr, "failed to open TOML config: %s\n", config_path);
        return -1;
    }

    while (std::getline(file, line)) {
        ++line_number;
        /* UTF-8 BOM is permitted at the beginning of configuration files. */
        if (line_number == 1 && line.size() >= 3 &&
            (unsigned char)line[0] == 0xef &&
            (unsigned char)line[1] == 0xbb &&
            (unsigned char)line[2] == 0xbf) {
            line.erase(0, 3);
        }
        std::string trimmed = trim_copy(line);
        std::string key;
        std::string value;
        std::string path;
        std::string parsed_value;
        std::string::size_type eq_pos;

        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        if (trimmed[0] == '[') {
            if (trimmed.size() < 3 || trimmed[trimmed.size() - 1] != ']' ||
                trimmed[1] == '[' || trimmed[trimmed.size() - 2] == ']') {
                std::fprintf(stderr, "%s:%d: invalid or unsupported TOML table\n", config_path, line_number);
                return -1;
            }
            section = trim_copy(trimmed.substr(1, trimmed.size() - 2));
            if (section.empty()) {
                std::fprintf(stderr, "%s:%d: empty TOML table\n", config_path, line_number);
                return -1;
            }
            continue;
        }

        eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) {
            std::fprintf(stderr, "%s:%d: expected key = value\n", config_path, line_number);
            return -1;
        }

        key = trim_copy(trimmed.substr(0, eq_pos));
        value = strip_inline_comment_copy(trimmed.substr(eq_pos + 1));
        if (key.empty() || value.empty()) {
            std::fprintf(stderr, "%s:%d: empty TOML key or value\n", config_path, line_number);
            return -1;
        }

        if (value[0] == '"' || value[0] == '\'') {
            if (parse_toml_string(value, parsed_value) != 0) {
                std::fprintf(stderr, "%s:%d: invalid TOML string\n", config_path, line_number);
                return -1;
            }
        } else if (value == "true") {
            parsed_value = "1";
        } else if (value == "false") {
            parsed_value = "0";
        } else {
            char *end_ptr = NULL;
            (void)std::strtod(value.c_str(), &end_ptr);
            if (!end_ptr || *end_ptr != '\0') {
                std::fprintf(stderr, "%s:%d: unsupported TOML value: %s\n",
                             config_path, line_number, value.c_str());
                return -1;
            }
            parsed_value = value;
        }

        path = section.empty() ? key : section + "." + key;
        key = toml_path_to_internal_key(path);
        if (g_values.find(key) == g_values.end()) {
            std::fprintf(stderr, "%s:%d: unknown TOML key: %s\n",
                         config_path, line_number, path.c_str());
            return -1;
        }
        if (g_file_values.find(key) != g_file_values.end()) {
            std::fprintf(stderr, "%s:%d: duplicate TOML key: %s\n",
                         config_path, line_number, path.c_str());
            return -1;
        }
        g_file_values[key] = parsed_value;
        g_values[key] = parsed_value;
    }

    g_loaded = 1;
    return 0;
}

static void set_stream_defaults(const char *prefix,
                                int is_main,
                                int width,
                                int height,
                                int fps,
                                int bitrate) {
    std::string p(prefix);

    set_value((p + "ENABLE").c_str(), is_main ? "1" : "0");
    set_value((p + "NAME").c_str(), is_main ? "main" : "sub");
    set_value_int((p + "SOURCE_INDEX").c_str(), is_main ? 0 : 1);
    set_value_int((p + "WIDTH").c_str(), width);
    set_value_int((p + "HEIGHT").c_str(), height);
    set_value_int((p + "FPS").c_str(), fps);
    set_value_int((p + "BITRATE").c_str(), bitrate);
    set_value_int((p + "GOP").c_str(), 30);
    set_value_int((p + "RC_MODE").c_str(), MPP_ENC_RC_MODE_CBR);
    set_value_int((p + "H264_PROFILE").c_str(), 100);
    set_value_int((p + "H264_LEVEL").c_str(), is_main ? 42 : 40);
    set_value_int((p + "H264_CABAC_EN").c_str(), 1);
    set_value_int((p + "QP_INIT").c_str(), 30);
    set_value_int((p + "QP_MIN").c_str(), 22);
    set_value_int((p + "QP_MAX").c_str(), 44);
    set_value_int((p + "QP_MIN_I").c_str(), 20);
    set_value_int((p + "QP_MAX_I").c_str(), 40);
    set_value_int((p + "QP_MAX_STEP").c_str(), 8);

    /*
     * 为动态帧率准备三组完整编码档位。setFps 运行期会按目标 fps 选择对应
     * profile，而不是只修改编码器 fps。
     */
    set_value_int((p + "DYNAMIC_LOW_FPS").c_str(), 15);
    set_value_int((p + "DYNAMIC_LOW_BITRATE").c_str(), bitrate / 2);
    set_value_int((p + "DYNAMIC_LOW_GOP").c_str(), 15);
    set_value_int((p + "DYNAMIC_LOW_RC_MODE").c_str(), MPP_ENC_RC_MODE_CBR);
    set_value_int((p + "DYNAMIC_LOW_QP_INIT").c_str(), 30);
    set_value_int((p + "DYNAMIC_LOW_QP_MIN").c_str(), 22);
    set_value_int((p + "DYNAMIC_LOW_QP_MAX").c_str(), 44);
    set_value_int((p + "DYNAMIC_LOW_QP_MIN_I").c_str(), 20);
    set_value_int((p + "DYNAMIC_LOW_QP_MAX_I").c_str(), 40);
    set_value_int((p + "DYNAMIC_LOW_QP_MAX_STEP").c_str(), 8);
    set_value_int((p + "DYNAMIC_NORMAL_FPS").c_str(), fps);
    set_value_int((p + "DYNAMIC_NORMAL_BITRATE").c_str(), bitrate);
    set_value_int((p + "DYNAMIC_NORMAL_GOP").c_str(), fps);
    set_value_int((p + "DYNAMIC_NORMAL_RC_MODE").c_str(), MPP_ENC_RC_MODE_CBR);
    set_value_int((p + "DYNAMIC_NORMAL_QP_INIT").c_str(), 30);
    set_value_int((p + "DYNAMIC_NORMAL_QP_MIN").c_str(), 22);
    set_value_int((p + "DYNAMIC_NORMAL_QP_MAX").c_str(), 44);
    set_value_int((p + "DYNAMIC_NORMAL_QP_MIN_I").c_str(), 20);
    set_value_int((p + "DYNAMIC_NORMAL_QP_MAX_I").c_str(), 40);
    set_value_int((p + "DYNAMIC_NORMAL_QP_MAX_STEP").c_str(), 8);
    set_value_int((p + "DYNAMIC_BRIGHT_FPS").c_str(), 60);
    set_value_int((p + "DYNAMIC_BRIGHT_BITRATE").c_str(), bitrate * 2);
    set_value_int((p + "DYNAMIC_BRIGHT_GOP").c_str(), 60);
    set_value_int((p + "DYNAMIC_BRIGHT_RC_MODE").c_str(), MPP_ENC_RC_MODE_CBR);
    set_value_int((p + "DYNAMIC_BRIGHT_QP_INIT").c_str(), 30);
    set_value_int((p + "DYNAMIC_BRIGHT_QP_MIN").c_str(), 22);
    set_value_int((p + "DYNAMIC_BRIGHT_QP_MAX").c_str(), 44);
    set_value_int((p + "DYNAMIC_BRIGHT_QP_MIN_I").c_str(), 20);
    set_value_int((p + "DYNAMIC_BRIGHT_QP_MAX_I").c_str(), 40);
    set_value_int((p + "DYNAMIC_BRIGHT_QP_MAX_STEP").c_str(), 8);

    set_value_int((p + "ENABLE_RTSP").c_str(), 1);
    set_value_int((p + "ENABLE_RTMP").c_str(), 0);
    set_value_int((p + "ENABLE_GB28181").c_str(), is_main ? 1 : 0);
    set_value_int((p + "ENABLE_WEBRTC").c_str(), 0);

    set_value((p + "RTSP_NAME").c_str(), is_main ? "rtsp-main" : "rtsp-sub");
    set_value((p + "RTSP_SESSION_NAME").c_str(), is_main ? "live_main" : "live_sub");
    set_value((p + "RTSP_SERVER_IP").c_str(), "0.0.0.0");
    set_value_int((p + "RTSP_SERVER_PORT").c_str(), 8554);
    set_value_int((p + "RTSP_AUTH_ENABLE").c_str(), 0);
    set_value((p + "RTSP_USER").c_str(), "admin");
    set_value((p + "RTSP_PASSWORD").c_str(), "123456");
    set_value_int((p + "RTSP_QUEUE_CAPACITY").c_str(), 32);
    set_value_int((p + "RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT").c_str(), 0);

    set_value((p + "RTMP_NAME").c_str(), is_main ? "rtmp-main" : "rtmp-sub");
    set_value((p + "RTMP_PUBLISH_URL").c_str(), "");
    set_value_int((p + "RTMP_QUEUE_CAPACITY").c_str(), 64);
    set_value_int((p + "RTMP_RECONNECT_INTERVAL_MS").c_str(), 1000);
    set_value_int((p + "RTMP_CONNECT_TIMEOUT_MS").c_str(), 3000);
    set_value_int((p + "RTMP_AUDIO_ENABLED").c_str(), 0);
    set_value_int((p + "RTMP_VIDEO_WIDTH").c_str(), width);
    set_value_int((p + "RTMP_VIDEO_HEIGHT").c_str(), height);
    set_value_int((p + "RTMP_VIDEO_FPS").c_str(), fps);
    set_value_int((p + "RTMP_VIDEO_BITRATE").c_str(), bitrate);
    set_value((p + "RTMP_VIDEO_CODEC_NAME").c_str(), "H264");
    set_value((p + "RTMP_ENCODER_NAME").c_str(), "RKMediaGateway");

    set_value((p + "GB28181_NAME").c_str(), is_main ? "gb28181-main" : "gb28181-sub");
    set_value((p + "GB28181_SERVER_IP").c_str(), "192.168.1.1");
    set_value_int((p + "GB28181_SERVER_PORT").c_str(), 5060);
    set_value((p + "GB28181_SERVER_DOMAIN").c_str(), "3402000000");
    set_value((p + "GB28181_SERVER_ID").c_str(), "34020000002000000001");
    set_value((p + "GB28181_DEVICE_ID").c_str(), "34020000001320000001");
    set_value((p + "GB28181_DEVICE_DOMAIN").c_str(), "3402000000");
    set_value((p + "GB28181_DEVICE_PASSWORD").c_str(), "12345678");
    set_value((p + "GB28181_BIND_IP").c_str(), "0.0.0.0");
    set_value_int((p + "GB28181_LOCAL_SIP_PORT").c_str(), 5060);
    set_value((p + "GB28181_CONTACT_IP").c_str(), "192.168.1.100");
    set_value((p + "GB28181_MEDIA_IP").c_str(), "192.168.1.100");
    set_value_int((p + "GB28181_MEDIA_PORT").c_str(), 30000);
    set_value_int((p + "GB28181_REGISTER_EXPIRES").c_str(), 3600);
    set_value_int((p + "GB28181_KEEPALIVE_INTERVAL").c_str(), 60);
    set_value_int((p + "GB28181_REGISTER_RETRY_INTERVAL").c_str(), 5);
    set_value((p + "GB28181_DEVICE_NAME").c_str(), "RK3568 Camera");
    set_value((p + "GB28181_MANUFACTURER").c_str(), "Topeet");
    set_value((p + "GB28181_MODEL").c_str(), "RKMediaGateway");
    set_value((p + "GB28181_FIRMWARE").c_str(), "1.0.0");
    set_value((p + "GB28181_CHANNEL_ID").c_str(), "34020000001320000001");
    set_value((p + "GB28181_USER_AGENT").c_str(), "RKMediaGateway-GB28181/1.0");
    set_value_int((p + "GB28181_QUEUE_CAPACITY").c_str(), 64);

    set_value((p + "WEBRTC_NAME").c_str(), is_main ? "webrtc-main" : "webrtc-sub");
    set_value((p + "WEBRTC_BIND_ADDRESS").c_str(), "0.0.0.0");
    set_value_int((p + "WEBRTC_PORT").c_str(), is_main ? 8000 : 8001);
    set_value_int((p + "WEBRTC_QUEUE_CAPACITY").c_str(), 32);
    set_value_int((p + "WEBRTC_VIDEO_FPS").c_str(), fps);
}

static void set_audio_defaults(void) {
    set_value_int("AUDIO_ENABLE", 0);
    set_value("AUDIO_DEVICE", "default");
    set_value_int("AUDIO_SAMPLE_RATE", 8000);
    set_value_int("AUDIO_CAPTURE_CHANNELS", 1);
    set_value_int("AUDIO_FORMAT", AUDIO_SAMPLE_FORMAT_S16LE);
    set_value_int("AUDIO_PERIOD_FRAMES", 160);
    set_value_int("AUDIO_BUFFER_PERIODS", 4);
    set_value_int("AUDIO_SOURCE_SLOTS", 8);
    set_value_int("AUDIO_RETRY_MS", 5);
    set_value_int("AUDIO_MAX_CONSECUTIVE_FAILURES", 30);
    set_value_int("AUDIO_CODEC", MEDIA_CODEC_NONE);
    set_value_int("AUDIO_ENCODER_CHANNELS", 1);
    set_value_int("AUDIO_ENCODER_INPUT_CHANNEL", 0);
    set_value_int("AUDIO_G711_MODE", G711_ENCODER_MODE_ALAW);
    set_value_int("AUDIO_AAC_BITRATE", 32000);
    set_value_int("AUDIO_AAC_PROFILE", 2);
    set_value_int("AUDIO_OPUS_BITRATE", 24000);
    set_value_int("AUDIO_OPUS_COMPLEXITY", 6);
    set_value_int("AUDIO_OPUS_VBR", 1);
    set_value_int("AUDIO_OPUS_FEC", 1);
    set_value_int("AUDIO_OPUS_DTX", 0);
    set_value_int("AUDIO_OPUS_PACKET_LOSS_PERCENT", 10);
    set_value_int("AUDIO_BIND_STREAM_INDEX", 0);
}

static void load_defaults(void) {
    g_values.clear();

    set_value_int("GATEWAY_ENABLE_RTSP", 1);
    set_value_int("GATEWAY_ENABLE_RTMP", 0);
    set_value_int("GATEWAY_ENABLE_GB28181", 1);
    set_value_int("GATEWAY_ENABLE_WEBRTC", 0);
    set_value_int("GATEWAY_FPS", 30);
    set_value_int("GATEWAY_BITRATE", 2 * 1024 * 1024);
    set_value_int("GATEWAY_GOP", 30);
    set_value_int("GATEWAY_RC_MODE", MPP_ENC_RC_MODE_CBR);
    set_value_int("GATEWAY_H264_PROFILE", 100);
    set_value_int("GATEWAY_H264_LEVEL", 40);
    set_value_int("GATEWAY_H264_CABAC_EN", 1);
    set_value_int("GATEWAY_QP_INIT", 30);
    set_value_int("GATEWAY_QP_MIN", 22);
    set_value_int("GATEWAY_QP_MAX", 44);
    set_value_int("GATEWAY_QP_MIN_I", 20);
    set_value_int("GATEWAY_QP_MAX_I", 40);
    set_value_int("GATEWAY_QP_MAX_STEP", 8);
    set_value_int("GATEWAY_LOW_LATENCY_MODE", 1);
    set_value_int("GATEWAY_STATS_INTERVAL_SEC", 1);
    set_value_int("GATEWAY_CAPTURE_RETRY_MS", 5);
    set_value_int("GATEWAY_MAX_CONSECUTIVE_FAILURES", 30);
    set_value("GATEWAY_RECORD_FILE_PATH", "");
    set_value_int("GATEWAY_RECORD_FLUSH_INTERVAL_FRAMES", 30);
    set_value("GATEWAY_CONFIG_FILE_PATH", "media_gateway.toml");
    set_value_int("GATEWAY_BENCH_ENABLE", 0);
    set_value_int("GATEWAY_BENCH_SAMPLE_EVERY", 1);
    set_value_int("GATEWAY_BENCH_PRINT_INTERVAL_SEC", 1);
    set_value_int("LOG_LEVEL", 1);
    set_value_int("DYNAMIC_FPS_ENABLE", 0);
    set_value_int("DYNAMIC_FPS_NORMAL_FPS", 30);
    set_value_int("DYNAMIC_FPS_LOW_LIGHT_FPS", 15);
    set_value_int("DYNAMIC_FPS_BRIGHT_FPS", 60);
    set_value_int("DYNAMIC_FPS_MIN_SWITCH_INTERVAL_MS", 30000);
    set_value_int("DYNAMIC_FPS_EVALUATE_INTERVAL_MS", 1000);
    set_value_int("DYNAMIC_FPS_BRIGHT_CONFIRM_MS", 1000);
    set_value_int("DYNAMIC_FPS_LOW_LIGHT_CONFIRM_MS", 15000);
    set_value("DYNAMIC_FPS_BRIGHT_MAX_EXPOSURE_US", "8000");
    set_value("DYNAMIC_FPS_BRIGHT_MAX_ANALOG_GAIN", "2.0");
    set_value("DYNAMIC_FPS_BRIGHT_MIN_MEAN_LUMA", "58");
    set_value("DYNAMIC_FPS_LOW_LIGHT_MIN_EXPOSURE_RATIO", "0.85");
    set_value("DYNAMIC_FPS_LOW_LIGHT_MIN_ANALOG_GAIN", "4.0");
    set_value("DYNAMIC_FPS_LOW_LIGHT_MAX_MEAN_LUMA", "42");
    set_value_int("DYNAMIC_FPS_AE_SCENE_CONFIRM_MS", 3000);
    set_value_int("GATEWAY_CAPTURE_SOURCE_COUNT", 2);
    set_value_int("GATEWAY_STREAM_COUNT", 2);

    set_value_int("ISP_ENABLE", 0);
    set_value("ISP_SENSOR_NAME", "");
    set_value("ISP_VIDEO_DEVICE", "/dev/video0");
    set_value("ISP_IQ_DIR", "thirdparty/rkaiq");
    set_value("ISP_FORCE_IQ_FILE", "");
    set_value_int("ISP_WIDTH", 0);
    set_value_int("ISP_HEIGHT", 0);
    set_value_int("ISP_WORKING_MODE", 0);
    set_value_int("ISP_KEEP_EXTERNAL_HW_STATE", 0);
    set_value_int("ISP_FALLBACK_ON_ERROR", 1);
    set_value_int("ISP_HEALTH_CHECK_ENABLE", 1);
    set_value_int("ISP_META_TIMEOUT_MS", 2000);
    set_value_int("ISP_MAX_ERROR_COUNT", 3);
    set_value_int("ISP_RESTART_ON_FAULT", 0);
    set_value_int("ISP_LOW_LIGHT_ENABLE", 0);
    set_value("ISP_LOW_LIGHT_ENTER_LUX", "20");
    set_value("ISP_LOW_LIGHT_EXIT_LUX", "35");
    set_value("ISP_LOW_LIGHT_ENTER_MEAN_LUMA", "35");
    set_value("ISP_LOW_LIGHT_EXIT_MEAN_LUMA", "48");
    set_value("ISP_LOW_LIGHT_EXPOSURE_GAIN", "-1");
    set_value("ISP_LOW_LIGHT_EXPOSURE_TIME", "-1");
    set_value_int("ISP_LOW_LIGHT_NR_STRENGTH", 70);
    set_value_int("ISP_LOW_LIGHT_SHARPNESS", 35);
    set_value_int("ISP_LOW_LIGHT_NORMAL_NR_STRENGTH", -1);
    set_value_int("ISP_LOW_LIGHT_NORMAL_SHARPNESS", -1);
    set_value_int("ISP_LOW_LIGHT_BITRATE_BOOST_PERCENT", 25);
    set_value_int("ISP_LOW_LIGHT_QP_DELTA", -3);
    set_value_int("ISP_LOW_LIGHT_MIN_SWITCH_INTERVAL_MS", 5000);
    set_value_int("ISP_CONTROL_ENABLE", 0);
    set_value_int("ISP_BRIGHTNESS", -1);
    set_value_int("ISP_CONTRAST", -1);
    set_value_int("ISP_SATURATION", -1);
    set_value_int("ISP_HUE", -1);
    set_value_int("ISP_SHARPNESS", -1);
    set_value_int("ISP_EXPOSURE_MODE", -1);
    set_value("ISP_EXPOSURE_TIME", "-1");
    set_value("ISP_EXPOSURE_GAIN", "-1");
    set_value_int("ISP_WB_MODE", -1);
    set_value_int("ISP_WB_CT", 0);
    set_value("ISP_WB_RGAIN", "-1");
    set_value("ISP_WB_GRGAIN", "-1");
    set_value("ISP_WB_GBGAIN", "-1");
    set_value("ISP_WB_BGAIN", "-1");
    set_value_int("ISP_ANTI_FLICKER_ENABLE", -1);
    set_value_int("ISP_POWER_LINE_FREQ", -1);
    set_value_int("ISP_NR_MODE", -1);
    set_value_int("ISP_ANR_STRENGTH", -1);
    set_value_int("ISP_DEHAZE_MODE", -1);
    set_value_int("ISP_DEHAZE_STRENGTH", -1);

    set_value_int("CAPTURE_MAIN_ENABLE", 1);
    set_value("CAPTURE_MAIN_NAME", "main_path");
    set_value("CAPTURE_MAIN_DEVICE", "/dev/video0");
    set_value_int("CAPTURE_MAIN_WIDTH", CAPTURE_WIDTH);
    set_value_int("CAPTURE_MAIN_HEIGHT", CAPTURE_HEIGHT);
    set_value_int("CAPTURE_MAIN_PIXELFORMAT", CAPTURE_FORMAT);
    set_value_int("CAPTURE_MAIN_BUFFER_COUNT", V4L2_CAPTURE_BUFFER_COUNT);

    set_value_int("CAPTURE_SUB_ENABLE", 0);
    set_value("CAPTURE_SUB_NAME", "self_path");
    set_value("CAPTURE_SUB_DEVICE", "/dev/video1");
    set_value_int("CAPTURE_SUB_WIDTH", 1280);
    set_value_int("CAPTURE_SUB_HEIGHT", 720);
    set_value_int("CAPTURE_SUB_PIXELFORMAT", CAPTURE_FORMAT);
    set_value_int("CAPTURE_SUB_BUFFER_COUNT", V4L2_CAPTURE_BUFFER_COUNT);

    set_stream_defaults("STREAM_MAIN_", 1, CAPTURE_WIDTH, CAPTURE_HEIGHT, 30, 2 * 1024 * 1024);
    set_stream_defaults("STREAM_SUB_", 0, 1280, 720, 30, 1024 * 1024);
    set_audio_defaults();

    set_value("RTSP_NAME", "rtsp");
    set_value("RTSP_SESSION_NAME", "live");
    set_value("RTSP_SERVER_IP", "0.0.0.0");
    set_value_int("RTSP_SERVER_PORT", 8554);
    set_value_int("RTSP_AUTH_ENABLE", 0);
    set_value("RTSP_USER", "admin");
    set_value("RTSP_PASSWORD", "123456");
    set_value_int("RTSP_QUEUE_CAPACITY", 32);

    set_value("RTMP_NAME", "rtmp");
    set_value("RTMP_PUBLISH_URL", "rtmp://192.168.1.2/live/stream");
    set_value_int("RTMP_QUEUE_CAPACITY", 64);
    set_value_int("RTMP_RECONNECT_INTERVAL_MS", 1000);
    set_value_int("RTMP_CONNECT_TIMEOUT_MS", 3000);
    set_value_int("RTMP_AUDIO_ENABLED", 0);
    set_value_int("RTMP_VIDEO_WIDTH", CAPTURE_WIDTH);
    set_value_int("RTMP_VIDEO_HEIGHT", CAPTURE_HEIGHT);
    set_value_int("RTMP_VIDEO_FPS", 30);
    set_value_int("RTMP_VIDEO_BITRATE", 2 * 1024 * 1024);
    set_value("RTMP_VIDEO_CODEC_NAME", "H264");
    set_value("RTMP_ENCODER_NAME", "RKMediaGateway");

    set_value("GB28181_NAME", "gb28181");
    set_value("GB28181_SERVER_IP", "192.168.1.1");
    set_value_int("GB28181_SERVER_PORT", 5060);
    set_value("GB28181_SERVER_DOMAIN", "3402000000");
    set_value("GB28181_SERVER_ID", "34020000002000000001");
    set_value("GB28181_DEVICE_ID", "34020000001320000001");
    set_value("GB28181_DEVICE_DOMAIN", "3402000000");
    set_value("GB28181_DEVICE_PASSWORD", "12345678");
    set_value("GB28181_BIND_IP", "0.0.0.0");
    set_value_int("GB28181_LOCAL_SIP_PORT", 5060);
    set_value("GB28181_CONTACT_IP", "192.168.1.100");
    set_value("GB28181_MEDIA_IP", "192.168.1.100");
    set_value_int("GB28181_MEDIA_PORT", 30000);
    set_value_int("GB28181_REGISTER_EXPIRES", 3600);
    set_value_int("GB28181_KEEPALIVE_INTERVAL", 60);
    set_value_int("GB28181_REGISTER_RETRY_INTERVAL", 5);
    set_value("GB28181_DEVICE_NAME", "RK3568 Camera");
    set_value("GB28181_MANUFACTURER", "Topeet");
    set_value("GB28181_MODEL", "RKMediaGateway");
    set_value("GB28181_FIRMWARE", "1.0.0");
    set_value("GB28181_CHANNEL_ID", "34020000001320000001");
    set_value("GB28181_USER_AGENT", "RKMediaGateway-GB28181/1.0");
    set_value_int("GB28181_QUEUE_CAPACITY", 64);
}

static void fill_capture_source(CaptureSourceConfig *source, const char *prefix) {
    std::string p(prefix);

    source->enabled = value_int((p + "ENABLE").c_str());
    source->name = value_string((p + "NAME").c_str());
    source->device_path = value_string((p + "DEVICE").c_str());
    source->width = value_int((p + "WIDTH").c_str());
    source->height = value_int((p + "HEIGHT").c_str());
    source->pixelformat = (uint32_t)value_int((p + "PIXELFORMAT").c_str());
    source->buffer_count = value_int((p + "BUFFER_COUNT").c_str());
}

static void fill_audio_source(AudioSourceConfig *audio) {
    audio->enabled = value_int("AUDIO_ENABLE");
    audio->capture.device_name = value_string("AUDIO_DEVICE");
    audio->capture.sample_rate = value_int("AUDIO_SAMPLE_RATE");
    audio->capture.channels = value_int("AUDIO_CAPTURE_CHANNELS");
    audio->capture.format = (AudioSampleFormat)value_int("AUDIO_FORMAT");
    audio->capture.period_frames = value_int("AUDIO_PERIOD_FRAMES");
    audio->capture.buffer_periods = value_int("AUDIO_BUFFER_PERIODS");
    audio->runtime.source_slots = value_int("AUDIO_SOURCE_SLOTS");
    audio->runtime.retry_ms = value_int("AUDIO_RETRY_MS");
    audio->runtime.max_consecutive_failures = value_int("AUDIO_MAX_CONSECUTIVE_FAILURES");
    audio->encoder.codec = (MediaCodecType)value_int("AUDIO_CODEC");
    audio->encoder.channels = value_int("AUDIO_ENCODER_CHANNELS");
    audio->encoder.input_channel = value_int("AUDIO_ENCODER_INPUT_CHANNEL");
    audio->encoder.g711.mode = (G711EncoderMode)value_int("AUDIO_G711_MODE");
    audio->encoder.aac.bitrate = value_int("AUDIO_AAC_BITRATE");
    audio->encoder.aac.profile = value_int("AUDIO_AAC_PROFILE");
    audio->encoder.opus.bitrate = value_int("AUDIO_OPUS_BITRATE");
    audio->encoder.opus.complexity = value_int("AUDIO_OPUS_COMPLEXITY");
    audio->encoder.opus.vbr = value_int("AUDIO_OPUS_VBR");
    audio->encoder.opus.fec = value_int("AUDIO_OPUS_FEC");
    audio->encoder.opus.dtx = value_int("AUDIO_OPUS_DTX");
    audio->encoder.opus.packet_loss_percent = value_int("AUDIO_OPUS_PACKET_LOSS_PERCENT");
    audio->bind_stream_index = value_int("AUDIO_BIND_STREAM_INDEX");
}

static void fill_isp_source(IspSourceConfig *isp) {
    isp->enabled = value_int("ISP_ENABLE");
    isp->sensor_name = value_string("ISP_SENSOR_NAME");
    isp->video_device = value_string("ISP_VIDEO_DEVICE");
    isp->iq_dir = value_string("ISP_IQ_DIR");
    isp->force_iq_file = value_string("ISP_FORCE_IQ_FILE");
    isp->width = value_int("ISP_WIDTH");
    isp->height = value_int("ISP_HEIGHT");
    isp->working_mode = value_int("ISP_WORKING_MODE");
    isp->keep_external_hw_state = value_int("ISP_KEEP_EXTERNAL_HW_STATE");
    isp->fallback_on_error = value_int("ISP_FALLBACK_ON_ERROR");
    isp->health_check_enable = value_int("ISP_HEALTH_CHECK_ENABLE");
    isp->meta_timeout_ms = value_int("ISP_META_TIMEOUT_MS");
    isp->max_error_count = value_int("ISP_MAX_ERROR_COUNT");
    isp->restart_on_fault = value_int("ISP_RESTART_ON_FAULT");
    isp->low_light.enabled = value_int("ISP_LOW_LIGHT_ENABLE");
    isp->low_light.enter_lux = value_float("ISP_LOW_LIGHT_ENTER_LUX");
    isp->low_light.exit_lux = value_float("ISP_LOW_LIGHT_EXIT_LUX");
    isp->low_light.enter_mean_luma = value_float("ISP_LOW_LIGHT_ENTER_MEAN_LUMA");
    isp->low_light.exit_mean_luma = value_float("ISP_LOW_LIGHT_EXIT_MEAN_LUMA");
    isp->low_light.exposure_gain = value_float("ISP_LOW_LIGHT_EXPOSURE_GAIN");
    isp->low_light.exposure_time = value_float("ISP_LOW_LIGHT_EXPOSURE_TIME");
    isp->low_light.nr_strength = value_int("ISP_LOW_LIGHT_NR_STRENGTH");
    isp->low_light.sharpness = value_int("ISP_LOW_LIGHT_SHARPNESS");
    isp->low_light.normal_nr_strength = value_int("ISP_LOW_LIGHT_NORMAL_NR_STRENGTH");
    isp->low_light.normal_sharpness = value_int("ISP_LOW_LIGHT_NORMAL_SHARPNESS");
    isp->low_light.bitrate_boost_percent = value_int("ISP_LOW_LIGHT_BITRATE_BOOST_PERCENT");
    isp->low_light.qp_delta = value_int("ISP_LOW_LIGHT_QP_DELTA");
    isp->low_light.min_switch_interval_ms = value_int("ISP_LOW_LIGHT_MIN_SWITCH_INTERVAL_MS");
    isp->controls.enabled = value_int("ISP_CONTROL_ENABLE");
    isp->controls.brightness = value_int("ISP_BRIGHTNESS");
    isp->controls.contrast = value_int("ISP_CONTRAST");
    isp->controls.saturation = value_int("ISP_SATURATION");
    isp->controls.hue = value_int("ISP_HUE");
    isp->controls.sharpness = value_int("ISP_SHARPNESS");
    isp->controls.exposure_mode = value_int("ISP_EXPOSURE_MODE");
    isp->controls.exposure_time = value_float("ISP_EXPOSURE_TIME");
    isp->controls.exposure_gain = value_float("ISP_EXPOSURE_GAIN");
    isp->controls.wb_mode = value_int("ISP_WB_MODE");
    isp->controls.wb_ct = value_int("ISP_WB_CT");
    isp->controls.wb_rgain = value_float("ISP_WB_RGAIN");
    isp->controls.wb_grgain = value_float("ISP_WB_GRGAIN");
    isp->controls.wb_gbgain = value_float("ISP_WB_GBGAIN");
    isp->controls.wb_bgain = value_float("ISP_WB_BGAIN");
    isp->controls.anti_flicker_enable = value_int("ISP_ANTI_FLICKER_ENABLE");
    isp->controls.power_line_freq = value_int("ISP_POWER_LINE_FREQ");
    isp->controls.nr_mode = value_int("ISP_NR_MODE");
    isp->controls.anr_strength = value_int("ISP_ANR_STRENGTH");
    isp->controls.dehaze_mode = value_int("ISP_DEHAZE_MODE");
    isp->controls.dehaze_strength = value_int("ISP_DEHAZE_STRENGTH");
}

static void fill_stream(MediaGatewayStreamConfig *stream, const char *prefix) {
    std::string p(prefix);

    stream->enabled = value_int((p + "ENABLE").c_str());
    stream->name = value_string((p + "NAME").c_str());
    stream->source_index = value_int((p + "SOURCE_INDEX").c_str());
    stream->width = value_int((p + "WIDTH").c_str());
    stream->height = value_int((p + "HEIGHT").c_str());
    stream->fps = value_int((p + "FPS").c_str());
    stream->bitrate = value_int((p + "BITRATE").c_str()); /* STREAM_MAIN_BITRATE */
    stream->gop = value_int((p + "GOP").c_str());
    stream->rc_mode = value_int((p + "RC_MODE").c_str());
    stream->h264_profile = value_int((p + "H264_PROFILE").c_str());
    stream->h264_level = value_int((p + "H264_LEVEL").c_str());
    stream->h264_cabac_en = value_int((p + "H264_CABAC_EN").c_str());
    stream->qp_init = value_int((p + "QP_INIT").c_str());
    stream->qp_min = value_int((p + "QP_MIN").c_str());
    stream->qp_max = value_int((p + "QP_MAX").c_str());
    stream->qp_min_i = value_int((p + "QP_MIN_I").c_str());
    stream->qp_max_i = value_int((p + "QP_MAX_I").c_str());
    stream->qp_max_step = value_int((p + "QP_MAX_STEP").c_str());

    /*
     * 读取每路码流的动态编码档位。缺失或非法字段会在 mediaGateway 配置
     * 归一化阶段补齐，保证运行期下发给 MPP 的 profile 是完整的。
     */
    stream->dynamic_profiles.low_light.fps = value_int((p + "DYNAMIC_LOW_FPS").c_str());
    stream->dynamic_profiles.low_light.bitrate = value_int((p + "DYNAMIC_LOW_BITRATE").c_str());
    stream->dynamic_profiles.low_light.gop = value_int((p + "DYNAMIC_LOW_GOP").c_str());
    stream->dynamic_profiles.low_light.rc_mode = value_int((p + "DYNAMIC_LOW_RC_MODE").c_str());
    stream->dynamic_profiles.low_light.qp_init = value_int((p + "DYNAMIC_LOW_QP_INIT").c_str());
    stream->dynamic_profiles.low_light.qp_min = value_int((p + "DYNAMIC_LOW_QP_MIN").c_str());
    stream->dynamic_profiles.low_light.qp_max = value_int((p + "DYNAMIC_LOW_QP_MAX").c_str());
    stream->dynamic_profiles.low_light.qp_min_i = value_int((p + "DYNAMIC_LOW_QP_MIN_I").c_str());
    stream->dynamic_profiles.low_light.qp_max_i = value_int((p + "DYNAMIC_LOW_QP_MAX_I").c_str());
    stream->dynamic_profiles.low_light.qp_max_step = value_int((p + "DYNAMIC_LOW_QP_MAX_STEP").c_str());
    stream->dynamic_profiles.normal.fps = value_int((p + "DYNAMIC_NORMAL_FPS").c_str());
    stream->dynamic_profiles.normal.bitrate = value_int((p + "DYNAMIC_NORMAL_BITRATE").c_str());
    stream->dynamic_profiles.normal.gop = value_int((p + "DYNAMIC_NORMAL_GOP").c_str());
    stream->dynamic_profiles.normal.rc_mode = value_int((p + "DYNAMIC_NORMAL_RC_MODE").c_str());
    stream->dynamic_profiles.normal.qp_init = value_int((p + "DYNAMIC_NORMAL_QP_INIT").c_str());
    stream->dynamic_profiles.normal.qp_min = value_int((p + "DYNAMIC_NORMAL_QP_MIN").c_str());
    stream->dynamic_profiles.normal.qp_max = value_int((p + "DYNAMIC_NORMAL_QP_MAX").c_str());
    stream->dynamic_profiles.normal.qp_min_i = value_int((p + "DYNAMIC_NORMAL_QP_MIN_I").c_str());
    stream->dynamic_profiles.normal.qp_max_i = value_int((p + "DYNAMIC_NORMAL_QP_MAX_I").c_str());
    stream->dynamic_profiles.normal.qp_max_step = value_int((p + "DYNAMIC_NORMAL_QP_MAX_STEP").c_str());
    stream->dynamic_profiles.bright.fps = value_int((p + "DYNAMIC_BRIGHT_FPS").c_str());
    stream->dynamic_profiles.bright.bitrate = value_int((p + "DYNAMIC_BRIGHT_BITRATE").c_str());
    stream->dynamic_profiles.bright.gop = value_int((p + "DYNAMIC_BRIGHT_GOP").c_str());
    stream->dynamic_profiles.bright.rc_mode = value_int((p + "DYNAMIC_BRIGHT_RC_MODE").c_str());
    stream->dynamic_profiles.bright.qp_init = value_int((p + "DYNAMIC_BRIGHT_QP_INIT").c_str());
    stream->dynamic_profiles.bright.qp_min = value_int((p + "DYNAMIC_BRIGHT_QP_MIN").c_str());
    stream->dynamic_profiles.bright.qp_max = value_int((p + "DYNAMIC_BRIGHT_QP_MAX").c_str());
    stream->dynamic_profiles.bright.qp_min_i = value_int((p + "DYNAMIC_BRIGHT_QP_MIN_I").c_str());
    stream->dynamic_profiles.bright.qp_max_i = value_int((p + "DYNAMIC_BRIGHT_QP_MAX_I").c_str());
    stream->dynamic_profiles.bright.qp_max_step = value_int((p + "DYNAMIC_BRIGHT_QP_MAX_STEP").c_str());

    stream->enable_rtsp = value_int((p + "ENABLE_RTSP").c_str());
    stream->enable_rtmp = value_int((p + "ENABLE_RTMP").c_str());
    stream->enable_gb28181 = value_int((p + "ENABLE_GB28181").c_str());
    stream->enable_webrtc = value_int((p + "ENABLE_WEBRTC").c_str());

    stream->rtsp.name = value_string((p + "RTSP_NAME").c_str());
    stream->rtsp.session_name = value_string((p + "RTSP_SESSION_NAME").c_str());
    stream->rtsp.server_ip = value_string((p + "RTSP_SERVER_IP").c_str());
    stream->rtsp.server_port = value_int((p + "RTSP_SERVER_PORT").c_str());
    stream->rtsp.auth_enable = value_int((p + "RTSP_AUTH_ENABLE").c_str());
    stream->rtsp.user = value_string((p + "RTSP_USER").c_str());
    stream->rtsp.password = value_string((p + "RTSP_PASSWORD").c_str());
    stream->rtsp.queue_capacity = value_int((p + "RTSP_QUEUE_CAPACITY").c_str());
    stream->rtsp.immediate_sps_pps_on_new_client = value_int((p + "RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT").c_str());

    stream->rtmp.name = value_string((p + "RTMP_NAME").c_str());
    stream->rtmp.publish_url = value_string((p + "RTMP_PUBLISH_URL").c_str());
    stream->rtmp.queue_capacity = value_int((p + "RTMP_QUEUE_CAPACITY").c_str());
    stream->rtmp.reconnect_interval_ms = value_int((p + "RTMP_RECONNECT_INTERVAL_MS").c_str());
    stream->rtmp.connect_timeout_ms = value_int((p + "RTMP_CONNECT_TIMEOUT_MS").c_str());
    stream->rtmp.audio_enabled = value_int((p + "RTMP_AUDIO_ENABLED").c_str());
    stream->rtmp.video_width = value_int((p + "RTMP_VIDEO_WIDTH").c_str());
    stream->rtmp.video_height = value_int((p + "RTMP_VIDEO_HEIGHT").c_str());
    stream->rtmp.video_fps = value_int((p + "RTMP_VIDEO_FPS").c_str());
    stream->rtmp.video_bitrate = value_int((p + "RTMP_VIDEO_BITRATE").c_str());
    stream->rtmp.video_codec_name = value_string((p + "RTMP_VIDEO_CODEC_NAME").c_str());
    stream->rtmp.encoder_name = value_string((p + "RTMP_ENCODER_NAME").c_str());

    stream->gb28181.name = value_string((p + "GB28181_NAME").c_str());
    stream->gb28181.server_ip = value_string((p + "GB28181_SERVER_IP").c_str());
    stream->gb28181.server_port = value_int((p + "GB28181_SERVER_PORT").c_str());
    stream->gb28181.server_domain = value_string((p + "GB28181_SERVER_DOMAIN").c_str());
    stream->gb28181.server_id = value_string((p + "GB28181_SERVER_ID").c_str());
    stream->gb28181.device_id = value_string((p + "GB28181_DEVICE_ID").c_str());
    stream->gb28181.device_domain = value_string((p + "GB28181_DEVICE_DOMAIN").c_str());
    stream->gb28181.device_password = value_string((p + "GB28181_DEVICE_PASSWORD").c_str());
    stream->gb28181.bind_ip = value_string((p + "GB28181_BIND_IP").c_str());
    stream->gb28181.local_sip_port = value_int((p + "GB28181_LOCAL_SIP_PORT").c_str());
    stream->gb28181.sip_contact_ip = value_string((p + "GB28181_CONTACT_IP").c_str());
    stream->gb28181.media_ip = value_string((p + "GB28181_MEDIA_IP").c_str());
    stream->gb28181.media_port = value_int((p + "GB28181_MEDIA_PORT").c_str());
    stream->gb28181.register_expires = value_int((p + "GB28181_REGISTER_EXPIRES").c_str());
    stream->gb28181.keepalive_interval_sec = value_int((p + "GB28181_KEEPALIVE_INTERVAL").c_str());
    stream->gb28181.register_retry_interval_sec = value_int((p + "GB28181_REGISTER_RETRY_INTERVAL").c_str());
    stream->gb28181.device_name = value_string((p + "GB28181_DEVICE_NAME").c_str());
    stream->gb28181.manufacturer = value_string((p + "GB28181_MANUFACTURER").c_str());
    stream->gb28181.model = value_string((p + "GB28181_MODEL").c_str());
    stream->gb28181.firmware = value_string((p + "GB28181_FIRMWARE").c_str());
    stream->gb28181.channel_id = value_string((p + "GB28181_CHANNEL_ID").c_str());
    stream->gb28181.user_agent = value_string((p + "GB28181_USER_AGENT").c_str());
    stream->gb28181.queue_capacity = value_int((p + "GB28181_QUEUE_CAPACITY").c_str());

    snprintf(stream->webrtc.name, sizeof(stream->webrtc.name), "%s",
             value_string((p + "WEBRTC_NAME").c_str()));
    snprintf(stream->webrtc.bind_address, sizeof(stream->webrtc.bind_address), "%s",
             value_string((p + "WEBRTC_BIND_ADDRESS").c_str()));
    stream->webrtc.port = value_int((p + "WEBRTC_PORT").c_str());
    stream->webrtc.queue_capacity = value_int((p + "WEBRTC_QUEUE_CAPACITY").c_str());
    stream->webrtc.video_fps = value_int((p + "WEBRTC_VIDEO_FPS").c_str());
}

int def_value_init(const char *config_path) {
    std::string path = config_path ? config_path : "";

    load_defaults();
    if (path.size() < 5 || path.substr(path.size() - 5) != ".toml") {
        std::fprintf(stderr, "configuration file must use the .toml extension: %s\n",
                     path.c_str());
        return -1;
    }
    if (load_toml_values(config_path) != 0)
        return -1;
    set_value("GATEWAY_CONFIG_FILE_PATH", config_path);
    return 0;
}

void def_value_get_media_gateway_config(MediaGatewayConfig *config) {
    if (!config) {
        return;
    }

    *config = MediaGatewayConfig();
    config->output.switches.enable_rtsp = value_int("GATEWAY_ENABLE_RTSP");
    config->output.switches.enable_rtmp = value_int("GATEWAY_ENABLE_RTMP");
    config->output.switches.enable_gb28181 = value_int("GATEWAY_ENABLE_GB28181");
    config->output.switches.enable_webrtc = value_int("GATEWAY_ENABLE_WEBRTC");
    config->video.encode.fps = value_int("GATEWAY_FPS");
    config->video.encode.bitrate = value_int("GATEWAY_BITRATE");
    config->video.encode.gop = value_int("GATEWAY_GOP");
    config->video.encode.rc_mode = value_int("GATEWAY_RC_MODE");
    config->video.encode.h264_profile = value_int("GATEWAY_H264_PROFILE");
    config->video.encode.h264_level = value_int("GATEWAY_H264_LEVEL");
    config->video.encode.h264_cabac_en = value_int("GATEWAY_H264_CABAC_EN");
    config->video.encode.qp_init = value_int("GATEWAY_QP_INIT");
    config->video.encode.qp_min = value_int("GATEWAY_QP_MIN");
    config->video.encode.qp_max = value_int("GATEWAY_QP_MAX");
    config->video.encode.qp_min_i = value_int("GATEWAY_QP_MIN_I");
    config->video.encode.qp_max_i = value_int("GATEWAY_QP_MAX_I");
    config->video.encode.qp_max_step = value_int("GATEWAY_QP_MAX_STEP");
    config->system.runtime.low_latency_mode = value_int("GATEWAY_LOW_LATENCY_MODE");
    config->system.runtime.stats_interval_sec = value_int("GATEWAY_STATS_INTERVAL_SEC");
    config->system.runtime.capture_retry_ms = value_int("GATEWAY_CAPTURE_RETRY_MS");
    config->system.runtime.max_consecutive_failures = value_int("GATEWAY_MAX_CONSECUTIVE_FAILURES");
    config->system.record.file_path = value_string("GATEWAY_RECORD_FILE_PATH");
    config->system.record.flush_interval_frames = value_int("GATEWAY_RECORD_FLUSH_INTERVAL_FRAMES");
    config->system.runtime.config_file_path = value_string("GATEWAY_CONFIG_FILE_PATH");
    config->system.bench.enabled = value_int("GATEWAY_BENCH_ENABLE");
    config->system.bench.sample_every = value_int("GATEWAY_BENCH_SAMPLE_EVERY");
    config->system.bench.print_interval_sec = value_int("GATEWAY_BENCH_PRINT_INTERVAL_SEC");
    config->system.log.level = value_int("LOG_LEVEL");
    config->policy.light_fps.enabled = value_int("DYNAMIC_FPS_ENABLE");
    config->policy.light_fps.targets.normal_fps = value_int("DYNAMIC_FPS_NORMAL_FPS");
    config->policy.light_fps.targets.low_light_fps = value_int("DYNAMIC_FPS_LOW_LIGHT_FPS");
    config->policy.light_fps.targets.bright_fps = value_int("DYNAMIC_FPS_BRIGHT_FPS");
    config->policy.light_fps.timing.min_switch_interval_ms = value_int("DYNAMIC_FPS_MIN_SWITCH_INTERVAL_MS");
    config->policy.light_fps.timing.evaluate_interval_ms = value_int("DYNAMIC_FPS_EVALUATE_INTERVAL_MS");
    config->policy.light_fps.timing.bright_confirm_ms = value_int("DYNAMIC_FPS_BRIGHT_CONFIRM_MS");
    config->policy.light_fps.timing.low_light_confirm_ms = value_int("DYNAMIC_FPS_LOW_LIGHT_CONFIRM_MS");
    config->policy.light_fps.timing.ae_scene_confirm_ms = value_int("DYNAMIC_FPS_AE_SCENE_CONFIRM_MS");
    config->policy.light_fps.ae.bright_max_exposure_us = value_float("DYNAMIC_FPS_BRIGHT_MAX_EXPOSURE_US");
    config->policy.light_fps.ae.bright_max_analog_gain = value_float("DYNAMIC_FPS_BRIGHT_MAX_ANALOG_GAIN");
    config->policy.light_fps.ae.bright_min_mean_luma = value_float("DYNAMIC_FPS_BRIGHT_MIN_MEAN_LUMA");
    config->policy.light_fps.ae.low_light_min_exposure_ratio = value_float("DYNAMIC_FPS_LOW_LIGHT_MIN_EXPOSURE_RATIO");
    config->policy.light_fps.ae.low_light_min_analog_gain = value_float("DYNAMIC_FPS_LOW_LIGHT_MIN_ANALOG_GAIN");
    config->policy.light_fps.ae.low_light_max_mean_luma = value_float("DYNAMIC_FPS_LOW_LIGHT_MAX_MEAN_LUMA");
    config->input.capture_source_count = value_int("GATEWAY_CAPTURE_SOURCE_COUNT");
    config->video.stream_count = value_int("GATEWAY_STREAM_COUNT");

    fill_capture_source(&config->input.capture_sources[0], "CAPTURE_MAIN_");
    fill_capture_source(&config->input.capture_sources[1], "CAPTURE_SUB_");
    fill_isp_source(&config->input.isp);
    fill_audio_source(&config->audio.source);
    fill_stream(&config->video.streams[0], "STREAM_MAIN_");
    fill_stream(&config->video.streams[1], "STREAM_SUB_");

    config->output.rtsp.name = value_string("RTSP_NAME");
    config->output.rtsp.session_name = value_string("RTSP_SESSION_NAME");
    config->output.rtsp.server_ip = value_string("RTSP_SERVER_IP");
    config->output.rtsp.server_port = value_int("RTSP_SERVER_PORT");
    config->output.rtsp.auth_enable = value_int("RTSP_AUTH_ENABLE");
    config->output.rtsp.user = value_string("RTSP_USER");
    config->output.rtsp.password = value_string("RTSP_PASSWORD");
    config->output.rtsp.queue_capacity = value_int("RTSP_QUEUE_CAPACITY");

    config->output.rtmp.name = value_string("RTMP_NAME");
    config->output.rtmp.publish_url = value_string("RTMP_PUBLISH_URL");
    config->output.rtmp.queue_capacity = value_int("RTMP_QUEUE_CAPACITY");
    config->output.rtmp.reconnect_interval_ms = value_int("RTMP_RECONNECT_INTERVAL_MS");
    config->output.rtmp.connect_timeout_ms = value_int("RTMP_CONNECT_TIMEOUT_MS");
    config->output.rtmp.audio_enabled = value_int("RTMP_AUDIO_ENABLED");
    config->output.rtmp.video_width = value_int("RTMP_VIDEO_WIDTH");
    config->output.rtmp.video_height = value_int("RTMP_VIDEO_HEIGHT");
    config->output.rtmp.video_fps = value_int("RTMP_VIDEO_FPS");
    config->output.rtmp.video_bitrate = value_int("RTMP_VIDEO_BITRATE");
    config->output.rtmp.video_codec_name = value_string("RTMP_VIDEO_CODEC_NAME");
    config->output.rtmp.encoder_name = value_string("RTMP_ENCODER_NAME");

    config->output.gb28181.name = value_string("GB28181_NAME");
    config->output.gb28181.server_ip = value_string("GB28181_SERVER_IP");
    config->output.gb28181.server_port = value_int("GB28181_SERVER_PORT");
    config->output.gb28181.server_domain = value_string("GB28181_SERVER_DOMAIN");
    config->output.gb28181.server_id = value_string("GB28181_SERVER_ID");
    config->output.gb28181.device_id = value_string("GB28181_DEVICE_ID");
    config->output.gb28181.device_domain = value_string("GB28181_DEVICE_DOMAIN");
    config->output.gb28181.device_password = value_string("GB28181_DEVICE_PASSWORD");
    config->output.gb28181.bind_ip = value_string("GB28181_BIND_IP");
    config->output.gb28181.local_sip_port = value_int("GB28181_LOCAL_SIP_PORT");
    config->output.gb28181.sip_contact_ip = value_string("GB28181_CONTACT_IP");
    config->output.gb28181.media_ip = value_string("GB28181_MEDIA_IP");
    config->output.gb28181.media_port = value_int("GB28181_MEDIA_PORT");
    config->output.gb28181.register_expires = value_int("GB28181_REGISTER_EXPIRES");
    config->output.gb28181.keepalive_interval_sec = value_int("GB28181_KEEPALIVE_INTERVAL");
    config->output.gb28181.register_retry_interval_sec = value_int("GB28181_REGISTER_RETRY_INTERVAL");
    config->output.gb28181.device_name = value_string("GB28181_DEVICE_NAME");
    config->output.gb28181.manufacturer = value_string("GB28181_MANUFACTURER");
    config->output.gb28181.model = value_string("GB28181_MODEL");
    config->output.gb28181.firmware = value_string("GB28181_FIRMWARE");
    config->output.gb28181.channel_id = value_string("GB28181_CHANNEL_ID");
    config->output.gb28181.user_agent = value_string("GB28181_USER_AGENT");
    config->output.gb28181.queue_capacity = value_int("GB28181_QUEUE_CAPACITY");
}

int def_value_loaded(void) {
    return g_loaded;
}

const char *def_value_source_path(void) {
    return g_source_path.c_str();
}
