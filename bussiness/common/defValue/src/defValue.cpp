#include "defValue.h"

#include <cstdlib>
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

static int file_has_key(const char *key) {
    return g_file_values.find(key) != g_file_values.end();
}

static void load_file_values(const char *config_path) {
    std::ifstream file;
    std::string line;

    g_file_values.clear();
    g_source_path = config_path ? config_path : "";
    g_loaded = 0;

    if (!config_path || config_path[0] == '\0') {
        return;
    }

    file.open(config_path);
    if (!file.is_open()) {
        return;
    }

    while (std::getline(file, line)) {
        std::string trimmed = trim_copy(line);
        std::string key;
        std::string value;
        std::string::size_type eq_pos;

        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        key = trim_copy(trimmed.substr(0, eq_pos));
        value = trim_copy(trimmed.substr(eq_pos + 1));
        if (key.empty()) {
            continue;
        }

        if (value.size() >= 2) {
            char first = value[0];
            char last = value[value.size() - 1];
            if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
        }

        g_file_values[key] = value;
        g_values[key] = value;
    }

    g_loaded = 1;
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
    set_value_int((p + "H264_LEVEL").c_str(), 40);
    set_value_int((p + "H264_CABAC_EN").c_str(), 1);
    set_value_int((p + "QP_INIT").c_str(), 30);
    set_value_int((p + "QP_MIN").c_str(), 22);
    set_value_int((p + "QP_MAX").c_str(), 44);
    set_value_int((p + "QP_MIN_I").c_str(), 20);
    set_value_int((p + "QP_MAX_I").c_str(), 40);
    set_value_int((p + "QP_MAX_STEP").c_str(), 8);

    set_value_int((p + "ENABLE_RTSP").c_str(), 1);
    set_value_int((p + "ENABLE_RTMP").c_str(), 0);
    set_value_int((p + "ENABLE_GB28181").c_str(), is_main ? 1 : 0);

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
}

static void set_audio_defaults(void) {
    set_value_int("AUDIO_ENABLE", 0);
    set_value("AUDIO_DEVICE", "default");
    set_value_int("AUDIO_SAMPLE_RATE", 8000);
    set_value_int("AUDIO_CHANNELS", 1);
    set_value_int("AUDIO_FORMAT", AUDIO_SAMPLE_FORMAT_S16LE);
    set_value_int("AUDIO_PERIOD_FRAMES", 160);
    set_value_int("AUDIO_BUFFER_PERIODS", 4);
    set_value_int("AUDIO_SOURCE_SLOTS", 8);
    set_value_int("AUDIO_RETRY_MS", 5);
    set_value_int("AUDIO_MAX_CONSECUTIVE_FAILURES", 30);
    set_value_int("AUDIO_CODEC", MEDIA_CODEC_NONE);
    set_value_int("AUDIO_G711_MODE", G711_ENCODER_MODE_ALAW);
    set_value_int("AUDIO_AAC_BITRATE", 32000);
    set_value_int("AUDIO_AAC_PROFILE", 2);
    set_value_int("AUDIO_BIND_STREAM_INDEX", 0);
}

