/**
 * @file webRTCOutput.cpp
 * @brief 实现通用 mediaOutput 框架到 WebRTC 媒体端点和云端信令路由的适配。
 */
#include "webRTCOutput.h"

#include "webRTCDebug.h"
#include "webrtcCloudSignalingClient.h"
#include "webrtcRuntime.h"
#include "webrtcStreamEndpoint.h"
#include <exception>
#include <memory>
#include <string>

#include <future>
#include <cstring>
#include <inttypes.h>
#include <mutex>
#include <new>
#include <rtc/global.hpp>
#include <stdlib.h>
#include <string.h>

#include "commonDef.h"
#include "logger.h"

using rkmedia::webrtc::H264_FRAME_FORMAT_ANNEX_B;
using rkmedia::webrtc::WebRtcStreamEndpoint;
using rkmedia::webrtc::WebRtcStreamEndpointConfig;
using rkmedia::webrtc::WebRtcCloudSignalingClient;
using rkmedia::webrtc::WebRtcCloudStreamBinding;
using rkmedia::webrtc::web_rtc_debug_register_output;
using rkmedia::webrtc::web_rtc_debug_unregister_output;
using rkmedia::webrtc::WebRtcAudioCodec;
using rkmedia::webrtc::WebRtcAudioFrame;
using rkmedia::webrtc::WebRtcIncomingAudioPacket;
using rkmedia::webrtc::WebRtcVideoFrame;
using rkmedia::webrtc::WEBRTC_AUDIO_CODEC_NONE;
using rkmedia::webrtc::WEBRTC_AUDIO_CODEC_PCMA;
using rkmedia::webrtc::WEBRTC_AUDIO_CODEC_PCMU;
using rkmedia::webrtc::WEBRTC_AUDIO_CODEC_OPUS;

typedef struct {
    MediaOutputWebRtcConfig config; /* WebRTC 输出配置副本。 */
    std::shared_ptr<WebRtcStreamEndpoint> endpoint; /* 会话、PeerConnection 与媒体广播端点。 */
    std::shared_ptr<WebRtcCloudStreamBinding> cloudBinding; /* 本路输出在共享云端客户端中的回调。 */
    bool cloudStreamRegistered = false; /* stream_id 是否已登记到共享云端客户端。 */
} WebRtcOutputImpl;

static std::mutex g_rtc_lifecycle_mutex; /* 保护 libdatachannel 全局生命周期引用计数。 */
static int g_rtc_lifecycle_ref_count = 0; /* 当前持有 rtc::InitLogger/Preload 生命周期的输出数量。 */

/**
 * @brief 获取一次 libdatachannel 全局生命周期引用。
 *
 * 第一个 WebRTC 输出负责初始化全局资源，后续输出只增加引用计数。初始化抛出
 * 异常时不增加引用计数，调用方必须检查返回值并终止本次启动。
 *
 * @retval MEDIA_OK 全局资源已经可用，引用计数已增加。
 * @retval MEDIA_ERR_NO_MEMORY 初始化过程中发生内存分配失败。
 * @retval MEDIA_ERR 初始化过程中发生其他异常。
 */
