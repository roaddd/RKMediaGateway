#ifndef __MEDIA_OUTPUT_H__
#define __MEDIA_OUTPUT_H__

#include <pthread.h>
#include <stdint.h>

#include "mediaPacket.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @description: 输出通道协议类型。
 */
typedef enum {
    MEDIA_OUTPUT_TYPE_RTSP = 0,    /* RTSP 服务端输出。 */
    MEDIA_OUTPUT_TYPE_RTMP = 1,    /* RTMP 推流输出。 */
    MEDIA_OUTPUT_TYPE_GB28181 = 2  /* GB28181/SIP+RTP 设备输出。 */
} MediaOutputType;

typedef struct MediaOutput MediaOutput;

/**
 * @description: RTSP 输出配置。
 */
typedef struct {
    const char *name;         /* 输出通道名称，用于日志和统计。 */
    const char *session_name; /* RTSP URL 中的 session 名称。 */
    const char *server_ip;    /* RTSP 服务监听地址。 */
    int server_port;          /* RTSP 服务监听端口。 */
    int auth_enable;          /* 是否启用 RTSP 鉴权。 */
    const char *user;         /* 鉴权用户名。 */
    const char *password;     /* 鉴权密码。 */
    int queue_capacity;       /* 单类媒体队列容量；音频和视频各自拥有该容量。 */
    int immediate_sps_pps_on_new_client; /* 新客户端接入时是否立即补发缓存的 SPS/PPS。 */
    MediaCodecType audio_codec; /* RTSP SDP 中声明的音频编码，支持 G711A/PCMA 或 AAC。 */
    int audio_sample_rate;      /* RTSP 音频采样率。 */
    int audio_channels;         /* RTSP 音频声道数。 */
    int aac_profile;            /* AAC object type，2 表示 AAC-LC。 */
    struct NetFeedbackCbInfo *feedback_holder; /* RTCP/协议网络反馈回调入口，由业务层注入。 */
} MediaOutputRtspConfig;

/**
 * @description: RTMP 输出配置。
 */
typedef struct {
    const char *name;          /* 输出通道名称，用于日志和统计。 */
    const char *publish_url;   /* RTMP 推流地址，例如 rtmp://host/app/stream。 */
    int queue_capacity;        /* 单类媒体队列容量；音频和视频各自拥有该容量。 */
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

/**
 * @description: GB28181 输出配置。
 */
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
    int queue_capacity;                /* 单类媒体队列容量；音频和视频各自拥有该容量。 */
} MediaOutputGb28181Config;

/**
 * @description: 输出通道创建配置，按 type 选择对应协议配置块。
 */
typedef struct {
    MediaOutputType type;              /* 输出协议类型。 */
    union {
        MediaOutputRtspConfig rtsp;
        MediaOutputRtmpConfig rtmp;
        MediaOutputGb28181Config gb28181;
    } protocol;
} MediaOutputConfig;

/**
 * @description: 协议实现传给通用输出层的公共通道配置。
 */
typedef struct {
    const char *name;                         /* 输出通道名称，用于日志和统计。 */
    int queue_capacity;                       /* 单类媒体队列容量；音频和视频各自拥有该容量。 */
    int reconnect_interval_ms;                /* 连接失败后的重试间隔，单位毫秒。 */
    int drop_until_keyframe_after_reconnect;  /* 重连后是否丢弃非关键帧，直到关键帧恢复发送。 */
    struct NetFeedbackCbInfo *feedback_holder; /* 输出层通用网络反馈回调入口。 */
} MediaOutputChannelConfig;

/* 输出协议上报给业务层的网络反馈快照。 */
typedef struct {
    MediaOutputType output_type; /* 输出协议类型，例如 RTSP/RTMP/GB28181。 */
    const char *output_name;     /* 输出通道名称。 */
    const char *session_name;    /* 协议会话名称，RTSP 中对应 URL session。 */
    const char *client_ip;       /* 反馈来源客户端 IP。 */
    int is_audio;                /* 0 表示视频反馈，1 表示音频反馈。 */
    uint8_t fraction_lost;       /* RTCP RR fraction lost，0~255 对应 0~100%。 */
    int32_t cumulative_lost;     /* RTCP RR 累计丢包数。 */
    uint32_t jitter;             /* RTCP RR 抖动，单位为对应 RTP 时钟 tick。 */
    uint32_t rtt_ms;             /* RTCP 推算 RTT，单位毫秒。 */
    uint64_t rr_count;           /* 累计 RR 数量。 */
} MediaOutputNetFeedbackInfo;

/* 输出协议网络反馈回调。输出层只上报指标，不直接参与编码自适应决策。 */
typedef void (*NetFeedbackCb)(const MediaOutputNetFeedbackInfo *feedback,
                                                   void *userdata);

/* 回调和业务私有上下文打包对象，便于协议 output 在创建时透传。 */
typedef struct NetFeedbackCbInfo {
    NetFeedbackCb callback; /* 业务层反馈处理函数。 */
    void *userdata;                              /* 业务层私有上下文。 */
} NetFeedbackCbInfo;

