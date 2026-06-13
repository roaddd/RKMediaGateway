/**
 * @file mediaGatewayPolicy.h
 * @brief Runtime policy hook for MediaGateway.
 *
 * In ISP baseline test mode this hook is intentionally observation-only.
 */
#ifndef __MEDIA_GATEWAY_POLICY_H__
#define __MEDIA_GATEWAY_POLICY_H__

#include "mediaGateway.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Update runtime policies when needed.
 *
 * Current behavior is a no-op: ISP/3A status is queried and logged from the
 * stats path, and no ISP or encoder runtime parameter is modified here.
 */
void media_gateway_update_runtime_policies_if_due(MediaGatewayCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
