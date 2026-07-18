#ifndef __MEDIA_GATEWAY_POLICY_H__
#define __MEDIA_GATEWAY_POLICY_H__

#include "mediaGateway.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 按策略评估周期刷新运行期自适应目标。
 * 该函数只计算融合后的目标帧率、编码参数和 pacing rate，不直接操作硬件或输出协议。
 */
void media_gateway_refresh_adaptive_policy_targets_if_due(MediaGatewayCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
