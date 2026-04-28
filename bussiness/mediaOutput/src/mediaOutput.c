#include "mediaOutput.h"

#include "gb28181/inc/gb28181Output.h"
#include "rtmp/inc/rtmpOutput.h"
#include "rtsp/inc/rtspOutput.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_OUTPUT_QUEUE_CAPACITY 32
#define DEFAULT_RECONNECT_INTERVAL_MS 1000

static int media_output_pop_locked(MediaOutput *output, MediaPacket *packet) {
    /* 调用方必须持有 output->lock；弹出的是 MediaPacket 引用副本。 */
    if (output->queue_size <= 0) {
        return -1;
    }

    *packet = output->queue[output->queue_head];
    media_packet_init(&output->queue[output->queue_head]);
    output->queue_head = (output->queue_head + 1) % output->queue_capacity;
    output->queue_size--;
    output->stats.queue_depth = output->queue_size;
    return 0;
}

static void media_output_drop_oldest_locked(MediaOutput *output) {
    MediaPacket packet;

    media_packet_init(&packet);
    /* 队列满且新包是关键帧时，淘汰旧包给关键帧腾出空间。 */
    if (media_output_pop_locked(output, &packet) == 0) {
        output->stats.dropped_frames++;
        media_packet_reset(&packet);
    }
}

static void *media_output_thread(void *arg) {
    MediaOutput *output = (MediaOutput *)arg;
    MediaPacket packet;

    media_packet_init(&packet);
    while (1) {
        pthread_mutex_lock(&output->lock);
        /* 无数据时阻塞等待；stop_requested 用于唤醒并退出线程。 */
        while (!output->stop_requested && output->queue_size == 0) {
            pthread_cond_wait(&output->cond, &output->lock);
        }
        /* 停止时先清空队列，避免丢失已经入队且持有引用的数据。 */
        if (output->stop_requested && output->queue_size == 0) {
            pthread_mutex_unlock(&output->lock);
            break;
        }
        if (media_output_pop_locked(output, &packet) != 0) {
            pthread_mutex_unlock(&output->lock);
            continue;
        }
        pthread_mutex_unlock(&output->lock);

        /* 懒连接：只有真正要发送数据时才建立或重建下游连接。 */
        if (!output->connected) {
            if (output->vtable->connect(output) == 0) {
                pthread_mutex_lock(&output->lock);
                output->connected = 1;
                output->stats.connected = 1;
                output->waiting_for_keyframe = output->config.drop_until_keyframe_after_reconnect ? 1 : 0;
                output->stats.waiting_for_keyframe = output->waiting_for_keyframe;
                output->stats.reconnect_count++;
                pthread_mutex_unlock(&output->lock);
                printf("[OUTPUT] name=%s event=connected reconnects=%" PRIu64 "\n",
                       output->config.name ? output->config.name : "unknown",
                       output->stats.reconnect_count);
            } else {
                pthread_mutex_lock(&output->lock);
                output->stats.connected = 0;
                pthread_mutex_unlock(&output->lock);
                fprintf(stderr, "[OUTPUT] name=%s event=connect_failed retry_ms=%d\n",
                        output->config.name ? output->config.name : "unknown",
                        output->config.reconnect_interval_ms);
                media_packet_reset(&packet);
                usleep((useconds_t)output->config.reconnect_interval_ms * 1000U);
                continue;
            }
        }

        /* 重连后可配置为等待关键帧，避免下游从 P 帧开始解码导致花屏。 */
        if (output->waiting_for_keyframe && !packet.is_key_frame) {
            pthread_mutex_lock(&output->lock);
            output->stats.dropped_frames++;
            pthread_mutex_unlock(&output->lock);
            media_packet_reset(&packet);
            continue;
        }

        if (output->waiting_for_keyframe && packet.is_key_frame) {
            pthread_mutex_lock(&output->lock);
            output->waiting_for_keyframe = 0;
            output->stats.waiting_for_keyframe = 0;
            pthread_mutex_unlock(&output->lock);
        }

        /* 协议发送失败后标记断开，下一轮发送会重新走 connect。 */
        if (output->vtable->send_packet(output, &packet) != 0) {
            pthread_mutex_lock(&output->lock);
            output->connected = 0;
            output->stats.connected = 0;
            output->waiting_for_keyframe = output->config.drop_until_keyframe_after_reconnect ? 1 : 0;
            output->stats.waiting_for_keyframe = output->waiting_for_keyframe;
            output->stats.send_failures++;
            pthread_mutex_unlock(&output->lock);
            fprintf(stderr, "[OUTPUT] name=%s event=send_failed frame=%" PRIu64 " failures=%" PRIu64 "\n",
                    output->config.name ? output->config.name : "unknown",
                    packet.frame_id,
                    output->stats.send_failures);
            if (output->vtable->disconnect) {
                output->vtable->disconnect(output);
            }
        } else {
            pthread_mutex_lock(&output->lock);
            output->stats.sent_frames++;
            output->stats.sent_bytes += packet.buffer ? packet.buffer->size : 0;
            pthread_mutex_unlock(&output->lock);
        }

        /* 当前输出通道对该包的引用到此结束。 */
        media_packet_reset(&packet);
    }

    /* 线程退出前给协议实现一次最终断开机会。 */
    if (output->vtable->disconnect) {
        output->vtable->disconnect(output);
    }
    return NULL;
}