static void load_defaults(void) {
    g_values.clear();

    set_value_int("GATEWAY_ENABLE_RTSP", 1);
    set_value_int("GATEWAY_ENABLE_RTMP", 0);
    set_value_int("GATEWAY_ENABLE_GB28181", 1);
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
    set_value("GATEWAY_CONFIG_FILE_PATH", "media_gateway.conf");
    set_value_int("GATEWAY_BENCH_ENABLE", 0);
    set_value_int("GATEWAY_BENCH_SAMPLE_EVERY", 1);
    set_value_int("GATEWAY_BENCH_PRINT_INTERVAL_SEC", 1);
    set_value_int("LOG_LEVEL", 1);
    set_value_int("GATEWAY_CAPTURE_SOURCE_COUNT", 2);
    set_value_int("GATEWAY_STREAM_COUNT", 2);

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
    audio->device_name = value_string("AUDIO_DEVICE");
    audio->sample_rate = value_int("AUDIO_SAMPLE_RATE");
    audio->channels = value_int("AUDIO_CHANNELS");
    audio->format = (AudioSampleFormat)value_int("AUDIO_FORMAT");
    audio->period_frames = value_int("AUDIO_PERIOD_FRAMES");
    audio->buffer_periods = value_int("AUDIO_BUFFER_PERIODS");
    audio->source_slots = value_int("AUDIO_SOURCE_SLOTS");
    audio->retry_ms = value_int("AUDIO_RETRY_MS");
    audio->max_consecutive_failures = value_int("AUDIO_MAX_CONSECUTIVE_FAILURES");
    audio->codec = (MediaCodecType)value_int("AUDIO_CODEC");
    audio->g711_mode = (G711EncoderMode)value_int("AUDIO_G711_MODE");
    audio->aac_bitrate = value_int("AUDIO_AAC_BITRATE");
    audio->aac_profile = value_int("AUDIO_AAC_PROFILE");
    audio->bind_stream_index = value_int("AUDIO_BIND_STREAM_INDEX");
}

static void fill_stream(MediaGatewayStreamConfig *stream, const char *prefix) {
    std::string p(prefix);

    stream->enabled = value_int((p + "ENABLE").c_str());
    stream->name = value_string((p + "NAME").c_str());
    stream->source_index = value_int((p + "SOURCE_INDEX").c_str());
    stream->width = value_int((p + "WIDTH").c_str());
    stream->height = value_int((p + "HEIGHT").c_str());
    stream->fps = value_int((p + "FPS").c_str());
    stream->bitrate = value_int((p + "BITRATE").c_str());
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

    stream->enable_rtsp = value_int((p + "ENABLE_RTSP").c_str());
    stream->enable_rtmp = value_int((p + "ENABLE_RTMP").c_str());
    stream->enable_gb28181 = value_int((p + "ENABLE_GB28181").c_str());

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
}

