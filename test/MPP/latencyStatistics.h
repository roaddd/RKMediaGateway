/**
 * @file latencyStatistics.h
 * @brief MPP性能测试使用的延迟样本统计接口。
 */

#ifndef TEST_MPP_LATENCY_STATISTICS_H
#define TEST_MPP_LATENCY_STATISTICS_H

#include <cstddef>
#include <cstdint>
#include <vector>

/** 一组微秒延迟样本的汇总结果。 */
struct LatencyStatistics {
    std::size_t sample_count; /* 有效样本数量。 */
    uint64_t average_us;      /* 算术平均耗时，单位微秒。 */
    uint64_t p50_us;          /* 第50百分位耗时，单位微秒。 */
    uint64_t p95_us;          /* 第95百分位耗时，单位微秒。 */
    uint64_t p99_us;          /* 第99百分位耗时，单位微秒。 */
    uint64_t max_us;          /* 最大耗时，单位微秒。 */
};

/**
 * @brief 计算一组微秒延迟样本的平均值和百分位。
 * @param samples 原始延迟样本，函数内部排序副本，不修改调用方数据。
 * @param statistics 输出统计结果。
 * @return true表示至少包含一个样本；参数为空或没有样本时返回false。
 */
bool calculateLatencyStatistics(const std::vector<uint64_t> &samples,
                                LatencyStatistics *statistics);

/**
 * @brief 计算总耗时中未被已知阶段覆盖的剩余耗时。
 * @param total_us 整次调用总耗时。
 * @param known_stage_us 已知阶段耗时之和。
 * @return total_us与known_stage_us的非负差值；阶段和超过总耗时时返回0。
 */
uint64_t calculateUnaccountedLatency(uint64_t total_us,
                                     uint64_t known_stage_us);

#endif
