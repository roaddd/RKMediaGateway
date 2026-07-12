/*** 
 * @Author: huangkelong
 * @Date: 2026-07-12 10:06:16
 * @LastEditTime: 2026-07-12 10:06:22
 * @LastEditors: huangkelong
 * @Description: 全工程公共定义头文件
 * @FilePath: \Fork\RKMediaGateway\bussiness\common\commonDef.h
 * @可以输入预定的版权声明、个性签名、空行等
 */

#ifndef __COMMON_DEF_H__
#define __COMMON_DEF_H__

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
