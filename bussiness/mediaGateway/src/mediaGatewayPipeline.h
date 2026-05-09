#ifndef __MEDIA_GATEWAY_PIPELINE_H__
#define __MEDIA_GATEWAY_PIPELINE_H__

#include "mediaGateway.h"

#include "audioFrameSource.h"
#include "mediaFrameSource.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t *data;
    size_t capacity;
    MediaFrame frame;
    int valid;
    int running;
    int ready;
    uint64_t dropped_frames;
} VideoEncodeInput;

typedef struct {
    uint8_t *data;
    size_t capacity;
    AudioFrame frame;
} AudioEncodeSlot;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    AudioEncodeSlot *slots;
    int capacity;
    int head;
    int size;
    int running;
    int ready;
    uint64_t dropped_frames;
} AudioEncodeQueue;

typedef struct {
    MediaGatewayCtx *ctx;
    MediaGatewayRunState state;
    VideoEncodeInput video_inputs[MEDIA_GATEWAY_MAX_STREAMS];
    AudioEncodeQueue audio_queue;
    pthread_t video_threads[MEDIA_GATEWAY_MAX_STREAMS];
    int video_thread_started[MEDIA_GATEWAY_MAX_STREAMS];
    pthread_t audio_thread;
    int audio_thread_started;
    pthread_mutex_t ret_lock;
    int ret_lock_ready;
    int ret;
} MediaGatewayPipeline;

typedef struct {
    MediaGatewayPipeline *pipeline;
    int stream_idx;
} VideoEncodeThreadArg;

int media_gateway_pipeline_init(MediaGatewayPipeline *pipeline, MediaGatewayCtx *ctx);
int media_gateway_pipeline_start_workers(MediaGatewayPipeline *pipeline);
void media_gateway_pipeline_join_workers(MediaGatewayPipeline *pipeline);
void media_gateway_pipeline_deinit(MediaGatewayPipeline *pipeline);
int media_gateway_pipeline_get_ret(MediaGatewayPipeline *pipeline);
int media_gateway_video_input_publish(VideoEncodeInput *input, const MediaFrame *frame);
int media_gateway_audio_queue_publish(AudioEncodeQueue *queue, const AudioFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
