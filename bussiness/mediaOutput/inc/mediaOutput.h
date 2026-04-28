#ifndef __MEDIA_OUTPUT_H__
#define __MEDIA_OUTPUT_H__

#include <pthread.h>
#include <stdint.h>

#include "mediaPacket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_OUTPUT_TYPE_RTSP = 0,
    MEDIA_OUTPUT_TYPE_RTMP = 1,
    MEDIA_OUTPUT_TYPE_GB28181 = 2
} MediaOutputType;

typedef struct MediaOutput MediaOutput;

typedef struct {
    const char *name;         /* 输出通道名称，用于日志和统计信息。 */
    const char *session_name; /* RTSP URL 中的 session 名称。 */
    const char *server_ip;    /* RTSP 服务监听地址。 */
    int server_port;          /* RTSP 服务监听端口。 */
    int auth_enable;          /* 是否启用 RTSP 鉴权。 */
    const char *user;         /* 鉴权用户名。 */
    const char *password;     /* 鉴权密码。 */
    int queue_capacity;       /* 输出队列容量。 */
    int immediate_sps_pps_on_new_client; /* 新客户端接入时是否立即补发缓存的 SPS/PPS。 */
} MediaOutputRtspConfig;

typedef struct {
    const char *name;          /* 输出通道名称，用于日志和统计信息。 */
    const char *publish_url;   /* RTMP 推流地址，例如 rtmp://host/app/stream。 */
    int queue_capacity;        /* 输出队列容量。 */
    int reconnect_interval_ms; /* 连接失败后的重连间隔，单位毫秒。 */
    int connect_timeout_ms;    /* 建立 RTMP 连接的超时时间，单位毫秒。 */
    int audio_enabled;         /* 音频通路预留开关，当前主要用于元数据描述。 */
    int video_width;           /* 元数据中的视频宽度。 */
    int video_height;          /* 元数据中的视频高度。 */
    int video_fps;             /* 元数据中的视频帧率。 */
    int video_bitrate;         /* 元数据中的视频码率。 */
    const char *video_codec_name; /* 元数据中的视频编码名称。 */
    const char *encoder_name;     /* 元数据中的编码器名称。 */
} MediaOutputRtmpConfig;

typedef struct {
    const char *name;                  /* 输出通道名称，便于日志和运行统计定位。 */
    const char *server_ip;             /* GB28181/SIP 平台地址。 */
    int server_port;                   /* SIP 服务端口。 */
    const char *server_id;             /* 平台国标编码。 */
    const char *server_domain;         /* 平台域。 */
    const char *device_id;             /* 设备国标编码。 */
    const char *device_domain;         /* 设备所属域。 */
    const char *device_password;       /* SIP Digest 鉴权密码。 */
    const char *bind_ip;               /* 本地 SIP 监听绑定地址。 */
    int local_sip_port;                /* 本地 SIP 监听端口。 */
    const char *sip_contact_ip;        /* SIP Contact 对外声明的设备 IP。 */
    const char *media_ip;              /* SDP 中对外声明的媒体发送 IP。 */
    int media_port;                    /* 本地 RTP 绑定端口。 */
    int register_expires;              /* REGISTER 有效期，单位秒。 */
    int keepalive_interval_sec;        /* Keepalive 周期，单位秒。 */
    int register_retry_interval_sec;   /* 注册失败后的重试间隔，单位秒。 */
    const char *device_name;           /* DeviceInfo/Catalog 响应里的设备名。 */
    const char *manufacturer;          /* DeviceInfo/Catalog 响应里的厂商字段。 */
    const char *model;                 /* DeviceInfo/Catalog 响应里的型号字段。 */
    const char *firmware;              /* DeviceInfo 响应里的固件版本字段。 */
    const char *channel_id;            /* 当前单通道实现里的通道编码。 */
    const char *user_agent;            /* SIP User-Agent。 */
    int queue_capacity;                /* 输出队列容量。 */
} MediaOutputGb28181Config;

typedef struct {
    MediaOutputType type;              /* 输出协议类型。 */
    union {
        MediaOutputRtspConfig rtsp;
        MediaOutputRtmpConfig rtmp;
        MediaOutputGb28181Config gb28181;
    } protocol;
} MediaOutputConfig;

typedef struct {
    const char *name;                         /* 输出通道名称，用于日志和统计信息标识。 */
    int queue_capacity;                       /* 发送队列容量。 */
    int reconnect_interval_ms;                /* 连接失败后的重试间隔，单位毫秒。 */
    int drop_until_keyframe_after_reconnect;  /* 重连后是否丢弃非关键帧，直到收到关键帧再恢复发送。 */
} MediaOutputChannelConfig;

