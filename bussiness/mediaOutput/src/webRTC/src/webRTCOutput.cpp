#include "../inc/webRTCOutput.h"

#include "../inc/webRTCDebug.h"
#include "../inc/webrtcServer.h"

#include <future>
#include <inttypes.h>
#include <mutex>
#include <new>
#include <rtc/global.hpp>
#include <stdlib.h>
#include <string.h>

#include "commonDef.h"
#include "logger.h"

#define DEFAULT_WEBRTC_OUTPUT_NAME "webrtc"
#define DEFAULT_WEBRTC_BIND_ADDRESS "0.0.0.0"
#define DEFAULT_WEBRTC_PORT 8000
#define DEFAULT_WEBRTC_QUEUE_CAPACITY 32
#define DEFAULT_WEBRTC_VIDEO_FPS 30
#define DEFAULT_WEBRTC_AUDIO_SAMPLE_RATE 8000
#define DEFAULT_WEBRTC_AUDIO_CHANNELS 1

using rkmedia::webrtc::H264_FRAME_FORMAT_ANNEX_B;
using rkmedia::webrtc::WebRtcServer;
using rkmedia::webrtc::WebRtcServerConfig;
using rkmedia::webrtc::web_rtc_debug_register_server;
using rkmedia::webrtc::web_rtc_debug_unregister_server;
using rkmedia::webrtc::WebRtcAudioCodec;
using rkmedia::webrtc::WebRtcAudioFrame;
using rkmedia::webrtc::WebRtcVideoFrame;
using rkmedia::webrtc::WEBRTC_AUDIO_CODEC_NONE;
using rkmedia::webrtc::WEBRTC_AUDIO_CODEC_PCMA;
using rkmedia::webrtc::WEBRTC_AUDIO_CODEC_PCMU;
using rkmedia::webrtc::WEBRTC_AUDIO_CODEC_OPUS;

typedef struct {
    MediaOutputWebRtcConfig config; /* WebRTC 输出配置副本。 */
    WebRtcServer *server;           /* C++ WebRTC 服务对象。 */
} WebRtcOutputImpl;

static std::mutex g_rtc_lifecycle_mutex; /* 保护 libdatachannel 全局生命周期引用计数。 */
static int g_rtc_lifecycle_ref_count = 0; /* 当前持有 rtc::InitLogger/Preload 生命周期的输出数量。 */

