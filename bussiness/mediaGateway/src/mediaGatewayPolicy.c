/**
 * @file mediaGatewayPolicy.c
 * @brief Runtime policy hook for MediaGateway.
 *
 * The current ISP test mode is observation-only. ISP/3A values are printed by
 * mediaGatewayStats.c through isp_controller_query_status(). This module must
 * not change ISP controls, low-light state, encoder bitrate, or QP settings.
 */
#include "mediaGatewayPolicy.h"

void media_gateway_update_runtime_policies_if_due(MediaGatewayCtx *ctx)
{
    (void)ctx;
}
