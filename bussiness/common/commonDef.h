/**
 * @file commonDef.h
 * @brief 全工程公共基础定义。
 *
 * 本文件放置跨模块共享的基础类型、公共错误码和轻量级约定。
 * 业务模块需要表达函数执行状态时，应优先使用这里定义的 MEDIA_OK/MEDIA_ERR_*。
 */

#ifndef __COMMON_DEF_H__
#define __COMMON_DEF_H__

/**
 * @brief 全工程公共返回码。
 *
 * 约定所有只表示执行状态的 int 返回值使用该枚举：
 * MEDIA_OK 表示成功，其他负数表示失败原因。
 * 如果函数成功时需要返回业务数值，例如 GOP、队列深度、长度等，可以只在失败时返回这些错误码。
 */
typedef enum {
    MEDIA_OK = 0,                 /* 执行成功。 */
    MEDIA_ERR = -1,               /* 通用失败，无法归类到更具体错误。 */
    MEDIA_ERR_INVALID_PARAM = -2, /* 输入参数非法，例如 NULL 指针或越界索引。 */
    MEDIA_ERR_INVALID_CONFIG = -3,/* 配置非法，例如帧率、码率、GOP 时间间隔为 0。 */
    MEDIA_ERR_NO_MEMORY = -4,     /* 内存申请失败。 */
    MEDIA_ERR_NOT_READY = -5,     /* 模块尚未初始化或资源未就绪。 */
    MEDIA_ERR_BUSY = -6,          /* 资源忙，当前无法执行操作。 */
    MEDIA_ERR_TIMEOUT = -7,       /* 操作超时。 */
    MEDIA_ERR_NOT_FOUND = -8,     /* 未找到指定对象。 */
    MEDIA_ERR_UNSUPPORTED = -9,   /* 当前平台或模块不支持该操作。 */
    MEDIA_ERR_FULL = -10,         /* 队列、缓存或表项已满。 */
    MEDIA_ERR_STOPPED = -11       /* 模块或队列已停止。 */
} MediaResult;

#define DEFAULT_ENABLE_RTSP 1                         /* 默认启用 RTSP 输出。 */
#define DEFAULT_ENCODE_FPS 30                         /* 默认编码帧率，同时用于采集、MPP 和 RTSP 流元数据。 */
#define DEFAULT_ENCODE_BITRATE (2 * 1024 * 1024)      /* 默认编码目标码率，单位 bps。 */
#define DEFAULT_ENCODE_GOP 30                         /* 默认 GOP 长度，通常与帧率一致以保持约 1 秒关键帧间隔。 */
#define DEFAULT_RC_MODE MPP_ENC_RC_MODE_CBR           /* 默认 MPP 编码码率控制模式。 */
#define DEFAULT_H264_PROFILE 100                      /* 默认 H.264 Profile，100 表示 High Profile。 */
#define DEFAULT_H264_LEVEL 40                         /* 默认 H.264 Level，40 表示 Level 4.0。 */
#define DEFAULT_H264_CABAC_EN 1                       /* 默认启用 H.264 CABAC 熵编码。 */
#define DEFAULT_LOW_LATENCY_MODE 1                    /* 默认启用低延迟模式。 */
#define DEFAULT_STATS_INTERVAL_SEC 1                  /* 默认运行统计打印间隔，单位秒。 */
#define DEFAULT_CAPTURE_RETRY_MS 5                    /* 视频采集失败后的默认重试间隔，单位毫秒。 */
#define DEFAULT_MAX_CONSECUTIVE_FAILURES 30           /* 视频采集连续失败达到该次数后认为源异常。 */
#define DEFAULT_RECORD_FLUSH_INTERVAL_FRAMES 30       /* 录像输出默认按该帧数间隔 flush。 */
#define DEFAULT_BENCH_ENABLE 0                        /* 默认关闭 benchmark 统计。 */
#define DEFAULT_BENCH_SAMPLE_EVERY 1                  /* benchmark 默认采样间隔，1 表示每帧采样。 */
#define DEFAULT_BENCH_PRINT_INTERVAL_SEC 1            /* benchmark 默认打印间隔，单位秒。 */
#define DEFAULT_ENABLE_AUDIO 1                        /* 默认启用音频采集和封装。 */
#define DEFAULT_AUDIO_BIND_STREAM_INDEX 0             /* 音频默认绑定的视频流索引。 */
#define DEFAULT_AUDIO_RETRY_MS 5                      /* 音频采集失败后的默认重试间隔，单位毫秒。 */
#define DEFAULT_AUDIO_MAX_CONSECUTIVE_FAILURES 30     /* 音频采集连续失败达到该次数后认为源异常。 */
#define DEFAULT_DYNAMIC_FPS_EVALUATE_INTERVAL_MS 1000 /* 动态帧率状态机默认评估间隔，单位毫秒。 */
#define DEFAULT_DYNAMIC_FPS_MIN_SWITCH_INTERVAL_MS 30000 /* 两次成功帧率切换之间的默认最小间隔，单位毫秒。 */
#define DEFAULT_DYNAMIC_FPS_BRIGHT_CONFIRM_MS 1000       /* 亮光场景切换到 60fps 前的默认确认时长，单位毫秒。 */
#define DEFAULT_DYNAMIC_FPS_LOW_LIGHT_CONFIRM_MS 15000   /* 低照场景切换到 15fps 前的默认确认时长，单位毫秒。 */
#define DEFAULT_ADAPTIVE_MIN_BITRATE (512 * 1024)        /* 联合控制默认最低码率，单位 bit/s。 */
#define DEFAULT_ADAPTIVE_MAX_BITRATE (8 * 1024 * 1024)   /* 联合控制默认最高码率，单位 bit/s。 */
#define MEDIA_GATEWAY_RESULT_QUEUE_CAPACITY 32           /* worker 执行结果队列容量。 */
#define MEDIA_GATEWAY_MAX_RESULTS_PER_LOOP 16            /* 单次主循环最多处理的执行结果数量。 */
#define MEDIA_GATEWAY_COMMAND_RETRY_MS 1000              /* 控制命令失败后的重试间隔。 */

#endif /* __COMMON_DEF_H__ */