/* 安全复制字符串到固定长度缓冲区，保证以 '\0' 结束。 */
static void copy_string_field(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/*
 * 初始化 libdatachannel 全局资源。
 * 多个 WebRTC output 共享同一套 rtc 全局生命周期，用引用计数避免重复 Cleanup。
 */
static void webrtc_output_retain_rtc()
{
    std::lock_guard<std::mutex> lock(g_rtc_lifecycle_mutex);

    if (g_rtc_lifecycle_ref_count == 0) {
        rtc::InitLogger(rtc::LogLevel::Info);
        rtc::Preload();
    }
    ++g_rtc_lifecycle_ref_count;
}

/* 释放 libdatachannel 全局资源，最后一个 WebRTC output 停止时调用 Cleanup。 */
static void webrtc_output_release_rtc()
{
    std::lock_guard<std::mutex> lock(g_rtc_lifecycle_mutex);

    if (g_rtc_lifecycle_ref_count <= 0) {
        LOG_WARN("[WEBRTC] rtc cleanup ignored: ref_count=%d", g_rtc_lifecycle_ref_count);
        return;
    }
    --g_rtc_lifecycle_ref_count;
    if (g_rtc_lifecycle_ref_count == 0) {
        rtc::Cleanup().wait();
    }
}

/* 填充 WebRTC 输出默认配置，避免上层没有配置可选字段时启动失败。 */
static void webrtc_output_normalize_config(MediaOutputWebRtcConfig *dst,
                                           const MediaOutputWebRtcConfig *src)
{
    memset(dst, 0, sizeof(*dst));
    copy_string_field(dst->name, sizeof(dst->name), DEFAULT_WEBRTC_OUTPUT_NAME);
    copy_string_field(dst->bind_address, sizeof(dst->bind_address), DEFAULT_WEBRTC_BIND_ADDRESS);
    dst->port = DEFAULT_WEBRTC_PORT;
    dst->queue_capacity = DEFAULT_WEBRTC_QUEUE_CAPACITY;
    dst->video_fps = DEFAULT_WEBRTC_VIDEO_FPS;
    dst->audio_codec = MEDIA_CODEC_NONE;
    dst->audio_sample_rate = DEFAULT_WEBRTC_AUDIO_SAMPLE_RATE;
    dst->audio_channels = DEFAULT_WEBRTC_AUDIO_CHANNELS;

    if (!src) {
        return;
    }
    if (src->name[0] != '\0') {
        copy_string_field(dst->name, sizeof(dst->name), src->name);
    }
    if (src->bind_address[0] != '\0') {
        copy_string_field(dst->bind_address, sizeof(dst->bind_address), src->bind_address);
    }
    if (src->port > 0) {
        dst->port = src->port;
    }
    if (src->queue_capacity > 0) {
        dst->queue_capacity = src->queue_capacity;
    }
    if (src->video_fps > 0) {
        dst->video_fps = src->video_fps;
    }
    if (src->audio_codec == MEDIA_CODEC_G711A || src->audio_codec == MEDIA_CODEC_G711U ||
        src->audio_codec == MEDIA_CODEC_OPUS) {
        dst->audio_codec = src->audio_codec;
    }
    if (src->audio_sample_rate > 0) {
        dst->audio_sample_rate = src->audio_sample_rate;
    }
    if (src->audio_channels > 0) {
        dst->audio_channels = src->audio_channels;
    }
    if ((dst->audio_codec == MEDIA_CODEC_G711A || dst->audio_codec == MEDIA_CODEC_G711U) &&
        (dst->audio_sample_rate != 8000 || dst->audio_channels != 1)) {
        LOG_WARN("[WEBRTC] disable audio: codec=%d rate=%d channels=%d, only G711 8000Hz mono is supported",
                 dst->audio_codec,
                 dst->audio_sample_rate,
                 dst->audio_channels);
        dst->audio_codec = MEDIA_CODEC_NONE;
    }
    if (dst->audio_codec == MEDIA_CODEC_OPUS &&
        (dst->audio_sample_rate != 48000 ||
         (dst->audio_channels != 1 && dst->audio_channels != 2))) {
        LOG_WARN("[WEBRTC] disable Opus: rate=%d channels=%d, expected 48000Hz mono/stereo",
                 dst->audio_sample_rate, dst->audio_channels);
        dst->audio_codec = MEDIA_CODEC_NONE;
    }
}

/* 把 mediaOutput 的音频编码类型转换为 WebRTC 会话内部编码类型。 */
static WebRtcAudioCodec webrtc_output_to_audio_codec(MediaCodecType codec)
{
    if (codec == MEDIA_CODEC_G711A) {
        return WEBRTC_AUDIO_CODEC_PCMA;
    }
    if (codec == MEDIA_CODEC_G711U) {
        return WEBRTC_AUDIO_CODEC_PCMU;
    }
    if (codec == MEDIA_CODEC_OPUS) {
        return WEBRTC_AUDIO_CODEC_OPUS;
    }
    return WEBRTC_AUDIO_CODEC_NONE;
}

/* 启动 WebRTC 输出服务，实际监听 WebSocket 信令端口。 */
static int webrtc_output_start(MediaOutput *output)
{
    WebRtcOutputImpl *impl;
    WebRtcServerConfig config;

    if (!output || !output->impl) {
        LOG_ERROR("[WEBRTC] output start failed: output=%p impl=%p",
                  (void *)output,
                  output ? output->impl : NULL);
        return MEDIA_ERR_INVALID_PARAM;
    }

    impl = (WebRtcOutputImpl *)output->impl;
    if (impl->server) {
        LOG_INFO("[WEBRTC] output start ignored: server already started name=%s", impl->config.name);
        return MEDIA_OK;
    }

    webrtc_output_retain_rtc();
    impl->server = new (std::nothrow) WebRtcServer();
    if (!impl->server) {
        LOG_ERROR("[WEBRTC] output start failed: alloc WebRtcServer name=%s", impl->config.name);
        webrtc_output_release_rtc();
        return MEDIA_ERR_NO_MEMORY;
    }
    config.name = impl->config.name;
    config.bindAddress = impl->config.bind_address;
    config.port = static_cast<uint16_t>(impl->config.port);
    config.videoFps = static_cast<uint32_t>(impl->config.video_fps);
    config.audioCodec = webrtc_output_to_audio_codec(impl->config.audio_codec);
    config.audioSampleRate = static_cast<uint32_t>(impl->config.audio_sample_rate);
    config.audioChannels = static_cast<uint32_t>(impl->config.audio_channels);

    if (!impl->server->start(config)) {
        LOG_ERROR("[WEBRTC] output start failed: bind=%s port=%d",
                  impl->config.bind_address,
                  impl->config.port);
        delete impl->server;
        impl->server = NULL;
        webrtc_output_release_rtc();
        return MEDIA_ERR;
    }
    if (web_rtc_debug_register_server(impl->server) != MEDIA_OK) {
        LOG_WARN("[WEBRTC] debug command not registered name=%s", impl->config.name);
    }

    return MEDIA_OK;
}

/*
 * WebRTC 是监听型输出，connect 不需要主动拨号。
 * 浏览器是否已连接由 WebRtcServer 内部 session 管理。
 */
static int webrtc_output_connect(MediaOutput *output)
{
    (void)output;
    return MEDIA_OK;
}

/* 发送一个媒体包到所有已连接且视频 Track 就绪的浏览器会话。 */
static int webrtc_output_send_packet(MediaOutput *output, const MediaPacket *packet)
{
    WebRtcOutputImpl *impl;
    WebRtcVideoFrame frame;
    WebRtcAudioFrame audioFrame;
    int sentCount;

    if (!output || !output->impl || !packet || !packet->buffer) {
        LOG_ERROR("[WEBRTC] send packet failed: output=%p impl=%p packet=%p buffer=%p",
                  (void *)output,
                  output ? output->impl : NULL,
                  (const void *)packet,
                  packet ? (void *)packet->buffer : NULL);
        return MEDIA_ERR_INVALID_PARAM;
    }
    impl = (WebRtcOutputImpl *)output->impl;
    if (!impl->server) {
        LOG_WARN("[WEBRTC] send packet failed: server not ready name=%s", impl->config.name);
        return MEDIA_ERR_NOT_READY;
    }
    if (packet->frame_type == MEDIA_FRAME_TYPE_VIDEO && packet->codec == MEDIA_CODEC_H264) {
        frame.data = packet->buffer->data;
        frame.size = packet->buffer->size;
        frame.ptsUs = packet->pts_us;
        frame.keyFrame = packet->is_key_frame ? true : false;
        frame.format = H264_FRAME_FORMAT_ANNEX_B;
        sentCount = impl->server->sendVideoFrame(frame);
    } else if (packet->frame_type == MEDIA_FRAME_TYPE_AUDIO &&
               (packet->codec == MEDIA_CODEC_G711A || packet->codec == MEDIA_CODEC_G711U ||
                packet->codec == MEDIA_CODEC_OPUS)) {
        audioFrame.data = packet->buffer->data;
        audioFrame.size = packet->buffer->size;
        audioFrame.ptsUs = packet->pts_us;
        audioFrame.codec = webrtc_output_to_audio_codec(packet->codec);
        sentCount = impl->server->sendAudioFrame(audioFrame);
    } else {
        LOG_DEBUG("[WEBRTC] packet ignored: frame_type=%d codec=%d",
                  packet->frame_type,
                  packet->codec);
        return MEDIA_OK;
    }
    if (sentCount <= 0) {
        /*
         * 没有浏览器观看时不算发送失败。
         * 这样 mediaOutput 不会因为暂时无人连接而进入重连/等待关键帧状态。
         */
        LOG_DEBUG("[WEBRTC] frame not sent: no ready browser session frame_id=%" PRIu64 " type=%d",
                  packet->frame_id,
                  packet->frame_type);
        return MEDIA_OK;
    }

    return MEDIA_OK;
}

/* WebRTC 监听型输出没有下游拨号连接，断开由 session 自己处理。 */
static void webrtc_output_disconnect(MediaOutput *output)
{
    (void)output;
}

/*
 * 将 WebRtcServer 合并后的 IDR 请求暴露给通用 mediaOutput 层。
 * 真正调用 MPP 编码器由 mediaGateway 统一完成，WebRTC 模块不直接依赖编码器实现。
 */
int media_output_webrtc_consume_external_idr_request(MediaOutput *output)
{
    WebRtcOutputImpl *impl;

    if (!output || output->type != MEDIA_OUTPUT_TYPE_WEBRTC || !output->impl) {
        LOG_ERROR("[WEBRTC] consume IDR request failed: output=%p type=%d impl=%p",
                  (void *)output,
                  output ? output->type : -1,
                  output ? output->impl : NULL);
        return 0;
    }
    impl = (WebRtcOutputImpl *)output->impl;
    if (!impl->server) {
        return 0;
    }
    return impl->server->consumeVideoKeyframeRequest() ? 1 : 0;
}

/* 停止 WebRTC 输出服务并释放 C++ 对象。 */
static void webrtc_output_stop(MediaOutput *output)
{
    WebRtcOutputImpl *impl;

    if (!output || !output->impl) {
        LOG_WARN("[WEBRTC] output stop ignored: output=%p impl=%p",
                 (void *)output,
                 output ? output->impl : NULL);
        return;
    }

    impl = (WebRtcOutputImpl *)output->impl;
    if (impl->server) {
        web_rtc_debug_unregister_server(impl->server);
        impl->server->stop();
        delete impl->server;
        impl->server = NULL;
        webrtc_output_release_rtc();
    }
}

static const MediaOutputVTable g_webrtc_output_vtable = {
    webrtc_output_start,
    webrtc_output_connect,
    webrtc_output_send_packet,
    webrtc_output_disconnect,
    webrtc_output_stop,
    NULL,
    NULL
};

/*
 * 初始化 WebRTC mediaOutput backend。
 * 对外仍然只通过 mediaOutput.h 暴露，WebRTC 私有类不出现在 C 接口中。
 */
int media_output_setup_webrtc(MediaOutput *output, const MediaOutputWebRtcConfig *config)
{
    WebRtcOutputImpl *impl;
    MediaOutputChannelConfig channelConfig;

    if (!output || !config) {
        LOG_ERROR("media_output_setup_webrtc failed: output=%p config=%p",
                  (void *)output,
                  (const void *)config);
        return MEDIA_ERR_INVALID_PARAM;
    }

    impl = (WebRtcOutputImpl *)calloc(1, sizeof(WebRtcOutputImpl));
    if (!impl) {
        LOG_ERROR("media_output_setup_webrtc failed: alloc impl");
        return MEDIA_ERR_NO_MEMORY;
    }
    webrtc_output_normalize_config(&impl->config, config);

    memset(&channelConfig, 0, sizeof(channelConfig));
    channelConfig.name = impl->config.name;
    channelConfig.queue_capacity = impl->config.queue_capacity;
    channelConfig.reconnect_interval_ms = 1000;
    channelConfig.drop_until_keyframe_after_reconnect = 0;
    channelConfig.feedback_holder = NULL;

    output->type = MEDIA_OUTPUT_TYPE_WEBRTC;
    if (media_output_init(output, &channelConfig, &g_webrtc_output_vtable, impl) != MEDIA_OK) {
        LOG_ERROR("media_output_setup_webrtc failed: media_output_init name=%s queue=%d",
                  impl->config.name,
                  impl->config.queue_capacity);
        free(impl);
        return MEDIA_ERR;
    }

    output->type = MEDIA_OUTPUT_TYPE_WEBRTC;
    return MEDIA_OK;
}
