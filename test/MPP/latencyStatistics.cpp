/**
 * @file latencyStatistics.cpp
 * @brief MPP性能测试延迟样本统计实现。
 */

#include "latencyStatistics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

/**
 * @brief 从已排序样本中按最近秩方法取得指定百分位。
 */
static uint64_t latencyPercentile(const std::vector<uint64_t> &sorted_samples,
                                  double ratio) {
    std::size_t index = 0;

    if (sorted_samples.empty() || ratio <= 0.0 || ratio > 1.0) {
        return 0;
    }
    index = static_cast<std::size_t>(
                std::ceil(ratio * static_cast<double>(sorted_samples.size()))) - 1U;
    if (index >= sorted_samples.size()) {
        index = sorted_samples.size() - 1U;
    }
    return sorted_samples[index];
}

bool calculateLatencyStatistics(const std::vector<uint64_t> &samples,
                                LatencyStatistics *statistics) {
    std::vector<uint64_t> sorted_samples;
    long double latency_sum = 0.0L;
    std::size_t sample_index = 0;

    if (statistics == NULL) {
        std::fprintf(stderr, "[ERROR] calculate latency statistics failed: output is null\n");
        return false;
    }
    std::memset(statistics, 0, sizeof(*statistics));
    if (samples.empty()) {
        return false;
    }

    sorted_samples = samples;
    std::sort(sorted_samples.begin(), sorted_samples.end());
    for (sample_index = 0; sample_index < sorted_samples.size(); ++sample_index) {
        latency_sum += static_cast<long double>(sorted_samples[sample_index]);
    }

    statistics->sample_count = sorted_samples.size();
    statistics->average_us = static_cast<uint64_t>(
        latency_sum / static_cast<long double>(sorted_samples.size()));
    statistics->p50_us = latencyPercentile(sorted_samples, 0.50);
    statistics->p95_us = latencyPercentile(sorted_samples, 0.95);
    statistics->p99_us = latencyPercentile(sorted_samples, 0.99);
    statistics->max_us = sorted_samples.back();
    return true;
}

uint64_t calculateUnaccountedLatency(uint64_t total_us,
                                     uint64_t known_stage_us) {
    if (known_stage_us >= total_us) {
        return 0;
    }
    return total_us - known_stage_us;
}
