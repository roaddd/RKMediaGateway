/*** 
 * @Author: huangkelong
 * @Date: 2026-03-12 23:18:12
 * @LastEditTime: 2026-03-15 23:13:48
 * @LastEditors: huangkelong
 * @Description: 
 * @FilePath: \RKMediaGateway\bussiness\mediaGateway\inc\mediaSink.h
 */
#ifndef __MEDIA_SINK_H__
#define __MEDIA_SINK_H__

#include "mediaPacket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MediaSink MediaSink;

typedef struct {
    const char *name;                         /* 输出通道名称，用于日志和统计信息标识。 */
    int queue_capacity;                       /* 发送队列容量，决定该 sink 最多能积压多少帧。 */
    int reconnect_interval_ms;                /* 连接失败后的重试间隔，单位毫秒。 */
    int drop_until_keyframe_after_reconnect;  /* 重连后是否丢弃非关键帧，直到收到关键帧再恢复发送。 */
} MediaSinkConfig;

typedef struct {
    uint64_t sent_frames;       /* 成功发送的帧数。 */
    uint64_t sent_bytes;        /* 成功发送的字节数。 */
    uint64_t dropped_frames;    /* 因队列满、等待关键帧等原因被丢弃的帧数。 */
    uint64_t reconnect_count;   /* 成功重连的次数。 */
    uint64_t send_failures;     /* 发送失败次数。 */
    int queue_depth;            /* 当前队列深度。 */
    int connected;              /* 当前是否已连接到底层输出端。 */
    int waiting_for_keyframe;   /* 当前是否处于等待关键帧恢复发送的状态。 */
} MediaSinkStats;

typedef struct {
    int (*start)(MediaSink *sink);                            /* sink 启动钩子，做一次性准备工作。 */
    int (*connect)(MediaSink *sink);                          /* sink 连接钩子，用于建立或重建下游连接。 */
    int (*send_packet)(MediaSink *sink, const MediaPacket *packet); /* sink 发送钩子，负责输出单帧数据。 */
    void (*disconnect)(MediaSink *sink);                      /* sink 断开钩子，用于释放连接态资源。 */
    void (*stop)(MediaSink *sink);                            /* sink 停止钩子，用于整体退出前清理。 */
} MediaSinkVTable;

struct MediaSink {
    MediaSinkConfig config;              /* 通用 sink 配置。 */
    MediaSinkStats stats;                /* 运行时统计信息。 */
    const MediaSinkVTable *vtable;       /* 不同协议 sink 的回调函数表。 */
    void *impl;                          /* 具体协议实现的私有上下文，例如 RTSP/RTMP 的 impl。 */
    pthread_t thread;                    /* 后台发送线程。 */
    pthread_mutex_t lock;                /* 保护队列和统计信息的互斥锁。 */
    pthread_cond_t cond;                 /* 队列为空时用于阻塞/唤醒发送线程的条件变量。 */
    MediaPacket *queue;                  /* 环形队列，保存待发送的媒体包引用。 */
    int queue_capacity;                  /* 队列总容量。 */
    int queue_head;                      /* 当前队头下标。 */
    int queue_size;                      /* 当前队列中有效元素数量。 */
    int running;                         /* 发送线程是否已经启动。 */
    int stop_requested;                  /* 是否已请求发送线程退出。 */
    int connected;                       /* 当前 sink 是否已经连接到底层输出目标。 */
    int waiting_for_keyframe;            /* 重连后是否仍在等待关键帧恢复发送。 */
};

int media_sink_init(MediaSink *sink,
                    const MediaSinkConfig *config,
                    const MediaSinkVTable *vtable,
                    void *impl);
int media_sink_start(MediaSink *sink);
int media_sink_enqueue(MediaSink *sink, const MediaPacket *packet);
void media_sink_stop(MediaSink *sink);
void media_sink_deinit(MediaSink *sink);
void media_sink_get_stats(MediaSink *sink, MediaSinkStats *stats);

#ifdef __cplusplus
}
#endif

#endif