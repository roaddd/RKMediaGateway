#ifndef __MEDIA_GATEWAY_PROCESS_H__
#define __MEDIA_GATEWAY_PROCESS_H__

#include "mediaGateway.h"

#include "audioFrameSource.h"
#include "mediaFrameSource.h"

#ifdef __cplusplus
extern "C" {
#endif

int media_gateway_reset_encoder(MediaGatewayCtx *ctx, int stream_idx);
int media_gateway_process_stream(MediaGatewayCtx *ctx,
                                 MediaGatewayRunState *state,
                                 const MediaFrame *frame,
                                 int stream_idx);
int media_gateway_process_audio_group(MediaGatewayCtx *ctx,
                                      const AudioFrame *frame,
                                      AudioEncoderRuntimeGroupId group_id);

#ifdef __cplusplus
}
#endif

#endif
