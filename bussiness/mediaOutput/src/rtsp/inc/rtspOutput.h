#ifndef __RTSP_OUTPUT_INTERNAL_H__
#define __RTSP_OUTPUT_INTERNAL_H__

#include "mediaOutput.h"

/* RTSP 输出实现的内部入口，仅供 mediaOutput 模块内部调用。 */
int media_output_setup_rtsp(MediaOutput *output, const MediaOutputRtspConfig *config);
int media_output_rtsp_consume_external_idr_request(MediaOutput *output);

#endif