int media_output_setup(MediaOutput *output, const MediaOutputConfig *config) {
    if (!output || !config) {
        fprintf(stderr, "[ERROR] media_output_setup failed: invalid arguments\n");
        return -1;
    }

    switch (config->type) {
    case MEDIA_OUTPUT_TYPE_RTSP:
        return media_output_setup_rtsp(output, &config->protocol.rtsp);
    case MEDIA_OUTPUT_TYPE_RTMP:
        return media_output_setup_rtmp(output, &config->protocol.rtmp);
    case MEDIA_OUTPUT_TYPE_GB28181:
        return media_output_setup_gb28181(output, &config->protocol.gb28181);
    default:
        fprintf(stderr, "[ERROR] media_output_setup failed: unknown type=%d\n", config->type);
        return -1;
    }
}

int media_output_init(MediaOutput *output,
                      const MediaOutputChannelConfig *config,
                      const MediaOutputVTable *vtable,
                      void *impl) {
    if (!output || !config || !vtable || !vtable->connect || !vtable->send_packet) {
        fprintf(stderr, "[ERROR] media_output_init failed: invalid arguments\n");
        return -1;
    }

    memset(output, 0, sizeof(*output));
    /* 通用层按值保存配置，协议私有状态通过 impl 交给具体输出实现管理。 */
    output->config = *config;
    output->vtable = vtable;
    output->impl = impl;
    output->queue_capacity = (config->queue_capacity > 0) ? config->queue_capacity : DEFAULT_OUTPUT_QUEUE_CAPACITY;
    output->config.reconnect_interval_ms = (config->reconnect_interval_ms > 0)
        ? config->reconnect_interval_ms
        : DEFAULT_RECONNECT_INTERVAL_MS;
    /* 队列保存的是 MediaPacket 引用副本，不复制底层媒体数据。 */
    output->queue = (MediaPacket *)calloc((size_t)output->queue_capacity, sizeof(MediaPacket));
    if (!output->queue) {
        fprintf(stderr, "[ERROR] media_output_init failed: queue alloc name=%s capacity=%d\n",
                config->name ? config->name : "unknown",
                output->queue_capacity);
        return -1;
    }

    pthread_mutex_init(&output->lock, NULL);
    pthread_cond_init(&output->cond, NULL);
    output->waiting_for_keyframe = output->config.drop_until_keyframe_after_reconnect ? 1 : 0;
    output->stats.waiting_for_keyframe = output->waiting_for_keyframe;
    return 0;
}

int media_output_start(MediaOutput *output) {
    if (!output) {
        fprintf(stderr, "[ERROR] media_output_start failed: output is NULL\n");
        return -1;
    }

    if (output->vtable->start && output->vtable->start(output) != 0) {
        fprintf(stderr, "[ERROR] media_output_start failed: vtable start name=%s\n",
                output->config.name ? output->config.name : "unknown");
        return -1;
    }

    output->running = 1;
    if (pthread_create(&output->thread, NULL, media_output_thread, output) != 0) {
        output->running = 0;
        fprintf(stderr, "[ERROR] media_output_start failed: pthread_create name=%s\n",
                output->config.name ? output->config.name : "unknown");
        if (output->vtable->stop) {
            output->vtable->stop(output);
        }
        return -1;
    }
    return 0;
}

int media_output_enqueue(MediaOutput *output, const MediaPacket *packet) {
    int tail;

    if (!output || !packet || !packet->buffer) {
        return -1;
    }

    pthread_mutex_lock(&output->lock);
    if (output->queue_size >= output->queue_capacity) {
        /* 非关键帧在队列满时直接丢弃，优先降低延迟。 */
        if (!packet->is_key_frame) {
            output->stats.dropped_frames++;
            pthread_mutex_unlock(&output->lock);
            return 0;
        }
        /* 关键帧到来时淘汰旧包，保证后续解码有恢复点。 */
        while (output->queue_size >= output->queue_capacity) {
            media_output_drop_oldest_locked(output);
        }
    }

    tail = (output->queue_head + output->queue_size) % output->queue_capacity;
    /* 入队时只增加共享 buffer 的引用计数。 */
    media_packet_copy_ref(&output->queue[tail], packet);
    output->queue_size++;
    output->stats.queue_depth = output->queue_size;
    pthread_cond_signal(&output->cond);
    pthread_mutex_unlock(&output->lock);
    return 0;
}

int media_output_consume_external_idr_request(MediaOutput *output) {
    if (!output) {
        return 0;
    }
    if (output->type == MEDIA_OUTPUT_TYPE_RTSP) {
        return media_output_rtsp_consume_external_idr_request(output);
    }
    if (output->type == MEDIA_OUTPUT_TYPE_GB28181) {
        return media_output_gb28181_consume_external_idr_request(output);
    }
    return 0;
}

void media_output_stop(MediaOutput *output) {
    if (!output || !output->running) {
        return;
    }

    pthread_mutex_lock(&output->lock);
    output->stop_requested = 1;
    pthread_cond_broadcast(&output->cond);
    pthread_mutex_unlock(&output->lock);
    pthread_join(output->thread, NULL);
    output->running = 0;
    if (output->vtable->stop) {
        output->vtable->stop(output);
    }
}

void media_output_deinit(MediaOutput *output) {
    int i;

    if (!output) {
        return;
    }

    media_output_stop(output);
    if (!output->running && output->vtable && output->vtable->stop) {
        output->vtable->stop(output);
    }
    for (i = 0; i < output->queue_capacity; ++i) {
        media_packet_reset(&output->queue[i]);
    }
    free(output->queue);
    output->queue = NULL;
    pthread_cond_destroy(&output->cond);
    pthread_mutex_destroy(&output->lock);
    memset(output, 0, sizeof(*output));
}

void media_output_get_stats(MediaOutput *output, MediaOutputStats *stats) {
    if (!output || !stats) {
        return;
    }

    pthread_mutex_lock(&output->lock);
    *stats = output->stats;
    pthread_mutex_unlock(&output->lock);
}
