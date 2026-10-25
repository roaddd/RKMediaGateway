/**
 * @file mediaGatewayCapabilityAdapter.h
 * @brief MediaGateway 视频配置与全局媒体能力模块之间的内部适配接口。
 *
 * 本文件不定义硬件能力上限，只负责把 MediaGatewayVideoConfig 转换成
 * capability 模块的通用需求，并执行统一校验。启动准入和调试查询必须复用
 * 本接口，避免两处对启用码流、分辨率和帧率采用不同的统计口径。
 */

#ifndef MEDIA_GATEWAY_CAPABILITY_ADAPTER_H
#define MEDIA_GATEWAY_CAPABILITY_ADAPTER_H

#include "mediaCapability.h"
#include "mediaGateway.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 查询平台 H.264 能力并校验一份 MediaGateway 视频配置。
 *
 * 仅把配置中实际启用的码流计入编码实例数和总像素吞吐量。函数不会创建编码器、
 * 打开采集设备或修改传入配置，因此既可用于启动前准入，也可用于调试命令查询。
 *
 * @param video_config 待评估的网关视频配置。
 * @param capability 输出当前平台 H.264 编码能力。
 * @param validation 输出候选配置的详细校验结果；校验拒绝时仍然有效。
 * @return MEDIA_OK 表示配置可运行；参数错误、配置错误或能力不足时返回对应
 * MediaResult 枚举值。
 */
MediaResult media_gateway_evaluate_video_capability(
    const MediaGatewayVideoConfig *video_config,
    MediaVideoEncodeCapability *capability,
    MediaVideoCapabilityValidation *validation);

/**
 * @brief 校验某个采集源切换到目标帧率后的完整视频编码组合。
 *
 * 函数先复制当前视频配置，再把绑定 source_index 的全部启用码流帧率替换为
 * target_fps，最后调用统一能力校验。传入配置不会被修改，适合在异步硬件事务
 * 启动前进行无副作用预检。
 *
 * @param video_config 当前网关视频配置。
 * @param source_index 准备切换帧率的采集源下标。
 * @param target_fps 该采集源及其编码码流的候选目标帧率。
 * @param capability 输出当前平台 H.264 编码能力。
 * @param validation 输出候选组合的详细校验结果。
 * @return MEDIA_OK 表示允许发起切换；参数、配置或能力校验失败时返回对应
 * MediaResult 枚举值。
 */
MediaResult media_gateway_evaluate_video_source_fps(
    const MediaGatewayVideoConfig *video_config,
    int source_index,
    int target_fps,
    MediaVideoEncodeCapability *capability,
    MediaVideoCapabilityValidation *validation);

#ifdef __cplusplus
}
#endif

#endif
