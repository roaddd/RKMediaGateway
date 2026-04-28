#ifndef __RTMP_OUTPUT_INTERNAL_H__
#define __RTMP_OUTPUT_INTERNAL_H__

#include "mediaOutput.h"

/* RTMP 输出实现的内部入口，仅供 mediaOutput 模块内部调用。 */
int media_output_setup_rtmp(MediaOutput *output, const MediaOutputRtmpConfig *config);

#endif