static void fill_legacy_single_stream(MediaGatewayConfig *config) {
    config->stream_count = 1;
    config->capture_source_count = 1;

    config->capture_sources[0].enabled = 1;
    config->capture_sources[0].name = "main_path";
    config->capture_sources[0].device_path = "/dev/video0";
    config->capture_sources[0].width = CAPTURE_WIDTH;
    config->capture_sources[0].height = CAPTURE_HEIGHT;
    config->capture_sources[0].pixelformat = CAPTURE_FORMAT;
    config->capture_sources[0].buffer_count = V4L2_CAPTURE_BUFFER_COUNT;

    config->streams[0].enabled = 1;
    config->streams[0].name = "main";
    config->streams[0].source_index = 0;
    config->streams[0].width = CAPTURE_WIDTH;
    config->streams[0].height = CAPTURE_HEIGHT;
    config->streams[0].fps = value_int("GATEWAY_FPS");
    config->streams[0].bitrate = value_int("GATEWAY_BITRATE");
    config->streams[0].gop = value_int("GATEWAY_GOP");
    config->streams[0].rc_mode = value_int("GATEWAY_RC_MODE");
    config->streams[0].h264_profile = value_int("GATEWAY_H264_PROFILE");
    config->streams[0].h264_level = value_int("GATEWAY_H264_LEVEL");
    config->streams[0].h264_cabac_en = value_int("GATEWAY_H264_CABAC_EN");
    config->streams[0].qp_init = value_int("GATEWAY_QP_INIT");
    config->streams[0].qp_min = value_int("GATEWAY_QP_MIN");
    config->streams[0].qp_max = value_int("GATEWAY_QP_MAX");
    config->streams[0].qp_min_i = value_int("GATEWAY_QP_MIN_I");
    config->streams[0].qp_max_i = value_int("GATEWAY_QP_MAX_I");
    config->streams[0].qp_max_step = value_int("GATEWAY_QP_MAX_STEP");
    config->streams[0].enable_rtsp = value_int("GATEWAY_ENABLE_RTSP");
    config->streams[0].enable_rtmp = value_int("GATEWAY_ENABLE_RTMP");
    config->streams[0].enable_gb28181 = value_int("GATEWAY_ENABLE_GB28181");
    config->streams[0].rtsp.name = value_string("RTSP_NAME");
    config->streams[0].rtsp.session_name = value_string("RTSP_SESSION_NAME");
    config->streams[0].rtsp.server_ip = value_string("RTSP_SERVER_IP");
    config->streams[0].rtsp.server_port = value_int("RTSP_SERVER_PORT");
    config->streams[0].rtsp.auth_enable = value_int("RTSP_AUTH_ENABLE");
    config->streams[0].rtsp.user = value_string("RTSP_USER");
    config->streams[0].rtsp.password = value_string("RTSP_PASSWORD");
    config->streams[0].rtsp.queue_capacity = value_int("RTSP_QUEUE_CAPACITY");
    config->streams[0].rtsp.immediate_sps_pps_on_new_client =
        value_int("GATEWAY_RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT");
    config->streams[0].rtmp.name = value_string("RTMP_NAME");
    config->streams[0].rtmp.publish_url = value_string("RTMP_PUBLISH_URL");
    config->streams[0].rtmp.queue_capacity = value_int("RTMP_QUEUE_CAPACITY");
    config->streams[0].rtmp.reconnect_interval_ms = value_int("RTMP_RECONNECT_INTERVAL_MS");
    config->streams[0].rtmp.connect_timeout_ms = value_int("RTMP_CONNECT_TIMEOUT_MS");
    config->streams[0].rtmp.audio_enabled = value_int("RTMP_AUDIO_ENABLED");
    config->streams[0].rtmp.video_width = value_int("RTMP_VIDEO_WIDTH");
    config->streams[0].rtmp.video_height = value_int("RTMP_VIDEO_HEIGHT");
    config->streams[0].rtmp.video_fps = value_int("RTMP_VIDEO_FPS");
    config->streams[0].rtmp.video_bitrate = value_int("RTMP_VIDEO_BITRATE");
    config->streams[0].rtmp.video_codec_name = value_string("RTMP_VIDEO_CODEC_NAME");
    config->streams[0].rtmp.encoder_name = value_string("RTMP_ENCODER_NAME");
    config->streams[0].gb28181.name = value_string("GB28181_NAME");
    config->streams[0].gb28181.server_ip = value_string("GB28181_SERVER_IP");
    config->streams[0].gb28181.server_port = value_int("GB28181_SERVER_PORT");
    config->streams[0].gb28181.server_domain = value_string("GB28181_SERVER_DOMAIN");
    config->streams[0].gb28181.server_id = value_string("GB28181_SERVER_ID");
    config->streams[0].gb28181.device_id = value_string("GB28181_DEVICE_ID");
    config->streams[0].gb28181.device_domain = value_string("GB28181_DEVICE_DOMAIN");
    config->streams[0].gb28181.device_password = value_string("GB28181_DEVICE_PASSWORD");
    config->streams[0].gb28181.bind_ip = value_string("GB28181_BIND_IP");
    config->streams[0].gb28181.local_sip_port = value_int("GB28181_LOCAL_SIP_PORT");
    config->streams[0].gb28181.sip_contact_ip = value_string("GB28181_CONTACT_IP");
    config->streams[0].gb28181.media_ip = value_string("GB28181_MEDIA_IP");
    config->streams[0].gb28181.media_port = value_int("GB28181_MEDIA_PORT");
    config->streams[0].gb28181.register_expires = value_int("GB28181_REGISTER_EXPIRES");
    config->streams[0].gb28181.keepalive_interval_sec = value_int("GB28181_KEEPALIVE_INTERVAL");
    config->streams[0].gb28181.register_retry_interval_sec = value_int("GB28181_REGISTER_RETRY_INTERVAL");
    config->streams[0].gb28181.device_name = value_string("GB28181_DEVICE_NAME");
    config->streams[0].gb28181.manufacturer = value_string("GB28181_MANUFACTURER");
    config->streams[0].gb28181.model = value_string("GB28181_MODEL");
    config->streams[0].gb28181.firmware = value_string("GB28181_FIRMWARE");
    config->streams[0].gb28181.channel_id = value_string("GB28181_CHANNEL_ID");
    config->streams[0].gb28181.user_agent = value_string("GB28181_USER_AGENT");
    config->streams[0].gb28181.queue_capacity = value_int("GB28181_QUEUE_CAPACITY");
}

