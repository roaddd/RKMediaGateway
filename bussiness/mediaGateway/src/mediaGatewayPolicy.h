#ifndef __MEDIA_GATEWAY_POLICY_H__
#define __MEDIA_GATEWAY_POLICY_H__

#include "mediaGateway.h"

#ifdef __cplusplus
extern "C" {
#endif

void media_gateway_update_runtime_policies_if_due(MediaGatewayCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
