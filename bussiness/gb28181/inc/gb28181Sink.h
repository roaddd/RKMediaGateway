#ifndef __GB28181_SINK_H__
#define __GB28181_SINK_H__

#include "mediaSink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;                  /* sink 名称，便于日志和运行统计定位。 */
    const char *server_ip;             /* GB28181/SIP 平台地址，当前默认对接 WVP。 */
    int server_port;                   /* SIP 服务端口，WVP 默认通常为 5060。 */
    const char *server_id;             /* 平台国标编码，用于构造 To/Request-URI。 */
    const char *server_domain;         /* 平台域，Digest 鉴权和 SIP URI 会复用该值。 */
    const char *device_id;             /* 设备国标编码。 */
    const char *device_domain;         /* 设备所属域，未配置时默认跟随 server_domain。 */
    const char *device_password;       /* SIP Digest 鉴权密码。 */
    const char *bind_ip;               /* 本地 SIP 监听绑定地址，通常使用 0.0.0.0。 */
    int local_sip_port;                /* 本地 SIP 监听端口。 */
    const char *sip_contact_ip;        /* SIP Contact 头中对外声明的设备 IP。 */
    const char *media_ip;              /* SDP 中对外声明的媒体发送 IP。 */
    int media_port;                    /* 本地 RTP 绑定端口，同时也是 SDP 中声明的媒体端口。 */
    int register_expires;              /* REGISTER 有效期，单位秒。 */
    int keepalive_interval_sec;        /* Keepalive 周期，单位秒。 */
    int register_retry_interval_sec;   /* 注册失败后的重试间隔，单位秒。 */
    const char *device_name;           /* DeviceInfo/Catalog 响应里的设备名。 */
    const char *manufacturer;          /* DeviceInfo/Catalog 响应里的厂商字段。 */
    const char *model;                 /* DeviceInfo/Catalog 响应里的型号字段。 */
    const char *firmware;              /* DeviceInfo 响应里的固件版本字段。 */
    const char *channel_id;            /* 当前单通道实现里的通道编码。 */
    const char *user_agent;            /* SIP User-Agent。 */
    int queue_capacity;                /* GB28181 sink 自己的发送队列容量。 */
} Gb28181SinkConfig;

int gb28181_sink_setup(MediaSink *sink, const Gb28181SinkConfig *config);

/* external 模式下供 mediaGateway 轮询：是否需要立刻请求一次 IDR。 */
int gb28181_sink_consume_external_idr_request(MediaSink *sink);

#ifdef __cplusplus
}
#endif

#endif
