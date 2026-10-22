/**
 * @file latency_statistics_test.cpp
 * @brief MPP延迟统计计算的独立单元测试。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "latencyStatistics.h"

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "[FAIL] %s\n", message); \
            return EXIT_FAILURE; \
        } \
    } while (0)

/** @brief 验证固定样本的平均值和最近秩百分位。 */
static int testKnownLatencyDistribution() {
    std::vector<uint64_t> samples;
    LatencyStatistics statistics = {};
    int value = 0;
    bool result = false;

    for (value = 1; value <= 100; ++value) {
        samples.push_back(static_cast<uint64_t>(value));
    }
    result = calculateLatencyStatistics(samples, &statistics);
    TEST_ASSERT(result, "known distribution must be accepted");
    TEST_ASSERT(statistics.sample_count == 100U, "known distribution sample count");
    TEST_ASSERT(statistics.average_us == 50U, "known distribution integer average");
    TEST_ASSERT(statistics.p50_us == 50U, "known distribution p50");
    TEST_ASSERT(statistics.p95_us == 95U, "known distribution p95");
    TEST_ASSERT(statistics.p99_us == 99U, "known distribution p99");
    TEST_ASSERT(statistics.max_us == 100U, "known distribution maximum");
    return EXIT_SUCCESS;
}

/** @brief 验证空样本和空输出参数会被拒绝且输出保持清零。 */
static int testInvalidLatencyDistribution() {
    std::vector<uint64_t> samples;
    LatencyStatistics statistics;
    bool result = true;

    std::memset(&statistics, 0x7f, sizeof(statistics));
    result = calculateLatencyStatistics(samples, &statistics);
    TEST_ASSERT(!result, "empty distribution must be rejected");
    TEST_ASSERT(statistics.sample_count == 0U, "empty distribution output reset");
    TEST_ASSERT(!calculateLatencyStatistics(samples, NULL), "null output must be rejected");
    return EXIT_SUCCESS;
}

/** @brief 验证未归属耗时使用饱和减法，避免无符号下溢。 */
static int testUnaccountedLatency() {
    TEST_ASSERT(calculateUnaccountedLatency(100U, 70U) == 30U,
                "unaccounted latency normal difference");
    TEST_ASSERT(calculateUnaccountedLatency(100U, 100U) == 0U,
                "unaccounted latency exact coverage");
    TEST_ASSERT(calculateUnaccountedLatency(100U, 120U) == 0U,
                "unaccounted latency saturated difference");
    return EXIT_SUCCESS;
}

int main() {
    TEST_ASSERT(testKnownLatencyDistribution() == EXIT_SUCCESS,
                "known latency distribution test");
    TEST_ASSERT(testInvalidLatencyDistribution() == EXIT_SUCCESS,
                "invalid latency distribution test");
    TEST_ASSERT(testUnaccountedLatency() == EXIT_SUCCESS,
                "unaccounted latency test");
    std::printf("latency statistics tests passed\n");
    return EXIT_SUCCESS;
}
