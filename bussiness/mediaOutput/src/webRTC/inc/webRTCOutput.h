#ifndef __WEBRTC_OUTPUT_INTERNAL_H__
#define __WEBRTC_OUTPUT_INTERNAL_H__

#include "mediaOutput.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WebRTC 输出实现的内部入口，仅供 mediaOutput 模块内部调用。 */
int media_output_setup_webrtc(MediaOutput *output, const MediaOutputWebRtcConfig *config);

#ifdef __cplusplus
}
#endif

#endif
