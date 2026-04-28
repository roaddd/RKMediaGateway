#ifndef __GB28181_OUTPUT_INTERNAL_H__
#define __GB28181_OUTPUT_INTERNAL_H__

#include "mediaOutput.h"

/* GB28181 输出实现的内部入口，仅供 mediaOutput 模块内部调用。 */
int media_output_setup_gb28181(MediaOutput *output, const MediaOutputGb28181Config *config);
int media_output_gb28181_consume_external_idr_request(MediaOutput *output);

#endif
