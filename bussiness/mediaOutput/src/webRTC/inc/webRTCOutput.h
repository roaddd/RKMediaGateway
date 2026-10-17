/**
 * @file webRTCOutput.h
 * @brief 声明 WebRTC 协议输出接入通用 mediaOutput 框架的内部接口。
 */
#ifndef __WEBRTC_OUTPUT_INTERNAL_H__
#define __WEBRTC_OUTPUT_INTERNAL_H__

#include "mediaOutput.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 为通用 MediaOutput 创建 WebRTC 协议后端。
 * @param[in,out] output 尚未初始化的输出对象。
 * @param[in] config 已由调用方准备的单路 WebRTC 输出配置。
 * @return MEDIA_OK 表示成功，其它 MediaResult 表示失败。
 */
int media_output_setup_webrtc(MediaOutput *output, const MediaOutputWebRtcConfig *config);

/**
 * @brief 消费 WebRTC 会话产生的一次性 IDR 请求。
 * @param[in,out] output 已初始化的 WebRTC 输出对象。
 * @return 1 表示上游编码器应立即生成关键帧，0 表示当前没有请求。
 */
int media_output_webrtc_consume_external_idr_request(MediaOutput *output);

#ifdef __cplusplus
}
#endif

#endif
