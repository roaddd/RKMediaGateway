/**
 * @file webrtcRuntime.cpp
 * @brief 实现 WebRTC 输出共享运行时的校验、凭据加载和生命周期管理。
 */
#include "webrtcRuntime.h"

#include "mediaOutput.h"
#include "webrtcCloudSignalingClient.h"

#include <exception>
#include <fstream>
#include <memory>
#include <new>
#include <string>

#include "logger.h"

using rkmedia::webrtc::WebRtcCloudConnectionConfig;
using rkmedia::webrtc::WebRtcCloudSignalingClient;

/**
 * @brief 从独立私有文件读取一行凭据。
 * @param[in] path 私有凭据文件路径，不能为空。
 * @param[out] secret 返回去除行尾回车后的凭据内容。
 * @retval true 成功读取长度为 1～4096 字节的凭据。
 * @retval false 路径无效、文件不可读或凭据长度非法。
 * @note 函数不会把凭据内容写入日志，文件只读取第一行。
 */
static bool webrtc_runtime_read_private_secret(const char *path, std::string &secret)
{
    std::ifstream file;
    std::string line;

    /* 先清空调用方原有内容，保证任意失败路径都不会保留旧凭据。 */
    secret.clear();
    if (!path || !path[0]) {
        LOG_ERROR("[WEBRTC] private secret path is empty");
        return false;
    }
    /* 使用二进制模式读取第一行，避免不同平台自动转换换行符。 */
    file.open(path, std::ios::in | std::ios::binary);
    if (!file.is_open() || !std::getline(file, line)) {
        LOG_ERROR("[WEBRTC] private secret file cannot be read: %s", path);
        return false;
    }
    /* getline 已去除 '\n'；兼容 CRLF 文件时还需要主动去除末尾 '\r'。 */
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    /* 拒绝空凭据和异常超长内容，防止把错误文件作为凭据加载。 */
    if (line.empty() || line.size() > 4096) {
        LOG_ERROR("[WEBRTC] private secret length is invalid: %s", path);
        return false;
    }

    /* 凭据内容不写日志，仅转移到本次进程持有的运行时配置。 */
    secret.swap(line);
    return true;
}

/**
 * @brief 严格校验共享 ICE 与云端信令配置。
 * @param[in] config 待校验的 WebRTC 共享配置。
 * @retval MEDIA_OK 配置完整且字段取值合法。
 * @retval MEDIA_ERR_INVALID_PARAM config 为空。
 * @retval MEDIA_ERR_INVALID_CONFIG ICE、TURN 或云端信令字段不合法。
 * @note 本函数只校验，不填充缺省值，也不纠正非法配置。
 */
