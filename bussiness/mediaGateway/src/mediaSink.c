#include "mediaSink.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_SINK_QUEUE_CAPACITY 32
#define DEFAULT_RECONNECT_INTERVAL_MS 1000

/**
 * @description: 在加锁状态下从发送队列取出一个媒体包
 * @param {MediaSink *} sink
 * @param {MediaPacket *} packet
 * @return {static int}
 */
static int media_sink_pop_locked(MediaSink *sink, MediaPacket *packet) {
    /* 调用方必须已经持有 sink->lock，这里只处理队列头部弹出逻辑。 */
    if (sink->queue_size <= 0) {
        return -1;
    }

    /* 队列里的 MediaPacket 持有的是共享 buffer 引用，直接结构体拷贝即可。 */
    *packet = sink->queue[sink->queue_head];
    /* 当前位置立刻清空，避免后续重复释放同一个 buffer 引用。 */
    media_packet_init(&sink->queue[sink->queue_head]);
    sink->queue_head = (sink->queue_head + 1) % sink->queue_capacity;
    sink->queue_size--;
    sink->stats.queue_depth = sink->queue_size;
    return 0;
}

/**
 * @description: 在加锁状态下丢弃队列中最旧的媒体包
 * @param {MediaSink *} sink
 * @return {static void}
 */
static void media_sink_drop_oldest_locked(MediaSink *sink) {
    MediaPacket packet;

    media_packet_init(&packet);
    /* 队列满时丢弃最老的数据，优先给更新的关键帧腾位置。 */
    if (media_sink_pop_locked(sink, &packet) == 0) {
        sink->stats.dropped_frames++;
        media_packet_reset(&packet);
    }
}

/**
 * @description: 发送线程主函数
 * @param {void *} arg
 * @return {static void *}
 */
static void *media_sink_thread(void *arg) {
    MediaSink *sink = (MediaSink *)arg;
    MediaPacket packet;

    media_packet_init(&packet);
    while (1) {
        pthread_mutex_lock(&sink->lock);
        /* 没有数据时阻塞等待；stop_requested 用于唤醒线程安全退出。 */
        while (!sink->stop_requested && sink->queue_size == 0) {
            pthread_cond_wait(&sink->cond, &sink->lock);
        }
        /* 停止时把队列中残留数据发送完，再真正退出线程。 */
        if (sink->stop_requested && sink->queue_size == 0) {
            pthread_mutex_unlock(&sink->lock);
            break;
        }
        if (media_sink_pop_locked(sink, &packet) != 0) {
            pthread_mutex_unlock(&sink->lock);
            continue;
        }
        pthread_mutex_unlock(&sink->lock);

        /* 懒连接策略：真正有数据要发时才去建立下游连接。 */
        if (!sink->connected) {
            if (sink->vtable->connect(sink) == 0) {
                pthread_mutex_lock(&sink->lock);
                sink->connected = 1;
                sink->stats.connected = 1;
                /* 重连成功后可选择等待关键帧，避免下游从非关键帧开始花屏。 */
                sink->waiting_for_keyframe = sink->config.drop_until_keyframe_after_reconnect ? 1 : 0;
                sink->stats.waiting_for_keyframe = sink->waiting_for_keyframe;
                sink->stats.reconnect_count++;
                pthread_mutex_unlock(&sink->lock);
                printf("[SINK] name=%s event=connected reconnects=%" PRIu64 "\n",
                       sink->config.name ? sink->config.name : "unknown",
                       sink->stats.reconnect_count);
            } else {
                pthread_mutex_lock(&sink->lock);
                sink->stats.connected = 0;
                pthread_mutex_unlock(&sink->lock);
                fprintf(stderr, "[SINK] name=%s event=connect_failed retry_ms=%d\n",
                        sink->config.name ? sink->config.name : "unknown",
                        sink->config.reconnect_interval_ms);
                media_packet_reset(&packet);
                usleep((useconds_t)sink->config.reconnect_interval_ms * 1000U);
                continue;
            }
        }

        /* 重连后的非关键帧直接丢弃，直到拿到新的关键帧再恢复发送。 */
        if (sink->waiting_for_keyframe && !packet.is_key_frame) {
            pthread_mutex_lock(&sink->lock);
            sink->stats.dropped_frames++;
            pthread_mutex_unlock(&sink->lock);
            media_packet_reset(&packet);
            continue;
        }

        /* 收到关键帧后，说明下游可以重新开始解码了。 */
        if (sink->waiting_for_keyframe && packet.is_key_frame) {
            pthread_mutex_lock(&sink->lock);
            sink->waiting_for_keyframe = 0;
            sink->stats.waiting_for_keyframe = 0;
            pthread_mutex_unlock(&sink->lock);
        }

        /* 发送失败时标记连接失效，下一轮自动走重连流程。 */
        if (sink->vtable->send_packet(sink, &packet) != 0) {
            pthread_mutex_lock(&sink->lock);
            sink->connected = 0;
            sink->stats.connected = 0;
            sink->waiting_for_keyframe = sink->config.drop_until_keyframe_after_reconnect ? 1 : 0;
            sink->stats.waiting_for_keyframe = sink->waiting_for_keyframe;
            sink->stats.send_failures++;
            pthread_mutex_unlock(&sink->lock);
            fprintf(stderr, "[SINK] name=%s event=send_failed frame=%" PRIu64 " failures=%" PRIu64 "\n",
                    sink->config.name ? sink->config.name : "unknown",
                    packet.frame_id,
                    sink->stats.send_failures);
            if (sink->vtable->disconnect) {
                sink->vtable->disconnect(sink);
            }
        } else {
            pthread_mutex_lock(&sink->lock);
            sink->stats.sent_frames++;
            sink->stats.sent_bytes += packet.buffer ? packet.buffer->size : 0;
            pthread_mutex_unlock(&sink->lock);
        }

        /* 无论发送成功还是失败，当前 packet 生命周期都在这里结束。 */
        /* 当最后一个 sink 也 reset 掉自己的引用时，buffer->ref_count 会降到 0 并真正释放。 */
        media_packet_reset(&packet);
    }

    /* 线程退出前统一断开一次，确保下游资源被释放。 */
    if (sink->vtable->disconnect) {
        sink->vtable->disconnect(sink);
    }
    return NULL;
}

