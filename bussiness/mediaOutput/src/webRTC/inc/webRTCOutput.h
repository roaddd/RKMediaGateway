#ifndef __WEBRTC_OUTPUT_INTERNAL_H__
#define __WEBRTC_OUTPUT_INTERNAL_H__

#include "mediaOutput.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WebRTC 输出实现的内部入口，仅供 mediaOutput 模块内部调用。 */
int media_output_setup_webrtc(MediaOutput *output, const MediaOutputWebRtcConfig *config);

/*
 * 消费 WebRTC 新会话就绪后触发的一次性 IDR 请求。
 * 返回 1 表示上游编码器应立即生成关键帧，返回 0 表示当前没有请求。
 */
int media_output_webrtc_consume_external_idr_request(MediaOutput *output);

#ifdef __cplusplus
}
#endif

#endif