static MediaResult webrtc_runtime_validate_config(const MediaOutputWebRtcSharedConfig *config)
{
    if (!config) {
        LOG_ERROR("[WEBRTC] runtime config validation failed: config is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    /* relay_only 来自 C 配置结构，只接受明确的 0 或 1。 */
    if (config->ice.relay_only != 0 && config->ice.relay_only != 1) {
        LOG_ERROR("[WEBRTC] runtime config validation failed: relay_only=%d",
                  config->ice.relay_only);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    /* 配置 STUN 主机后必须同时提供有效 UDP 端口。 */
    if (config->ice.stun.host[0] &&
        (config->ice.stun.port <= 0 || config->ice.stun.port > 65535)) {
        LOG_ERROR("[WEBRTC] runtime config validation failed: STUN port=%d",
                  config->ice.stun.port);
        return MEDIA_ERR_INVALID_CONFIG;
    }
    /* TURN 启用时，地址、端口、用户名和密码文件必须作为一组完整提供。 */
    if (config->ice.turn.host[0] &&
        (config->ice.turn.port <= 0 || config->ice.turn.port > 65535 ||
         !config->ice.turn.username[0] || !config->ice.turn.password_file[0])) {
        LOG_ERROR("[WEBRTC] runtime config validation failed: incomplete TURN configuration");
        return MEDIA_ERR_INVALID_CONFIG;
    }
    /* 仅中继模式无法回退到 host/srflx，因此必须配置 TURN。 */
    if (config->ice.relay_only && !config->ice.turn.host[0]) {
        LOG_ERROR("[WEBRTC] runtime config validation failed: relay_only requires TURN");
        return MEDIA_ERR_INVALID_CONFIG;
    }
    /* 云端是唯一信令入口，设备长连接参数必须始终完整。 */
    if (!config->cloud.url[0] || !config->cloud.device_id[0] ||
        !config->cloud.token_file[0]) {
        LOG_ERROR("[WEBRTC] runtime config validation failed: incomplete cloud configuration");
        return MEDIA_ERR_INVALID_CONFIG;
    }
    return MEDIA_OK;
}

/**
 * @brief 构造所有 PeerConnection 共用的 ICE 配置快照。
 * @param[in] config 已通过共享配置校验的 ICE 配置。
 * @param[in,out] runtime 待写入 ICE 快照的 WebRTC 共享运行时。
 * @retval MEDIA_OK ICE 配置和可选 TURN 密码加载成功。
 * @retval MEDIA_ERR_INVALID_PARAM config 或 runtime 为空。
 * @retval MEDIA_ERR_INVALID_CONFIG TURN 密码文件不可用。
 */
static MediaResult webrtc_runtime_initialize_ice(
    const MediaOutputWebRtcIceConfig *config,
    MediaOutputWebRtcRuntime *runtime)
{
    if (!config || !runtime) {
        LOG_ERROR("[WEBRTC] initialize ICE failed: config=%p runtime=%p",
                  (const void *)config,
                  (void *)runtime);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* 先复制不敏感的 ICE 参数，形成后续 PeerConnection 使用的配置快照。 */
    runtime->ice.stunHost = config->stun.host;
    runtime->ice.stunPort = static_cast<uint16_t>(config->stun.port);
    runtime->ice.turnHost = config->turn.host;
    runtime->ice.turnPort = static_cast<uint16_t>(config->turn.port);
    runtime->ice.turnUsername = config->turn.username;
    runtime->ice.relayOnly = config->relay_only != 0;
    /* TURN 密码只从私有文件加载，不从 TOML 明文配置或日志中获取。 */
    if (!runtime->ice.turnHost.empty() &&
        !webrtc_runtime_read_private_secret(config->turn.password_file,
                                            runtime->ice.turnPassword)) {
        LOG_ERROR("[WEBRTC] initialize ICE failed: TURN password unavailable");
        return MEDIA_ERR_INVALID_CONFIG;
    }
    return MEDIA_OK;
}

/**
 * @brief 加载设备令牌并创建网关唯一的云端信令客户端。
 * @param[in] config 已通过共享配置校验的云端连接配置。
 * @param[in,out] runtime 待保存云端地址和共享客户端的运行时对象。
 * @retval MEDIA_OK 设备令牌加载成功且客户端创建完成。
 * @retval MEDIA_ERR_INVALID_PARAM config 或 runtime 为空。
 * @retval MEDIA_ERR_INVALID_CONFIG 设备令牌文件不可用。
 * @throws std::bad_alloc 创建字符串或客户端对象时内存不足，由上层统一转换错误码。
 */
static MediaResult webrtc_runtime_initialize_cloud_client(
    const MediaOutputWebRtcCloudConfig *config,
    MediaOutputWebRtcRuntime *runtime)
{
    WebRtcCloudConnectionConfig cloudConfig;

    if (!config || !runtime) {
        LOG_ERROR("[WEBRTC] initialize cloud client failed: config=%p runtime=%p",
                  (const void *)config,
                  (void *)runtime);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* 先复制公开的连接标识，再单独从私有文件加载敏感令牌。 */
    cloudConfig.url = config->url;
    cloudConfig.deviceId = config->device_id;
    if (!webrtc_runtime_read_private_secret(config->token_file, cloudConfig.token)) {
        LOG_ERROR("[WEBRTC] initialize cloud client failed: device token unavailable");
        return MEDIA_ERR_INVALID_CONFIG;
    }
    /* 客户端只负责共享设备长连接；每路 stream_id 由输出启动时另行登记。 */
    runtime->cloudEndpoint = cloudConfig.url;
    runtime->cloudClient = std::make_shared<WebRtcCloudSignalingClient>(cloudConfig);
    return MEDIA_OK;
}

/**
 * @brief 创建 WebRTC 输出共享运行时。
 * @param[in] config WebRTC 共享 ICE 和云端信令配置。
 * @param[out] runtime 返回新创建的运行时对象；失败时保持为空。
 * @retval MEDIA_OK 运行时创建成功。
 * @retval MEDIA_ERR_INVALID_PARAM 输入参数非法。
 * @retval MEDIA_ERR_INVALID_CONFIG 共享配置或私有凭据无效。
 * @retval MEDIA_ERR_NO_MEMORY 创建运行时对象或内部配置时内存不足。
 * @retval MEDIA_ERR 创建过程中发生其他异常。
 */
extern "C" MediaResult media_output_webrtc_runtime_create(const MediaOutputWebRtcSharedConfig *config, MediaOutputWebRtcRuntime **runtime)
{
    std::unique_ptr<MediaOutputWebRtcRuntime> created;
    MediaResult result = MEDIA_OK; 

    if (!config || !runtime) {
        LOG_ERROR("[WEBRTC] runtime create failed: config=%p runtime=%p",
                  (const void *)config,
                  (void *)runtime);
        return MEDIA_ERR_INVALID_PARAM;
    }
    /* 从此处开始保证所有后续失败路径都向调用方返回空指针。 */
    *runtime = NULL;
    /* 第一阶段：在分配资源前完成严格配置校验。 */
    result = webrtc_runtime_validate_config(config);
    if (result != MEDIA_OK) {
        LOG_ERROR("[WEBRTC] runtime create failed: invalid config result=%d", result);
        return result;
    }

    try {
        /* 第二阶段：创建临时运行时并加载所有 PeerConnection 共用的 ICE 快照。 */
        created.reset(new MediaOutputWebRtcRuntime());
        result = webrtc_runtime_initialize_ice(&config->ice, created.get());
        if (result != MEDIA_OK) {
            LOG_ERROR("[WEBRTC] runtime create failed: initialize ICE result=%d", result);
            return result;
        }
        /* 第三阶段：创建所有输出共用的云端设备信令客户端。 */
        result = webrtc_runtime_initialize_cloud_client(&config->cloud, created.get());
        if (result != MEDIA_OK) {
            LOG_ERROR("[WEBRTC] runtime create failed: initialize cloud client result=%d",
                      result);
            return result;
        }
    } catch (const std::bad_alloc &error) {
        LOG_ERROR("[WEBRTC] runtime create failed: no memory: %s", error.what());
        return MEDIA_ERR_NO_MEMORY;
    } catch (const std::exception &error) {
        LOG_ERROR("[WEBRTC] runtime create failed: %s", error.what());
        return MEDIA_ERR;
    }

    /* 全部阶段完成后再向调用方发布对象，避免暴露半初始化运行时。 */
    *runtime = created.release();
    return MEDIA_OK;
}

/**
 * @brief 启动 WebRTC 共享运行时中的云端信令线程。
 * @param[in,out] runtime 已成功创建的 WebRTC 共享运行时。
 * @retval MEDIA_OK 云端信令客户端启动成功。
 * @retval MEDIA_ERR_INVALID_PARAM runtime 为空。
 * @retval MEDIA_ERR 云端信令客户端启动失败。
 */
extern "C" MediaResult media_output_webrtc_runtime_start(MediaOutputWebRtcRuntime *runtime)
{
    if (!runtime) {
        LOG_ERROR("[WEBRTC] runtime start failed: runtime is NULL");
        return MEDIA_ERR_INVALID_PARAM;
    }
    if (!runtime->cloudClient) {
        LOG_ERROR("[WEBRTC] runtime start failed: cloud client is unavailable");
        return MEDIA_ERR_NOT_READY;
    }

    /* 云端模式只启动一个设备长连接，所有云端输出共享该连接。 */
    if (!runtime->cloudClient->start()) {
        LOG_ERROR("[WEBRTC] runtime start failed: cloud signaling worker");
        return MEDIA_ERR;
    }
    return MEDIA_OK;
}

/**
 * @brief 停止 WebRTC 共享运行时中的云端信令线程。
 * @param[in,out] runtime WebRTC 共享运行时；允许传入空指针。
 * @note 函数可重复调用；没有云端客户端时直接返回。
 */
extern "C" void media_output_webrtc_runtime_stop(MediaOutputWebRtcRuntime *runtime)
{
    /* 共享客户端内部负责关闭设备长连接、停止重连并清理会话路由。 */
    if (runtime && runtime->cloudClient) {
        runtime->cloudClient->stop();
    }
}

/**
 * @brief 停止并销毁 WebRTC 共享运行时。
 * @param[in,out] runtime 待销毁的运行时对象；允许传入空指针。
 * @note 销毁前会主动停止云端信令，调用方不得继续使用原指针。
 */
extern "C" void media_output_webrtc_runtime_destroy(MediaOutputWebRtcRuntime *runtime)
{
    if (!runtime) {
        return;
    }
    /* 先停止异步线程和回调，再释放其配置及共享客户端对象。 */
    media_output_webrtc_runtime_stop(runtime);
    delete runtime;
}
