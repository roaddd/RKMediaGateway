/**
 * @file mediaGatewayWebRtcProfile.h
 * @brief WebRTC视频质量档位绑定的内部解析接口。
 *
 * 本文件仅供MediaGateway配置初始化阶段使用。模块负责校验流畅/高清档位、
 * 将档位名称解析为已有编码流，并保证运行期只启用一个WebRTC视频输出。
 */

#ifndef MEDIA_GATEWAY_WEBRTC_PROFILE_H
#define MEDIA_GATEWAY_WEBRTC_PROFILE_H

#include "mediaGateway.h"

/**
 * @description: 校验WebRTC视频档位配置，并解析当前档位对应的唯一编码流。
 * @param config 待校验的网关配置；成功后会写入selected_stream_index，
 *               并仅为选中的视频流设置enable_webrtc。
 * @return 0表示解析成功，-1表示档位、码流引用或WebRTC媒体参数无效。
 */
int media_gateway_resolve_webrtc_video_binding(MediaGatewayConfig *config);

#endif /* MEDIA_GATEWAY_WEBRTC_PROFILE_H */