/**
 * @description: 输出通道运行统计快照。
 */
typedef struct {
    uint64_t sent_frames;       /* 成功发送的媒体包数量。 */
    uint64_t sent_bytes;        /* 成功发送的媒体负载字节数。 */
    uint64_t dropped_frames;    /* 因队列满、等待关键帧等原因丢弃的媒体包数量。 */
    uint64_t reconnect_count;   /* 成功连接或重连次数。 */
    uint64_t send_failures;     /* 协议发送失败次数。 */
    int queue_depth;            /* 当前音频队列和视频队列深度之和。 */
    int connected;              /* 当前输出通道是否已连接。 */
    int waiting_for_keyframe;   /* 当前是否处于等待关键帧恢复发送的状态。 */
} MediaOutputStats;

/**
 * @description: 具体协议输出实现的回调表。
 */
typedef struct {
    int (*start)(MediaOutput *output);                            /* 启动协议私有资源。 */
    int (*connect)(MediaOutput *output);                          /* 建立或恢复下游连接。 */
    int (*send_packet)(MediaOutput *output, const MediaPacket *packet); /* 发送一个媒体包。 */
    void (*disconnect)(MediaOutput *output);                      /* 断开下游连接。 */
    void (*stop)(MediaOutput *output);                            /* 停止协议私有资源。 */
} MediaOutputVTable;

/**
 * @description: 单类媒体环形队列，音频和视频在 MediaOutput 中各使用一个实例。
 */
typedef struct {
    MediaPacket *items; /* 环形队列槽位，保存 MediaPacket 引用副本。 */
    int capacity;       /* 队列总容量。 */
    int head;           /* 当前队头下标。 */
    int size;           /* 当前有效元素数量。 */
} MediaOutputPacketQueue;

/**
 * @description: 通用输出通道对象，封装队列、发送线程、统计和协议回调。
 */
struct MediaOutput {
    MediaOutputType type;                    /* 输出协议类型。 */
    MediaOutputChannelConfig config;         /* 通用输出通道配置。 */
    MediaOutputStats stats;                  /* 运行期统计信息。 */
    const MediaOutputVTable *vtable;         /* 协议输出回调函数表。 */
    void *impl;                              /* 具体协议实现的私有上下文。 */
    pthread_t thread;                        /* 后台发送线程。 */
    pthread_mutex_t lock;                    /* 保护队列和统计信息的互斥锁。 */
    pthread_cond_t cond;                     /* 队列为空时阻塞/唤醒发送线程的条件变量。 */
    MediaOutputPacketQueue video_queue;      /* 视频包队列。 */
    MediaOutputPacketQueue audio_queue;      /* 音频包队列。 */
    int running;                             /* 发送线程是否已经启动。 */
    int stop_requested;                      /* 是否已请求发送线程退出。 */
    int connected;                           /* 当前输出通道是否已连接。 */
    int waiting_for_keyframe;                /* 重连后是否仍在等待关键帧恢复发送。 */
};

/**
 * @description: 按协议类型创建并初始化一个输出通道。
 * @param output 输出通道对象。
 * @param config 输出协议配置。
 * @return 0 成功，-1 失败。
 */
int media_output_setup(MediaOutput *output, const MediaOutputConfig *config);

/**
 * @description: 启动输出通道协议资源和后台发送线程。
 * @param output 输出通道对象。
 * @return 0 成功，-1 失败。
 */
int media_output_start(MediaOutput *output);

/**
 * @description: 将媒体包引用压入对应音频/视频输出队列，不复制底层媒体数据。
 * @param output 输出通道对象。
 * @param packet 待发送媒体包。
 * @return 0 成功或按丢帧策略丢弃，-1 参数非法或入队失败。
 */
int media_output_enqueue(MediaOutput *output, const MediaPacket *packet);

/**
 * @description: 消费协议侧触发的外部 IDR 请求，例如新 RTSP 客户端接入或 GB28181 点播。
 * @param output 输出通道对象。
 * @return 1 需要请求编码器立即出 IDR，0 无请求。
 */
int media_output_consume_external_idr_request(MediaOutput *output);

/**
 * @description: 请求输出通道停止，并等待后台发送线程退出。
 * @param output 输出通道对象。
 */
void media_output_stop(MediaOutput *output);

/**
 * @description: 释放输出通道队列、同步原语和通用状态。
 * @param output 输出通道对象。
 */
void media_output_deinit(MediaOutput *output);

/**
 * @description: 读取输出通道运行统计快照。
 * @param output 输出通道对象。
 * @param stats 输出统计快照。
 */
void media_output_get_stats(MediaOutput *output, MediaOutputStats *stats);

/**
 * @description: 协议实现内部使用的通用输出通道初始化函数。
 * @param output 输出通道对象。
 * @param config 通用输出通道配置。
 * @param vtable 协议回调表。
 * @param impl 协议私有上下文。
 * @return 0 成功，-1 失败。
 */
int media_output_init(MediaOutput *output,
                      const MediaOutputChannelConfig *config,
                      const MediaOutputVTable *vtable,
                      void *impl);

#ifdef __cplusplus
}
#endif

#endif
