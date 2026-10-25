/**
 * @file mediaGatewayVideoPipelineStats.h
 * @brief 正式视频编码pipeline运行统计接口。
 *
 * 模块接收编码worker已经发生的语义事件，统一计算deadline miss、连续超期和MPP阶段耗时。
 * 调用方负责使用所属运行时互斥锁保护记录及快照操作。
 */

#ifndef __MEDIA_GATEWAY_VIDEO_PIPELINE_STATS_H__
#define __MEDIA_GATEWAY_VIDEO_PIPELINE_STATS_H__

#include "commonDef.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单路视频编码pipeline从启动至当前时刻的累计统计快照。 */
typedef struct {
    uint64_t acquired_frames;                 /* 编码worker从帧源取得的帧数。 */
    uint64_t encoded_frames;                  /* MPP成功产生编码结果的帧数。 */
    uint64_t encode_failures;                 /* MPP编码调用失败的帧数。 */
    uint64_t overwrites_while_encoder_busy;   /* worker同步编码和分发期间帧源覆盖的旧帧数。 */
    uint64_t deadline_miss_frames;            /* 编码总耗时超过当前帧周期的帧数。 */
    uint64_t max_consecutive_deadline_misses; /* 连续deadline miss的历史峰值。 */
    uint64_t encode_total_us;                 /* 成功编码帧的累计总耗时。 */
    uint64_t encode_max_us;                   /* 单帧最大编码总耗时。 */
    uint64_t dmabuf_import_total_us;          /* DMA-BUF导入MPP的累计耗时。 */
    uint64_t submit_total_us;                 /* encode_put_frame累计调用耗时。 */
    uint64_t packet_wait_total_us;            /* encode_get_packet及轮询等待累计耗时。 */
    uint64_t packet_copy_total_us;            /* H.264编码包复制累计耗时。 */
} MediaGatewayVideoPipelineStats;

/** 编码器实现传给统计模块的耗时视图，避免统计接口依赖具体硬件编码器类型。 */
typedef struct {
    uint64_t input_import_us; /* DMA-BUF导入或其它输入准备耗时。 */
    uint64_t submit_us;       /* 向编码器提交输入帧的耗时。 */
    uint64_t packet_wait_us;  /* 等待编码输出包的耗时。 */
    uint64_t packet_copy_us;  /* 将编码输出复制到稳定缓存的耗时。 */
    uint64_t total_us;        /* 一次完整编码调用耗时。 */
} MediaGatewayVideoEncodeTiming;

/** 视频编码pipeline统计跟踪器。 */
typedef struct {
    MediaGatewayVideoPipelineStats totals; /* 对外可见的累计统计。 */
    uint64_t consecutive_deadline_misses;  /* 当前连续deadline miss次数。 */
} MediaGatewayVideoPipelineStatsTracker;

/** @brief 初始化单路视频编码pipeline统计跟踪器。 */
MediaResult media_gateway_video_pipeline_stats_init(
    MediaGatewayVideoPipelineStatsTracker *tracker);

/** @brief 记录编码worker成功从MediaFrameSource取得一帧。 */
MediaResult media_gateway_video_pipeline_stats_record_acquire(
    MediaGatewayVideoPipelineStatsTracker *tracker);

/**
 * @brief 根据worker同步编码和分发前后的帧源累计覆盖数记录本次覆盖增量。
 * @param tracker 单路统计跟踪器。
 * @param overwrites_before 编码开始前的帧源累计覆盖数。
 * @param overwrites_after 编码结束后的帧源累计覆盖数，不得小于开始值。
 */
MediaResult media_gateway_video_pipeline_stats_record_busy_overwrite_window(
    MediaGatewayVideoPipelineStatsTracker *tracker,
    uint64_t overwrites_before,
    uint64_t overwrites_after);

/**
 * @brief 记录一次MPP编码结果并按照当前帧周期判断deadline miss。
 * @param tracker 单路统计跟踪器。
 * @param frame_period_us 当前编码流一帧对应的时间，单位微秒。
 * @param encode_result MPP编码结果，MEDIA_OK表示成功，其它值表示失败。
 * @param timing MPP返回的分阶段耗时。
 */
MediaResult media_gateway_video_pipeline_stats_record_encode(
    MediaGatewayVideoPipelineStatsTracker *tracker,
    uint64_t frame_period_us,
    int encode_result,
    const MediaGatewayVideoEncodeTiming *timing);

/** @brief 获取单路视频编码pipeline累计统计快照。 */
MediaResult media_gateway_video_pipeline_stats_snapshot(
    const MediaGatewayVideoPipelineStatsTracker *tracker,
    MediaGatewayVideoPipelineStats *snapshot);

#ifdef __cplusplus
}
#endif

#endif
