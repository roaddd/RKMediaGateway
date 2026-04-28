#ifndef __GB28181_DEVICE_H__
#define __GB28181_DEVICE_H__

#ifdef __cplusplus
extern "C" {
#endif

struct eXosip_t;

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "mppEncoder.h"
#include "v4l2Capture.h"

/**
 * @brief GB28181 设备侧运行参数。
 *
 * 该配置同时覆盖 SIP 信令参数与媒体发送参数。
 * 当 `external_media_input=1` 时，采集/编码由外部模块负责，
 * 当前模块仅负责 SIP + PS/RTP 封装与发送。
 */
typedef struct {
    const char *server_ip;            /* SIP 服务器 IP（例如 WVP 地址）。 */
    int server_port;                  /* SIP 服务器端口，常用 5060。 */
    const char *server_id;            /* 平台国标 ID。 */
    const char *server_domain;        /* SIP 域（realm / domain）。 */
    const char *device_id;            /* 设备国标 ID。 */
    const char *device_domain;        /* 设备域，未设置时通常跟随 server_domain。 */
    const char *device_password;      /* SIP Digest 鉴权密码。 */
    const char *bind_ip;              /* 本地 SIP 监听绑定地址。 */
    int local_sip_port;               /* 本地 SIP 监听端口。 */
    const char *sip_contact_ip;       /* SIP Contact 中对外声明的 IP。 */
    const char *media_ip;             /* SDP 中声明的媒体发送 IP。 */
    int media_port;                   /* RTP 本地绑定端口（同时写入 SDP）。 */
    int register_expires;             /* REGISTER 过期时间（秒）。 */
    int keepalive_interval_sec;       /* Keepalive 发送周期（秒）。 */
    int register_retry_interval_sec;  /* 注册失败重试周期（秒）。 */
    const char *device_name;          /* DeviceInfo/Catalog 返回的设备名。 */
    const char *manufacturer;         /* 厂商字段。 */
    const char *model;                /* 型号字段。 */
    const char *firmware;             /* 固件版本字段。 */
    const char *channel_id;           /* 目录里返回的通道 ID。 */
    const char *user_agent;           /* SIP User-Agent。 */
    int fps;                          /* 编码帧率（用于本地编码模式与时间戳节奏）。 */
    int bitrate;                      /* 编码码率（用于本地编码模式）。 */
    int gop;                          /* GOP 长度（用于本地编码模式）。 */
    int h264_profile;                 /* H264 Profile（用于本地编码模式）。 */
    int h264_level;                   /* H264 Level（用于本地编码模式）。 */
    int h264_cabac_en;                /* CABAC 开关（用于本地编码模式）。 */
    int external_media_input;         /* 1: 外部注入 H264，不初始化 V4L2/MPP。 */
} Gb28181DeviceConfig;

/**
 * @brief 当前点播会话状态。
 */
typedef struct {
    int active;                       /* 是否存在有效会话。 */
    int established;                  /* 是否收到 ACK，进入可发流状态。 */
    int cid;                          /* eXosip call-id。 */
    int did;                          /* eXosip dialog-id。 */
    int tid;                          /* eXosip transaction-id。 */
    char remote_ip[64];               /* 对端 SDP 中的视频接收 IP。 */
    int remote_port;                  /* 对端 SDP 中的视频接收端口。 */
    char remote_ssrc[32];             /* 对端 SDP 里声明的 SSRC（可选）。 */
    char local_ssrc[32];              /* 本端在 SDP 中声明的 SSRC。 */
    char transport[32];               /* SDP 传输描述（例如 RTP/AVP）。 */
    int rtp_socket_fd;                /* RTP UDP socket。 */
    unsigned short rtp_sequence;      /* RTP sequence，逐包递增。 */
    unsigned int rtp_ssrc;            /* RTP SSRC 数值形式。 */
    unsigned int last_rtp_timestamp;  /* 最近一次发送使用的 RTP 时间戳。 */
} Gb28181MediaSession;

/**
 * @brief GB28181 设备运行时上下文。
 */
typedef struct {
    struct eXosip_t *sip_context;     /* eXosip SIP 协议栈上下文。 */
    V4L2CaptureCtx *capture;          /* 本地采集上下文（外部输入模式下可为 NULL）。 */
    MppEncoderCtx *encoder;           /* 本地编码上下文（外部输入模式下可为 NULL）。 */
    Gb28181DeviceConfig config;       /* 归一化后的配置副本。 */
    Gb28181MediaSession media_session;/* 当前媒体会话快照。 */
    int rid;                          /* 注册事务 ID。 */
    int running;                      /* 主循环运行标记。 */
    int registered_ok;                /* 最近一次注册是否成功。 */
    int capture_ready;                /* capture 是否已成功初始化。 */
    int encoder_ready;                /* encoder 是否已成功初始化。 */
    int pending_force_idr;            /* ACK 建立后待执行的一次性 IDR 请求标记。 */
    int external_idr_requested;       /* external 模式下该请求是否已转交上游编码器。 */
    int sync_ready;                   /* 互斥锁/条件变量是否可用。 */
    int media_thread_started;         /* 媒体线程是否已创建。 */
    unsigned int xml_sn;              /* XML 消息流水号。 */
    long long next_keepalive_ms;      /* 下一次 keepalive 的绝对时间（毫秒）。 */
    long long next_register_retry_ms; /* 下一次注册重试时间（毫秒）。 */
    pthread_t media_thread;           /* 本地采集编码发流线程。 */
    pthread_mutex_t session_lock;     /* 保护 media_session 的互斥锁。 */
    pthread_cond_t session_cond;      /* SIP 与媒体线程之间的唤醒条件。 */
} Gb28181DeviceCtx;

/**
 * @brief 初始化 GB28181 设备模块。
 * @param ctx 设备上下文，调用前可为未初始化内存。
 * @param config 输入配置，可为 NULL（走默认值）。
 * @return 0 成功，<0 失败。
 */
int gb28181_device_init(Gb28181DeviceCtx *ctx, const Gb28181DeviceConfig *config);

/**
 * @brief 运行 SIP 事件循环（阻塞）。
 * @param ctx 设备上下文。
 * @return 0 正常退出，<0 异常退出。
 */
int gb28181_device_run(Gb28181DeviceCtx *ctx);

/**
 * @brief 请求停止模块运行。
 * @param ctx 设备上下文。
 */
void gb28181_device_stop(Gb28181DeviceCtx *ctx);

/**
 * @brief 释放模块资源。
 * @param ctx 设备上下文。
 */
void gb28181_device_deinit(Gb28181DeviceCtx *ctx);

/**
 * @brief 读取当前媒体会话快照。
 * @param ctx 设备上下文。
 * @param session 输出会话快照。
 */
void gb28181_device_get_media_session(const Gb28181DeviceCtx *ctx, Gb28181MediaSession *session);

/**
 * @brief 外部输入 H264（Annex-B）帧并发送为 GB28181 PS/RTP。
 *
 * 该接口用于复用外部编码链路（例如 mediaGateway 的共享编码输出）。
 * 函数内部会根据当前会话状态决定是否发送：
 * - 无有效会话时直接返回 0（不报错）；
 * - 有效会话时执行 PS 封装 + RTP 发送。
 *
 * @param ctx 设备上下文。
 * @param h264_data Annex-B 格式 H264 数据。
 * @param h264_len 数据长度。
 * @param is_key_frame 是否关键帧（IDR）。
 * @param pts_us 时间戳（微秒）。
 * @return 0 成功或无需发送，<0 发送链路异常。
 */
int gb28181_device_send_h264(Gb28181DeviceCtx *ctx,
                             const uint8_t *h264_data,
                             size_t h264_len,
                             int is_key_frame,
                             uint64_t pts_us);

/*
 * external 模式下：查询并“消费”一次 ACK 触发的 IDR 请求。
 * 返回 1 表示上游应立即请求一次 IDR；返回 0 表示当前无需请求。
 */
int gb28181_device_consume_external_idr_request(Gb28181DeviceCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