typedef struct {
    uint64_t sent_frames;       /* 成功发送的帧数。 */
    uint64_t sent_bytes;        /* 成功发送的字节数。 */
    uint64_t dropped_frames;    /* 因队列满、等待关键帧等原因被丢弃的帧数。 */
    uint64_t reconnect_count;   /* 成功重连的次数。 */
    uint64_t send_failures;     /* 发送失败次数。 */
    int queue_depth;            /* 当前队列深度。 */
    int connected;              /* 当前输出通道是否已就绪。 */
    int waiting_for_keyframe;   /* 当前是否处于等待关键帧恢复发送的状态。 */
} MediaOutputStats;

typedef struct {
    int (*start)(MediaOutput *output);                            /* 协议输出启动钩子。 */
    int (*connect)(MediaOutput *output);                          /* 协议输出连接/重连钩子。 */
    int (*send_packet)(MediaOutput *output, const MediaPacket *packet); /* 协议输出发送钩子。 */
    void (*disconnect)(MediaOutput *output);                      /* 协议输出断开钩子。 */
    void (*stop)(MediaOutput *output);                            /* 协议输出停止钩子。 */
} MediaOutputVTable;

struct MediaOutput {
    MediaOutputType type;                    /* 输出协议类型。 */
    MediaOutputChannelConfig config;         /* 通用输出通道配置。 */
    MediaOutputStats stats;                  /* 运行时统计信息。 */
    const MediaOutputVTable *vtable;         /* 协议输出回调函数表。 */
    void *impl;                              /* 具体协议实现的私有上下文。 */
    pthread_t thread;                        /* 后台发送线程。 */
    pthread_mutex_t lock;                    /* 保护队列和统计信息的互斥锁。 */
    pthread_cond_t cond;                     /* 队列为空时用于阻塞/唤醒发送线程的条件变量。 */
    MediaPacket *queue;                      /* 环形队列，保存待发送的媒体包引用。 */
    int queue_capacity;                      /* 队列总容量。 */
    int queue_head;                          /* 当前队头下标。 */
    int queue_size;                          /* 当前队列中有效元素数量。 */
    int running;                             /* 发送线程是否已经启动。 */
    int stop_requested;                      /* 是否已请求发送线程退出。 */
    int connected;                           /* 当前输出通道是否已就绪。 */
    int waiting_for_keyframe;                /* 重连后是否仍在等待关键帧恢复发送。 */
};

/**
 * @description: 按协议类型创建并初始化一个输出通道。
 * @param {MediaOutput *} output 输出通道对象。
 * @param {const MediaOutputConfig *} config 输出协议配置。
 * @return {int} 0 成功，-1 失败。
 */
int media_output_setup(MediaOutput *output, const MediaOutputConfig *config);

/**
 * @description: 启动输出通道的协议资源和后台发送线程。
 * @param {MediaOutput *} output 输出通道对象。
 * @return {int} 0 成功，-1 失败。
 */
int media_output_start(MediaOutput *output);

/**
 * @description: 将媒体包引用压入输出队列，不复制底层媒体数据。
 * @param {MediaOutput *} output 输出通道对象。
 * @param {const MediaPacket *} packet 待发送媒体包。
 * @return {int} 0 成功或按丢帧策略丢弃，-1 参数非法。
 */
int media_output_enqueue(MediaOutput *output, const MediaPacket *packet);

/**
 * @description: 消费协议侧触发的外部 IDR 请求，例如新 RTSP 客户端接入或 GB28181 点播。
 * @param {MediaOutput *} output 输出通道对象。
 * @return {int} 1 需要请求编码器立即出 IDR，0 无请求。
 */
int media_output_consume_external_idr_request(MediaOutput *output);

/**
 * @description: 请求输出通道停止，并等待后台发送线程退出。
 * @param {MediaOutput *} output 输出通道对象。
 * @return {void}
 */
void media_output_stop(MediaOutput *output);

/**
 * @description: 释放输出通道队列、同步原语和通用状态。
 * @param {MediaOutput *} output 输出通道对象。
 * @return {void}
 */
void media_output_deinit(MediaOutput *output);

/**
 * @description: 读取输出通道运行统计信息。
 * @param {MediaOutput *} output 输出通道对象。
 * @param {MediaOutputStats *} stats 输出统计快照。
 * @return {void}
 */
void media_output_get_stats(MediaOutput *output, MediaOutputStats *stats);

/**
 * @description: 协议实现内部使用的通用输出通道初始化函数。
 * @param {MediaOutput *} output 输出通道对象。
 * @param {const MediaOutputChannelConfig *} config 通用通道配置。
 * @param {const MediaOutputVTable *} vtable 协议回调表。
 * @param {void *} impl 协议私有上下文。
 * @return {int} 0 成功，-1 失败。
 */
int media_output_init(MediaOutput *output,
                      const MediaOutputChannelConfig *config,
                      const MediaOutputVTable *vtable,
                      void *impl);

#ifdef __cplusplus
}
#endif

#endif