int def_value_init(const char *config_path) {
    load_defaults();
    load_file_values(config_path);
    if (config_path && config_path[0] != '\0' && !file_has_key("GATEWAY_CONFIG_FILE_PATH")) {
        set_value("GATEWAY_CONFIG_FILE_PATH", config_path);
    }
    return 0;
}

void def_value_get_media_gateway_config(MediaGatewayConfig *config) {
    if (!config) {
        return;
    }

    *config = MediaGatewayConfig();
    config->enable_rtsp = value_int("GATEWAY_ENABLE_RTSP");
    config->enable_rtmp = value_int("GATEWAY_ENABLE_RTMP");
    config->enable_gb28181 = value_int("GATEWAY_ENABLE_GB28181");
    config->fps = value_int("GATEWAY_FPS");
    config->bitrate = value_int("GATEWAY_BITRATE");
    config->gop = value_int("GATEWAY_GOP");
    config->rc_mode = value_int("GATEWAY_RC_MODE");
    config->h264_profile = value_int("GATEWAY_H264_PROFILE");
    config->h264_level = value_int("GATEWAY_H264_LEVEL");
    config->h264_cabac_en = value_int("GATEWAY_H264_CABAC_EN");
    config->qp_init = value_int("GATEWAY_QP_INIT");
    config->qp_min = value_int("GATEWAY_QP_MIN");
    config->qp_max = value_int("GATEWAY_QP_MAX");
    config->qp_min_i = value_int("GATEWAY_QP_MIN_I");
    config->qp_max_i = value_int("GATEWAY_QP_MAX_I");
    config->qp_max_step = value_int("GATEWAY_QP_MAX_STEP");
    config->low_latency_mode = value_int("GATEWAY_LOW_LATENCY_MODE");
    config->stats_interval_sec = value_int("GATEWAY_STATS_INTERVAL_SEC");
    config->capture_retry_ms = value_int("GATEWAY_CAPTURE_RETRY_MS");
    config->max_consecutive_failures = value_int("GATEWAY_MAX_CONSECUTIVE_FAILURES");
    config->record_file_path = value_string("GATEWAY_RECORD_FILE_PATH");
    config->record_flush_interval_frames = value_int("GATEWAY_RECORD_FLUSH_INTERVAL_FRAMES");
    config->config_file_path = value_string("GATEWAY_CONFIG_FILE_PATH");
    config->bench_enable = value_int("GATEWAY_BENCH_ENABLE");
    config->bench_sample_every = value_int("GATEWAY_BENCH_SAMPLE_EVERY");
    config->bench_print_interval_sec = value_int("GATEWAY_BENCH_PRINT_INTERVAL_SEC");
    config->log_level = value_int("LOG_LEVEL");
    config->capture_source_count = value_int("GATEWAY_CAPTURE_SOURCE_COUNT");
    config->stream_count = value_int("GATEWAY_STREAM_COUNT");

    fill_capture_source(&config->capture_sources[0], "CAPTURE_MAIN_");
    fill_capture_source(&config->capture_sources[1], "CAPTURE_SUB_");
    fill_audio_source(&config->audio);
    fill_stream(&config->streams[0], "STREAM_MAIN_");
    fill_stream(&config->streams[1], "STREAM_SUB_");

    if (!file_has_key("STREAM_MAIN_ENABLE")) {
        fill_legacy_single_stream(config);
    }

    config->rtsp.name = value_string("RTSP_NAME");
    config->rtsp.session_name = value_string("RTSP_SESSION_NAME");
    config->rtsp.server_ip = value_string("RTSP_SERVER_IP");
    config->rtsp.server_port = value_int("RTSP_SERVER_PORT");
    config->rtsp.auth_enable = value_int("RTSP_AUTH_ENABLE");
    config->rtsp.user = value_string("RTSP_USER");
    config->rtsp.password = value_string("RTSP_PASSWORD");
    config->rtsp.queue_capacity = value_int("RTSP_QUEUE_CAPACITY");

    config->rtmp.name = value_string("RTMP_NAME");
    config->rtmp.publish_url = value_string("RTMP_PUBLISH_URL");
    config->rtmp.queue_capacity = value_int("RTMP_QUEUE_CAPACITY");
    config->rtmp.reconnect_interval_ms = value_int("RTMP_RECONNECT_INTERVAL_MS");
    config->rtmp.connect_timeout_ms = value_int("RTMP_CONNECT_TIMEOUT_MS");
    config->rtmp.audio_enabled = value_int("RTMP_AUDIO_ENABLED");
    config->rtmp.video_width = value_int("RTMP_VIDEO_WIDTH");
    config->rtmp.video_height = value_int("RTMP_VIDEO_HEIGHT");
    config->rtmp.video_fps = value_int("RTMP_VIDEO_FPS");
    config->rtmp.video_bitrate = value_int("RTMP_VIDEO_BITRATE");
    config->rtmp.video_codec_name = value_string("RTMP_VIDEO_CODEC_NAME");
    config->rtmp.encoder_name = value_string("RTMP_ENCODER_NAME");

    config->gb28181.name = value_string("GB28181_NAME");
    config->gb28181.server_ip = value_string("GB28181_SERVER_IP");
    config->gb28181.server_port = value_int("GB28181_SERVER_PORT");
    config->gb28181.server_domain = value_string("GB28181_SERVER_DOMAIN");
    config->gb28181.server_id = value_string("GB28181_SERVER_ID");
    config->gb28181.device_id = value_string("GB28181_DEVICE_ID");
    config->gb28181.device_domain = value_string("GB28181_DEVICE_DOMAIN");
    config->gb28181.device_password = value_string("GB28181_DEVICE_PASSWORD");
    config->gb28181.bind_ip = value_string("GB28181_BIND_IP");
    config->gb28181.local_sip_port = value_int("GB28181_LOCAL_SIP_PORT");
    config->gb28181.sip_contact_ip = value_string("GB28181_CONTACT_IP");
    config->gb28181.media_ip = value_string("GB28181_MEDIA_IP");
    config->gb28181.media_port = value_int("GB28181_MEDIA_PORT");
    config->gb28181.register_expires = value_int("GB28181_REGISTER_EXPIRES");
    config->gb28181.keepalive_interval_sec = value_int("GB28181_KEEPALIVE_INTERVAL");
    config->gb28181.register_retry_interval_sec = value_int("GB28181_REGISTER_RETRY_INTERVAL");
    config->gb28181.device_name = value_string("GB28181_DEVICE_NAME");
    config->gb28181.manufacturer = value_string("GB28181_MANUFACTURER");
    config->gb28181.model = value_string("GB28181_MODEL");
    config->gb28181.firmware = value_string("GB28181_FIRMWARE");
    config->gb28181.channel_id = value_string("GB28181_CHANNEL_ID");
    config->gb28181.user_agent = value_string("GB28181_USER_AGENT");
    config->gb28181.queue_capacity = value_int("GB28181_QUEUE_CAPACITY");
}

int def_value_loaded(void) {
    return g_loaded;
}

const char *def_value_source_path(void) {
    return g_source_path.c_str();
}
