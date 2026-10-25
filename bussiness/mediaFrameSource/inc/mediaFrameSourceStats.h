/**
 * @file mediaFrameSourceStats.h
 * @brief 视频帧源运行统计及V4L2采集序号连续性分析接口。
 *
 * 统计模块只处理已经发生的帧源事件，不直接依赖V4L2设备或编码器。生产帧源负责记录
 * 事件，调试命令和测试程序通过快照读取结果。
 */

#ifndef __MEDIA_FRAME_SOURCE_STATS_H__
#define __MEDIA_FRAME_SOURCE_STATS_H__

#include "commonDef.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 视频帧源从启动至当前时刻的累计统计快照。 */
typedef struct {
    uint64_t captured_frames;       /* V4L2通过DQBUF实际交付给应用的帧数。 */
    uint64_t sequence_gap_events;   /* V4L2 sequence发生向前跳变的次数。 */
    uint64_t sequence_gap_frames;   /* 根据sequence跳变量计算的累计缺失帧数。 */
    uint32_t last_capture_sequence; /* 最近一次记录的V4L2 capture sequence。 */
    uint64_t published_frames;      /* 成功发布到latest-frame缓存的新帧数。 */
    uint64_t overwritten_frames;    /* 尚未消费便被更新帧替换的旧帧数。 */
    uint64_t no_writable_slot_drops; /* 所有槽位都在使用而无法发布的新帧数。 */
    uint64_t acquired_frames;       /* 编码worker成功取得的最新帧数。 */
} MediaFrameSourceStats;

/**
 * 视频帧源统计跟踪器。
 *
 * 调用方应使用自身已有的互斥锁保护记录和快照操作，统计模块内部不重复创建锁。
 */
typedef struct {
    MediaFrameSourceStats totals; /* 对外返回的累计统计。 */
    int has_capture_sequence;     /* 是否已经记录过首个V4L2 sequence。 */
} MediaFrameSourceStatsTracker;

/**
 * @brief 初始化视频帧源统计跟踪器。
 * @param tracker 待初始化的统计跟踪器。
 * @return 成功返回MEDIA_OK；参数为空返回MEDIA_ERR_INVALID_PARAM。
 */
MediaResult media_frame_source_stats_init(MediaFrameSourceStatsTracker *tracker);

/**
 * @brief 记录一帧由V4L2交付的采集帧并分析sequence连续性。
 * @param tracker 已初始化的统计跟踪器。
 * @param sequence 当前V4L2 buffer携带的sequence。
 * @return 成功返回MEDIA_OK；参数为空返回MEDIA_ERR_INVALID_PARAM。
 */
MediaResult media_frame_source_stats_record_capture(MediaFrameSourceStatsTracker *tracker,
                                                    uint32_t sequence);

/** @brief 记录一次成功发布到latest-frame缓存的事件。 */
MediaResult media_frame_source_stats_record_publish(MediaFrameSourceStatsTracker *tracker);

/** @brief 记录一帧尚未消费便被更新帧替换的旧帧。 */
MediaResult media_frame_source_stats_record_overwrite(MediaFrameSourceStatsTracker *tracker);

/** @brief 记录一次所有帧源槽位均在使用而无法发布新帧的事件。 */
MediaResult media_frame_source_stats_record_no_writable_slot_drop(
    MediaFrameSourceStatsTracker *tracker);

/** @brief 记录编码worker成功取得一帧最新帧的事件。 */
MediaResult media_frame_source_stats_record_acquire(MediaFrameSourceStatsTracker *tracker);

/**
 * @brief 获取当前累计统计快照。
 * @param tracker 统计跟踪器。
 * @param snapshot 输出统计快照。
 * @return 成功返回MEDIA_OK；参数非法返回MEDIA_ERR_INVALID_PARAM。
 */
MediaResult media_frame_source_stats_snapshot(const MediaFrameSourceStatsTracker *tracker,
                                              MediaFrameSourceStats *snapshot);

#ifdef __cplusplus
}
#endif

#endif
