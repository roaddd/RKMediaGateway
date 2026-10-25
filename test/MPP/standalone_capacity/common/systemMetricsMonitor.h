/**
 * @file systemMetricsMonitor.h
 * @brief MPP容量测试使用的RK3568系统状态采样模块。
 *
 * 该模块隐藏Linux procfs、sysfs和debugfs节点发现及解析细节。调用方只需
 * 配置采样周期和CSV文件，然后在测试前后启动、停止并打印汇总结果。
 */

#ifndef TEST_MPP_SYSTEM_METRICS_MONITOR_H
#define TEST_MPP_SYSTEM_METRICS_MONITOR_H

#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

/** 系统指标监控配置。 */
struct SystemMetricsConfig {
    int sample_interval_ms;  /* 相邻两次采样的目标间隔，单位毫秒。 */
    std::string csv_path;    /* 逐次采样结果的CSV输出路径。 */
};

/**
 * @brief 周期采集SoC温度、CPU、DDR、RKVENC和内存状态。
 *
 * 单项系统节点缺失不会导致测试失败：对应CSV字段写为N/A，并且只在启动
 * 或首次读取失败时打印警告。CSV每次采样后立即刷新，异常终止时也尽量
 * 保留已经完成的样本。
 */
class SystemMetricsMonitor {
public:
    SystemMetricsMonitor();
    ~SystemMetricsMonitor();

    SystemMetricsMonitor(const SystemMetricsMonitor &) = delete;
    SystemMetricsMonitor &operator=(const SystemMetricsMonitor &) = delete;

    /**
     * @brief 启动后台采样线程。
     * @param config 采样周期和CSV输出路径。
     * @return true表示线程已启动；参数错误、文件创建或线程创建失败返回false。
     */
    bool start(const SystemMetricsConfig &config);

    /** @brief 请求采样线程退出，等待线程结束并关闭CSV文件。 */
    void stop();

    /** @brief 向标准输出打印本轮有效样本的最小、平均和最大值。 */
    void printSummary() const;

private:
    /** 单个可选指标值。 */
    struct OptionalMetric {
        bool valid;
        double value;
    };

    /** 单项指标在完整测试周期内的汇总值。 */
    struct MetricSummary {
        uint64_t count;
        double sum;
        double minimum;
        double maximum;
    };

    /** `/proc/stat`中CPU累计时间。 */
    struct CpuTimes {
        bool valid;
        uint64_t total;
        uint64_t idle;
    };

    /** 一次系统状态采样。 */
    struct Sample {
        uint64_t elapsed_ms;
        OptionalMetric soc_temp_c;
        OptionalMetric gpu_temp_c;
        OptionalMetric cpu_usage_percent;
        OptionalMetric cpu_freq_khz;
        OptionalMetric ddr_load_percent;
        OptionalMetric ddr_freq_hz;
        OptionalMetric rkvenc_enabled;
        OptionalMetric rkvenc_freq_hz;
        OptionalMetric mem_available_kb;
    };

    /** 后台线程入口，立即采样一次，随后按固定周期采样。 */
    void workerMain();

    /** 发现thermal、CPU、DMC和RKVENC节点并打印可用性。 */
    void discoverMetricSources();

    /** 读取当前所有系统指标并更新CPU累计时间基线。 */
    Sample collectSample(uint64_t elapsed_ms);

    /** 将一次采样写入CSV并立即刷新。 */
    void writeSample(const Sample &sample);

    /** 将一次采样合并进测试周期汇总。 */
    void updateSummary(const Sample &sample);

    /** 更新一个有效指标的统计值。 */
    static void updateMetricSummary(MetricSummary *summary,
                                    const OptionalMetric &metric);

    /** 打印一个指标的有效样本数及最小、平均和最大值。 */
    static void printMetricSummary(const char *name,
                                   const char *unit,
                                   const MetricSummary &summary);

    /** 将仅包含整数的sysfs文本转换为可选指标。 */
    static OptionalMetric readIntegerMetric(const std::string &path,
                                            double divisor);

    /** CSV输出可选指标，不可用时写入N/A。 */
    static void writeOptionalMetric(std::ofstream *output,
                                    const OptionalMetric &metric,
                                    int precision);

    /* ---------- 生命周期及线程同步 ---------- */
    bool running_;                  /* 监控线程是否已经成功启动。 */
    bool stop_requested_;           /* 主线程是否请求结束采样。 */
    std::thread worker_;            /* 周期采样后台线程。 */
    mutable std::mutex mutex_;      /* 保护运行状态和停止条件。 */
    std::condition_variable condition_; /* 支持按周期等待并及时响应停止。 */

    /* ---------- 配置及输出 ---------- */
    SystemMetricsConfig config_;    /* 当前采样周期和CSV路径。 */
    std::ofstream csv_file_;        /* 实时刷新的CSV输出流。 */

    /* ---------- 自动发现的系统节点 ---------- */
    std::string soc_temp_path_;     /* type=soc-thermal对应的temp节点。 */
    std::string gpu_temp_path_;     /* type=gpu-thermal对应的temp节点。 */
    std::string cpu_freq_path_;     /* policy0当前CPU频率节点。 */
    std::string ddr_load_path_;     /* DMC负载及当前频率节点。 */
    std::string ddr_freq_path_;     /* DMC独立当前频率节点。 */
    std::string clk_summary_path_;  /* debugfs时钟汇总节点。 */

    /* ---------- 相邻样本状态 ---------- */
    CpuTimes previous_cpu_times_;   /* 上一次/proc/stat累计值。 */

    /* ---------- 测试周期统计 ---------- */
    uint64_t sample_count_;         /* 已成功写出的总采样次数。 */
    MetricSummary soc_temp_summary_;
    MetricSummary gpu_temp_summary_;
    MetricSummary cpu_usage_summary_;
    MetricSummary cpu_freq_summary_;
    MetricSummary ddr_load_summary_;
    MetricSummary ddr_freq_summary_;
    MetricSummary rkvenc_enabled_summary_;
    MetricSummary rkvenc_freq_summary_;
    MetricSummary mem_available_summary_;
};

#endif