static MediaResult webrtc_output_retain_rtc()
{
    std::lock_guard<std::mutex> lock(g_rtc_lifecycle_mutex);

    /* 只有第一个输出执行全局初始化，避免多个输出重复创建 libdatachannel 资源。 */
    if (g_rtc_lifecycle_ref_count == 0) {
        try {
            rtc::InitLogger(rtc::LogLevel::Info);
            rtc::Preload();
        } catch (const std::bad_alloc &e) {
            LOG_ERROR("[WEBRTC] rtc initialization failed: no memory: %s", e.what());
            return MEDIA_ERR_NO_MEMORY;
        } catch (const std::exception &e) {
            LOG_ERROR("[WEBRTC] rtc initialization failed: %s", e.what());
            return MEDIA_ERR;
        } catch (...) {
            LOG_ERROR("[WEBRTC] rtc initialization failed: unknown exception");
            return MEDIA_ERR;
        }
    }

    /* 初始化成功后才增加引用；失败路径保持计数不变。 */
    ++g_rtc_lifecycle_ref_count;
    return MEDIA_OK;
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

/** @brief 检查固定长度字符数组是否包含非空且以 '\0' 终止的字符串。 */
static bool webrtc_output_is_valid_string(const char *value, size_t capacity)
{
    if (!value || capacity == 0 || value[0] == '\0') {
        return false;
    }
    return memchr(value, '\0', capacity) != NULL;
}

/**
 * @brief 校验 WebRTC 输出配置
 * @param[in] config 待校验的单路 WebRTC 输出配置。
 * @return MEDIA_OK 表示配置有效；参数为空或配置字段非法时返回对应错误码。
 */
static MediaResult webrtc_output_validate_config(const MediaOutputWebRtcConfig *config)
{
    if (!config) {
        LOG_ERROR("[WEBRTC] output config validation failed: config is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!webrtc_output_is_valid_string(config->name, sizeof(config->name))) {
        LOG_ERROR("[WEBRTC] output config validation failed: name is empty or unterminated");
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (config->queue_capacity <= 0) {
        LOG_ERROR("[WEBRTC] output config validation failed: queue_capacity=%d",
                  config->queue_capacity);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (config->media.video_fps <= 0) {
        LOG_ERROR("[WEBRTC] output config validation failed: video_fps=%d",
                  config->media.video_fps);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (!config->runtime) {
        LOG_ERROR("[WEBRTC] output config validation failed: runtime is NULL name=%s",
                  config->name);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    if (!webrtc_output_is_valid_string(config->signaling.stream_id,
                                       sizeof(config->signaling.stream_id))) {
        LOG_ERROR("[WEBRTC] output config validation failed: stream_id is empty or unterminated");
        return MEDIA_ERR_INVALID_CONFIG;
    }

    /* 未启用音频时，采样率和声道字段不参与媒体协商，无需校验。 */
    if (config->media.audio.codec == MEDIA_CODEC_NONE) {
        return MEDIA_OK;
    }
    if (config->media.audio.sample_rate <= 0 || config->media.audio.channels <= 0) {
        LOG_ERROR("[WEBRTC] output config validation failed: audio codec=%d rate=%d channels=%d",
                  config->media.audio.codec,
                  config->media.audio.sample_rate,
                  config->media.audio.channels);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    if (config->media.audio.codec == MEDIA_CODEC_G711A || config->media.audio.codec == MEDIA_CODEC_G711U) {
        if (config->media.audio.sample_rate != 8000 || config->media.audio.channels != 1) {
            LOG_ERROR("[WEBRTC] output config validation failed: G711 requires 8000Hz mono, rate=%d channels=%d",
                      config->media.audio.sample_rate,
                      config->media.audio.channels);
            return MEDIA_ERR_INVALID_CONFIG;
        }
        return MEDIA_OK;
    }
    if (config->media.audio.codec == MEDIA_CODEC_OPUS) {
        if (config->media.audio.sample_rate != 48000 ||
            (config->media.audio.channels != 1 && config->media.audio.channels != 2)) {
            LOG_ERROR("[WEBRTC] output config validation failed: Opus requires 48000Hz mono/stereo, rate=%d channels=%d",
                      config->media.audio.sample_rate,
                      config->media.audio.channels);
            return MEDIA_ERR_INVALID_CONFIG;
        }
        return MEDIA_OK;
    }

    LOG_ERROR("[WEBRTC] output config validation failed: unsupported audio codec=%d",
              config->media.audio.codec);
    return MEDIA_ERR_UNSUPPORTED;
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

/**
 * @brief 把 WebRTC 会话内部音频编码转换为 mediaOutput 编码类型。
 * @param[in] codec WebRTC 会话内部音频编码。
 * @return 返回对应的 mediaOutput 编码；无法识别时返回 MEDIA_CODEC_NONE。
 */
static MediaCodecType webrtc_output_to_media_codec(WebRtcAudioCodec codec)
{
    if (codec == WEBRTC_AUDIO_CODEC_OPUS) {
        return MEDIA_CODEC_OPUS;
    }
    if (codec == WEBRTC_AUDIO_CODEC_PCMA) {
        return MEDIA_CODEC_G711A;
    }
    if (codec == WEBRTC_AUDIO_CODEC_PCMU) {
        return MEDIA_CODEC_G711U;
    }
    return MEDIA_CODEC_NONE;
}

/**
 * @brief 将浏览器上行音频包转换后交给 mediaOutput 业务回调。
 * @param[in] impl 当前 WebRTC 输出实现对象。
 * @param[in] packet 已完成 RTP 解析的上行音频包。
 * @note callbackPacket.payload 只在本函数调用期间有效，异步消费者必须立即复制。
 */
static void webrtc_output_handle_incoming_audio(
    WebRtcOutputImpl *impl,
    const WebRtcIncomingAudioPacket &packet)
{
    MediaOutputIncomingAudioPacket callbackPacket = {};

    if (!impl) {
        LOG_ERROR("[WEBRTC] incoming audio packet dropped: impl is NULL");
        return;
    }
    if (impl->config.media.audio.incoming_audio_callback == NULL) {
        LOG_DEBUG("[WEBRTC] incoming audio packet ignored: no callback registered session_id=%s codec=%d",
                  packet.sessionId.c_str(),
                  packet.codec);
        return;
    }

    /* 将 WebRTC 内部包描述转换为 C 侧统一的上行音频包描述。 */
    callbackPacket.session_id = packet.sessionId.c_str();
    callbackPacket.codec = webrtc_output_to_media_codec(packet.codec);
    callbackPacket.payload_type = packet.payloadType;
    callbackPacket.ssrc = packet.ssrc;
    callbackPacket.sequence_number = packet.sequenceNumber;
    callbackPacket.rtp_timestamp = packet.rtpTimestamp;
    callbackPacket.arrival_time_us = packet.arrivalTimeUs;
    callbackPacket.payload = packet.payload.data();
    callbackPacket.payload_size = packet.payload.size();

    /* 在 payload 有效期内同步交付；异步模块需要在回调内部完成数据复制。 */
    impl->config.media.audio.incoming_audio_callback(
        &callbackPacket,
        impl->config.media.audio.incoming_audio_user_data);
}

/**
 * @brief 根据 mediaOutput 配置构造媒体端点配置。
 * @param[in] impl 当前 WebRTC 输出实现对象。
 * @param[out] endpointConfig 待填充的媒体端点配置。
 * @retval MEDIA_OK 配置构造成功。
 * @retval MEDIA_ERR_INVALID_PARAM 参数为空。
 * @retval MEDIA_ERR_INVALID_CONFIG 共享 WebRTC runtime 缺失。
 */
static MediaResult webrtc_output_build_endpoint_config(
    WebRtcOutputImpl *impl,
    WebRtcStreamEndpointConfig *endpointConfig)
{
    std::weak_ptr<WebRtcCloudSignalingClient> weakClient;
    std::shared_ptr<WebRtcCloudSignalingClient> client;

    if (!impl || !endpointConfig) {
        LOG_ERROR("[WEBRTC] build endpoint config failed: impl=%p config=%p",
                  (void *)impl,
                  (void *)endpointConfig);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!impl->config.runtime) {
        LOG_ERROR("[WEBRTC] build endpoint config failed: runtime missing name=%s",
                  impl->config.name);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    /* 填充每个 PeerConnection 创建时都需要继承的媒体协商参数。 */
    endpointConfig->name = impl->config.name;
    endpointConfig->streamId = impl->config.signaling.stream_id;
    endpointConfig->signalingEndpoint = impl->config.runtime->cloudEndpoint;
    client = impl->config.runtime->cloudClient;
    weakClient = client;
    endpointConfig->signalingReadyCallback = [weakClient]() {
        std::shared_ptr<WebRtcCloudSignalingClient> callbackClient;

        callbackClient = weakClient.lock();
        return callbackClient && callbackClient->isReady();
    };
    endpointConfig->videoFps = static_cast<uint32_t>(impl->config.media.video_fps);
    endpointConfig->audioCodec = webrtc_output_to_audio_codec(impl->config.media.audio.codec);
    endpointConfig->audioSampleRate =
        static_cast<uint32_t>(impl->config.media.audio.sample_rate);
    endpointConfig->audioChannels =
        static_cast<uint32_t>(impl->config.media.audio.channels);
    /* ICE 配置来自网关级 runtime，本端点创建的所有会话共享该配置快照。 */
    endpointConfig->ice = impl->config.runtime->ice;

    /* 端点只依赖统一 C++ 回调，协议包到业务包的转换留在输出适配层。 */
    /* todo:每个通道的音频都接收吗 */
    endpointConfig->incomingAudioCallback =
        [impl](const WebRtcIncomingAudioPacket &packet) {
            webrtc_output_handle_incoming_audio(impl, packet);
        };
    return MEDIA_OK;
}

/**
 * @brief 创建并启动当前输出的媒体端点。
 * @param[in,out] impl 当前 WebRTC 输出实现对象；成功后保存已启动端点。
 * @return 成功返回 MEDIA_OK，失败返回具体错误码并保持 endpoint 为空。
 */
static MediaResult webrtc_output_start_endpoint(WebRtcOutputImpl *impl)
{
    WebRtcStreamEndpointConfig endpointConfig;
    std::shared_ptr<WebRtcStreamEndpoint> endpoint;
    MediaResult result = MEDIA_OK;

    if (!impl) {
        LOG_ERROR("[WEBRTC] start endpoint failed: impl is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }

    try {
        /* 先构造完整配置，配置失败时不创建任何运行时对象。 */
        result = webrtc_output_build_endpoint_config(impl, &endpointConfig);
        if (result != MEDIA_OK) {
            return result;
        }

        /* 使用局部对象完成创建和启动，成功前不写入 impl，避免暴露半启动端点。 */
        endpoint = std::make_shared<WebRtcStreamEndpoint>();
        if (!endpoint) {
            LOG_ERROR("[WEBRTC] start endpoint failed: allocation returned NULL name=%s",
                      impl->config.name);
            return MEDIA_ERR_NO_MEMORY;
        }
        if (!endpoint->start(endpointConfig)) {
            LOG_ERROR("[WEBRTC] start endpoint failed: endpoint start rejected name=%s",
                      impl->config.name);
            endpoint->stop();
            return MEDIA_ERR;
        }
    } catch (const std::bad_alloc &e) {
        LOG_ERROR("[WEBRTC] start endpoint failed: no memory name=%s error=%s",
                  impl->config.name,
                  e.what());
        if (endpoint) endpoint->stop();
        return MEDIA_ERR_NO_MEMORY;
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBRTC] start endpoint failed: name=%s error=%s",
                  impl->config.name,
                  e.what());
        if (endpoint) endpoint->stop();
        return MEDIA_ERR;
    } catch (...) {
        LOG_ERROR("[WEBRTC] start endpoint failed: name=%s unknown exception",
                  impl->config.name);
        if (endpoint) endpoint->stop();
        return MEDIA_ERR;
    }

    /* 端点完全启动后再发布到输出实现，后续信令才能通过它创建会话。 */
    impl->endpoint = endpoint;
    return MEDIA_OK;
}

/**
 * @brief 将当前媒体输出登记到共享云端信令客户端。
 * @param[in,out] impl 已成功创建媒体端点的 WebRTC 输出实现。
 * @retval MEDIA_OK 回调绑定及 stream_id 登记成功。
 * @retval MEDIA_ERR_INVALID_PARAM 运行时或媒体端点缺失。
 * @retval MEDIA_ERR_INVALID_CONFIG 云端客户端或 stream_id 不可用。
 * @retval MEDIA_ERR_NO_MEMORY 创建回调绑定时内存不足。
 * @retval MEDIA_ERR 云端拒绝登记或发生其他异常。
 */
static MediaResult webrtc_output_register_cloud_stream(WebRtcOutputImpl *impl)
{
    std::shared_ptr<WebRtcCloudStreamBinding> binding;
    std::weak_ptr<WebRtcStreamEndpoint> weakEndpoint;
    std::weak_ptr<WebRtcCloudSignalingClient> weakClient;
    std::shared_ptr<WebRtcCloudSignalingClient> client; /* 网关共享的云端信令客户端。 */
    std::string streamId;

    if (!impl || !impl->endpoint || !impl->config.runtime) {
        LOG_ERROR("[WEBRTC] register cloud stream failed: impl=%p endpoint=%p",
                  (void *)impl,
                  impl ? (void *)impl->endpoint.get() : NULL);
        return MEDIA_ERR_INVALID_PARAM;
    }
    client = impl->config.runtime->cloudClient;
    streamId = impl->config.signaling.stream_id;
    if (!client || streamId.empty()) {
        LOG_ERROR("[WEBRTC] register cloud stream failed: cloud client or stream_id missing name=%s",
                  impl->config.name);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    weakEndpoint = impl->endpoint;
    weakClient = client;
    try {
        binding = std::make_shared<WebRtcCloudStreamBinding>();
        /* 创建媒体会话执行的回调 */
        binding->openSession = [weakEndpoint, weakClient](const std::string &sessionId) {
            std::shared_ptr<WebRtcStreamEndpoint> endpoint;

            endpoint = weakEndpoint.lock();
            if (!endpoint) {
                LOG_ERROR("[WEBRTC] open session failed: endpoint released session=%s",
                          sessionId.c_str());
                return false;
            }
            return endpoint->openSession(
                sessionId,
                [weakClient, sessionId](const std::string &message) {
                    std::shared_ptr<WebRtcCloudSignalingClient> cloudClient;

                    cloudClient = weakClient.lock();
                    return cloudClient && cloudClient->sendSessionMessage(sessionId, message);
                },
                [weakClient, sessionId](const char *reason) {
                    std::shared_ptr<WebRtcCloudSignalingClient> cloudClient;

                    cloudClient = weakClient.lock();
                    if (cloudClient) {
                        cloudClient->closeSession(sessionId, reason);
                    }
                });
        };
        /* 收到会话信令执行的回调 */
        binding->deliverMessage =
            [weakEndpoint](const std::string &sessionId, const std::string &message) {
                std::shared_ptr<WebRtcStreamEndpoint> endpoint;

                endpoint = weakEndpoint.lock();
                if (endpoint) {
                    endpoint->handleSessionSignalingMessage(sessionId, message);
                }
            };
        binding->closeSession = [weakEndpoint](const std::string &sessionId) {
            std::shared_ptr<WebRtcStreamEndpoint> endpoint;

            endpoint = weakEndpoint.lock();
            if (endpoint) {
                endpoint->handleSessionSignalingClosed(sessionId);
            }
        };
    } catch (const std::bad_alloc &e) {
        LOG_ERROR("[WEBRTC] register cloud stream failed: no memory name=%s error=%s",
                  impl->config.name, e.what());
        return MEDIA_ERR_NO_MEMORY;
    } catch (const std::exception &e) {
        LOG_ERROR("[WEBRTC] register cloud stream failed: name=%s error=%s",
                  impl->config.name, e.what());
        return MEDIA_ERR;
    }
    if (!client->addStream(streamId, binding)) {
        LOG_ERROR("[WEBRTC] register cloud stream failed: registration rejected name=%s stream=%s",
                  impl->config.name, streamId.c_str());
        return MEDIA_ERR;
    }
    impl->cloudBinding = binding;
    impl->cloudStreamRegistered = true;
    return MEDIA_OK;
}

/** @brief 注销云端码流并释放启动过程中创建的媒体端点。 */
static void webrtc_output_release_objects(WebRtcOutputImpl *impl)
{
    if (!impl) return;

    /* 先注销云端码流，确保端点销毁期间不会再路由新的会话请求。 */
    if (impl->cloudStreamRegistered && impl->config.runtime &&
        impl->config.runtime->cloudClient) {
        impl->config.runtime->cloudClient->removeStream(impl->config.signaling.stream_id);
        impl->cloudStreamRegistered = false;
        impl->cloudBinding.reset();
    }

    /* 云端路由移除后再停止媒体端点及其全部 PeerConnection。 */
    if (impl->endpoint) {
        impl->endpoint->stop();
        impl->endpoint.reset();
    }
}

/**
 * @brief 回滚一次已经获取 RTC 全局引用的 WebRTC 输出启动。
 * @param[in,out] impl 当前 WebRTC 输出实现对象。
 * @pre 调用前必须已经成功执行 webrtc_output_retain_rtc()。
 */
static void webrtc_output_rollback_start(WebRtcOutputImpl *impl)
{
    /* 严格按照启动的相反顺序撤销局部对象和全局 RTC 引用。 */
    webrtc_output_release_objects(impl);
    webrtc_output_release_rtc();
}

/**
 * @brief 校验启动入口参数并取得 WebRTC 输出实现对象。
 * @param[in] output 通用媒体输出对象。
 * @param[out] impl 返回 output 持有的 WebRTC 输出实现对象。
 * @retval MEDIA_OK 参数和共享 runtime 有效。
 * @retval MEDIA_ERR_INVALID_PARAM output、output->impl 或 impl 输出参数为空。
 * @retval MEDIA_ERR_INVALID_CONFIG 共享 WebRTC runtime 缺失。
 */
static MediaResult webrtc_output_validate_start_context(
    MediaOutput *output,
    WebRtcOutputImpl **impl)
{
    WebRtcOutputImpl *outputImpl = NULL;

    if (!impl) {
        LOG_ERROR("[WEBRTC] output start validation failed: impl output is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    *impl = NULL;
    if (!output || !output->impl) {
        LOG_ERROR("[WEBRTC] output start validation failed: output=%p impl=%p",
                  (void *)output,
                  output ? output->impl : NULL);
        return MEDIA_ERR_INVALID_PARAM;
    }

    outputImpl = (WebRtcOutputImpl *)output->impl;
    if (!outputImpl->config.runtime) {
        LOG_ERROR("[WEBRTC] output start validation failed: shared runtime missing name=%s",
                  outputImpl->config.name);
        return MEDIA_ERR_INVALID_CONFIG;
    }

    *impl = outputImpl;
    return MEDIA_OK;
}

/**
 * @brief 启动单路 WebRTC 输出。
 * @param[in,out] output 通用媒体输出对象。
 * @return 成功返回 MEDIA_OK；失败返回参数、配置、内存或运行时错误码。
 *
 * 按“RTC 全局资源、媒体端点、云端码流登记、调试入口”的顺序启动。媒体端点创建
 * 或云端码流登记失败时按相反顺序回滚，避免对外暴露半启动状态。
 */
static int webrtc_output_start(MediaOutput *output)
{
    WebRtcOutputImpl *impl = NULL;
    MediaResult result = MEDIA_OK;

    /* 阶段一：校验 C 侧输出对象及其共享 runtime，取得内部实现。 */
    result = webrtc_output_validate_start_context(output, &impl);
    if (result != MEDIA_OK) {
        return result;
    }

    /* 重复启动保持幂等，不重复增加 RTC 全局资源引用。 */
    if (impl->endpoint) {
        LOG_INFO("[WEBRTC] output start ignored: endpoint already started name=%s",
                 impl->config.name);
        return MEDIA_OK;
    }

    /* 阶段二：获取进程级 RTC 资源引用；失败时尚未创建任何输出对象。 */
    result = webrtc_output_retain_rtc();
    if (result != MEDIA_OK) {
        LOG_ERROR("[WEBRTC] output start failed: retain RTC name=%s result=%d",
                  impl->config.name,
                  result);
        return result;
    }

    /* 阶段三：创建媒体端点，使其具备管理会话和收发媒体的能力。 */
    result = webrtc_output_start_endpoint(impl);
    if (result != MEDIA_OK) {
        webrtc_output_rollback_start(impl);
        return result;
    }

    /* 阶段四：最后登记云端码流，避免云端请求进入尚未就绪的媒体端点。 */
    result = webrtc_output_register_cloud_stream(impl);
    if (result != MEDIA_OK) {
        webrtc_output_rollback_start(impl);
        return result;
    }

    /* 阶段五：注册只读调试入口；该入口不是媒体服务的必要条件。 */
    if (web_rtc_debug_register_output(impl->endpoint.get()) != MEDIA_OK) {
        LOG_WARN("[WEBRTC] debug command not registered name=%s", impl->config.name);
    }

    return MEDIA_OK;
}

/*
 * WebRTC 的 protocol_ready 状态：WebRtcStreamEndpoint 已由 start 回调创建，
 * 输出框架可以把媒体包交给端点进行会话扇出。该状态不表示已有浏览器连接，
 * 也不表示 PeerConnection、ICE、DTLS 或媒体 Track 已经就绪；这些状态由每个
 * WebRtcSession 独立维护。没有可发送会话时，端点会正常记录 no_ready 统计。
 */
static int webrtc_output_connect(MediaOutput *output)
{
    WebRtcOutputImpl *impl = NULL;

    if (!output || !output->impl) {
        LOG_ERROR("[WEBRTC] protocol ready check failed: output=%p impl=%p",
                  (void *)output,
                  output ? output->impl : NULL);
        return MEDIA_ERR_INVALID_PARAM;
    }

    impl = (WebRtcOutputImpl *)output->impl;
    if (!impl->endpoint) {
        LOG_ERROR("[WEBRTC] protocol ready check failed: endpoint is NULL name=%s",
                  impl->config.name);
        return MEDIA_ERR_NOT_READY;
    }
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
    if (!impl->endpoint) {
        LOG_WARN("[WEBRTC] send packet failed: endpoint not ready name=%s", impl->config.name);
        return MEDIA_ERR_NOT_READY;
    }
    if (packet->frame_type == MEDIA_FRAME_TYPE_VIDEO && packet->codec == MEDIA_CODEC_H264) {
        frame.data = packet->buffer->data;
        frame.size = packet->buffer->size;
        frame.ptsUs = packet->pts_us;
        frame.keyFrame = packet->is_key_frame ? true : false;
        frame.format = H264_FRAME_FORMAT_ANNEX_B;
        sentCount = impl->endpoint->sendVideoFrame(frame);
    } else if (packet->frame_type == MEDIA_FRAME_TYPE_AUDIO &&
               (packet->codec == MEDIA_CODEC_G711A || packet->codec == MEDIA_CODEC_G711U ||
                packet->codec == MEDIA_CODEC_OPUS)) {
        audioFrame.data = packet->buffer->data;
        audioFrame.size = packet->buffer->size;
        audioFrame.ptsUs = packet->pts_us;
        audioFrame.codec = webrtc_output_to_audio_codec(packet->codec);
        audioFrame.frameId = packet->frame_id;
        audioFrame.traceSample = packet->path_metrics.sample != 0;
        sentCount = impl->endpoint->sendAudioFrame(audioFrame);
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
 * 将 WebRtcStreamEndpoint 合并后的 IDR 请求暴露给通用 mediaOutput 层。
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
    if (!impl->endpoint) {
        return 0;
    }
    return impl->endpoint->consumeVideoKeyframeRequest() ? 1 : 0;
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
    if (impl->endpoint || impl->cloudStreamRegistered) {
        if (impl->endpoint) web_rtc_debug_unregister_output(impl->endpoint.get());
        /* 先注销云端码流，再销毁会话端点，防止停止期间创建新会话。 */
        webrtc_output_release_objects(impl);
        webrtc_output_release_rtc();
    }
}

/* 查询 WebRTC RTP 音频发送和 SDP 协商当前支持的编码集合。 */
static int webrtc_output_get_audio_capabilities(const MediaOutput *output,
                                                MediaOutputAudioCapabilities *capabilities)
{
    (void)output;

    if (!capabilities) {
        LOG_ERROR("[WEBRTC] get audio capabilities failed: capabilities is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    capabilities->codec_mask = MEDIA_OUTPUT_AUDIO_CODEC_MASK(MEDIA_CODEC_G711A) |
                               MEDIA_OUTPUT_AUDIO_CODEC_MASK(MEDIA_CODEC_G711U) |
                               MEDIA_OUTPUT_AUDIO_CODEC_MASK(MEDIA_CODEC_OPUS);
    capabilities->count = 3;
    capabilities->entries[0].codec = MEDIA_CODEC_G711A;
    capabilities->entries[0].sample_rate = 8000;
    capabilities->entries[0].min_channels = 1;
    capabilities->entries[0].max_channels = 1;
    capabilities->entries[1].codec = MEDIA_CODEC_G711U;
    capabilities->entries[1].sample_rate = 8000;
    capabilities->entries[1].min_channels = 1;
    capabilities->entries[1].max_channels = 1;
    capabilities->entries[2].codec = MEDIA_CODEC_OPUS;
    capabilities->entries[2].sample_rate = 48000;
    capabilities->entries[2].min_channels = 1;
    capabilities->entries[2].max_channels = 2;
    return MEDIA_OK;
}

static const MediaOutputVTable g_webrtc_output_vtable = {
    webrtc_output_start,
    webrtc_output_connect,
    webrtc_output_send_packet,
    webrtc_output_disconnect,
    webrtc_output_stop,
    NULL,
    NULL,
    webrtc_output_get_audio_capabilities
};

/*
 * 初始化 WebRTC mediaOutput backend。
 * 对外仍然只通过 mediaOutput.h 暴露，WebRTC 私有类不出现在 C 接口中。
 */
int media_output_setup_webrtc(MediaOutput *output, const MediaOutputWebRtcConfig *config)
{
    WebRtcOutputImpl *impl;
    MediaOutputChannelConfig channelConfig;
    MediaResult result = MEDIA_OK;

    if (!output || !config) {
        LOG_ERROR("media_output_setup_webrtc failed: output=%p config=%p",
                  (void *)output,
                  (const void *)config);
        return MEDIA_ERR_INVALID_PARAM;
    }

    result = webrtc_output_validate_config(config);
    if (result != MEDIA_OK) {
        LOG_ERROR("media_output_setup_webrtc failed: invalid config result=%d", result);
        return result;
    }
    /* 分配webRTC私有参数 */
    impl = new (std::nothrow) WebRtcOutputImpl();
    if (!impl) {
        LOG_ERROR("media_output_setup_webrtc failed: alloc impl");
        return MEDIA_ERR_NO_MEMORY;
    }
    /* 校验成功后按值保存原始配置 */
    impl->config = *config;

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
        delete impl;
        return MEDIA_ERR;
    }

    output->type = MEDIA_OUTPUT_TYPE_WEBRTC;
    return MEDIA_OK;
}