/**
 * @description: 初始化通用媒体输出通道
 * @param {MediaSink *} sink
 * @param {const MediaSinkConfig *} config
 * @param {const MediaSinkVTable *} vtable
 * @param {void *} impl
 * @return {int}
 */
int media_sink_init(MediaSink *sink,
                    const MediaSinkConfig *config,
                    const MediaSinkVTable *vtable,
                    void *impl) {
    if (!sink || !config || !vtable || !vtable->connect || !vtable->send_packet) {
        fprintf(stderr, "[ERROR] media_sink_init failed: invalid arguments\n");
        return -1;
    }

    memset(sink, 0, sizeof(*sink));
    /* 配置按值保存，避免调用方传入的临时对象失效。 */
    sink->config = *config;
    sink->vtable = vtable;
    sink->impl = impl;
    sink->queue_capacity = (config->queue_capacity > 0) ? config->queue_capacity : DEFAULT_SINK_QUEUE_CAPACITY;
    sink->config.reconnect_interval_ms = (config->reconnect_interval_ms > 0)
        ? config->reconnect_interval_ms
        : DEFAULT_RECONNECT_INTERVAL_MS;
    /* 环形队列存放的是 MediaPacket 引用副本，不复制底层媒体数据。 */
    sink->queue = (MediaPacket *)calloc((size_t)sink->queue_capacity, sizeof(MediaPacket));
    if (!sink->queue) {
        fprintf(stderr, "[ERROR] media_sink_init failed: queue alloc name=%s capacity=%d\n",
                config->name ? config->name : "unknown",
                sink->queue_capacity);
        return -1;
    }

    pthread_mutex_init(&sink->lock, NULL);
    pthread_cond_init(&sink->cond, NULL);
    sink->waiting_for_keyframe = sink->config.drop_until_keyframe_after_reconnect ? 1 : 0;
    sink->stats.waiting_for_keyframe = sink->waiting_for_keyframe;
    return 0;
}

/**
 * @description: 启动媒体输出通道
 * @param {MediaSink *} sink
 * @return {int}
 */
int media_sink_start(MediaSink *sink) {
    if (!sink) {
        fprintf(stderr, "[ERROR] media_sink_start failed: sink is NULL\n");
        return -1;
    }

    /* 先让具体 sink 做自己的启动准备，再拉起通用发送线程。 */
    if (sink->vtable->start && sink->vtable->start(sink) != 0) {
        fprintf(stderr, "[ERROR] media_sink_start failed: vtable start name=%s\n",
                sink->config.name ? sink->config.name : "unknown");
        return -1;
    }

    sink->running = 1;
    if (pthread_create(&sink->thread, NULL, media_sink_thread, sink) != 0) {
        sink->running = 0;
        fprintf(stderr, "[ERROR] media_sink_start failed: pthread_create name=%s\n",
                sink->config.name ? sink->config.name : "unknown");
        if (sink->vtable->stop) {
            sink->vtable->stop(sink);
        }
        return -1;
    }
    return 0;
}

/**
 * @description: 向媒体输出通道队列压入一个媒体包
 * @param {MediaSink *} sink
 * @param {const MediaPacket *} packet
 * @return {int}
 */
int media_sink_enqueue(MediaSink *sink, const MediaPacket *packet) {
    int tail;

    if (!sink || !packet || !packet->buffer) {
        return -1;
    }

    pthread_mutex_lock(&sink->lock);
    if (sink->queue_size >= sink->queue_capacity) {
        /* 队列满时优先丢非关键帧，尽量保住关键帧，便于后续恢复画面。 */
        if (!packet->is_key_frame) {
            sink->stats.dropped_frames++;
            pthread_mutex_unlock(&sink->lock);
            return 0;
        }
        /* 如果当前来的就是关键帧，则连续淘汰旧数据，确保关键帧一定能入队。 */
        while (sink->queue_size >= sink->queue_capacity) {
            media_sink_drop_oldest_locked(sink);
        }
    }

    tail = (sink->queue_head + sink->queue_size) % sink->queue_capacity;
    /* 例如 producer 初始为 1，两个 sink 入队后会变成 3。 */
    /* 入队时只增加 buffer 引用计数，避免大块媒体数据重复拷贝。 */
    media_packet_copy_ref(&sink->queue[tail], packet);
    sink->queue_size++;
    sink->stats.queue_depth = sink->queue_size;
    pthread_cond_signal(&sink->cond);
    pthread_mutex_unlock(&sink->lock);
    return 0;
}

/**
 * @description: 停止媒体输出通道
 * @param {MediaSink *} sink
 * @return {void}
 */
void media_sink_stop(MediaSink *sink) {
    if (!sink || !sink->running) {
        return;
    }

    pthread_mutex_lock(&sink->lock);
    /* 通知工作线程停止接收新任务，并唤醒可能阻塞在条件变量上的线程。 */
    sink->stop_requested = 1;
    pthread_cond_broadcast(&sink->cond);
    pthread_mutex_unlock(&sink->lock);
    pthread_join(sink->thread, NULL);
    sink->running = 0;
    if (sink->vtable->stop) {
        sink->vtable->stop(sink);
    }
}

/**
 * @description: 释放媒体输出通道资源
 * @param {MediaSink *} sink
 * @return {void}
 */
void media_sink_deinit(MediaSink *sink) {
    int i;

    if (!sink) {
        return;
    }

    /* 先停止线程，再释放队列和同步原语。 */
    media_sink_stop(sink);
    if (!sink->running && sink->vtable && sink->vtable->stop) {
        /* 兼容未成功 start 但已完成 init 的场景，确保 stop 钩子仍有机会清理。 */
        sink->vtable->stop(sink);
    }
    for (i = 0; i < sink->queue_capacity; ++i) {
        media_packet_reset(&sink->queue[i]);
    }
    free(sink->queue);
    sink->queue = NULL;
    pthread_cond_destroy(&sink->cond);
    pthread_mutex_destroy(&sink->lock);
    memset(sink, 0, sizeof(*sink));
}

/**
 * @description: 获取媒体输出通道统计信息
 * @param {MediaSink *} sink
 * @param {MediaSinkStats *} stats
 * @return {void}
 */
void media_sink_get_stats(MediaSink *sink, MediaSinkStats *stats) {
    if (!sink || !stats) {
        return;
    }

    /* 统计信息和工作线程共享，读取时同样需要加锁。 */
    pthread_mutex_lock(&sink->lock);
    *stats = sink->stats;
    pthread_mutex_unlock(&sink->lock);
}
