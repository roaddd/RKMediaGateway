/**
 * @file systemMetricsMonitor.cpp
 * @brief RK3568系统状态采样模块实现。
 */

#include "systemMetricsMonitor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <glob.h>
#include <iomanip>
#include <sstream>

namespace {

const char *kProcStatPath = "/proc/stat";
const char *kProcMeminfoPath = "/proc/meminfo";
const char *kCpuFrequencyPath =
    "/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq";
const char *kDdrLoadPath = "/sys/class/devfreq/dmc/load";
const char *kDdrFrequencyPath = "/sys/class/devfreq/dmc/cur_freq";
const char *kClockSummaryPath = "/sys/kernel/debug/clk/clk_summary";
const char *kRkvencClockName = "clk_rkvenc_core";

/**
 * @brief 读取文本文件的第一行，并去除行尾空白。
 */
bool readFirstLine(const std::string &path, std::string *line) {
    std::ifstream input;
    size_t last_non_space = 0;

    if (path.empty() || line == NULL) {
        return false;
    }
    input.open(path.c_str());
    if (!input.is_open() || !std::getline(input, *line)) {
        return false;
    }
    last_non_space = line->find_last_not_of(" \t\r\n");
    if (last_non_space == std::string::npos) {
        line->clear();
    } else {
        line->erase(last_non_space + 1U);
    }
    return true;
}

/**
 * @brief 判断指定路径当前是否可以读取。
 */
bool isReadable(const std::string &path) {
    std::ifstream input;

    if (path.empty()) {
        return false;
    }
    input.open(path.c_str());
    return input.is_open();
}

} // namespace

SystemMetricsMonitor::SystemMetricsMonitor()
    : running_(false),
      stop_requested_(false),
      previous_cpu_times_(),
      sample_count_(0),
      soc_temp_summary_(),
      gpu_temp_summary_(),
      cpu_usage_summary_(),
      cpu_freq_summary_(),
      ddr_load_summary_(),
      ddr_freq_summary_(),
      rkvenc_enabled_summary_(),
      rkvenc_freq_summary_(),
      mem_available_summary_() {
}

SystemMetricsMonitor::~SystemMetricsMonitor() {
    stop();
}

SystemMetricsMonitor::OptionalMetric SystemMetricsMonitor::readIntegerMetric(
    const std::string &path,
    double divisor) {
    OptionalMetric result = {false, 0.0};
    std::string line;
    std::istringstream parser;
    uint64_t value = 0;

    if (divisor <= 0.0 || !readFirstLine(path, &line)) {
        return result;
    }
    parser.str(line);
    if (!(parser >> value)) {
        return result;
    }
    result.valid = true;
    result.value = static_cast<double>(value) / divisor;
    return result;
}

void SystemMetricsMonitor::writeOptionalMetric(std::ofstream *output,
                                                const OptionalMetric &metric,
                                                int precision) {
    if (output == NULL) {
        return;
    }
    if (!metric.valid) {
        *output << "N/A";
        return;
    }
    *output << std::fixed << std::setprecision(precision) << metric.value;
}

bool SystemMetricsMonitor::start(const SystemMetricsConfig &config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) {
        std::fprintf(stderr, "[ERROR] system metrics monitor start failed: already running\n");
        return false;
    }
    if (config.sample_interval_ms <= 0 || config.csv_path.empty()) {
        std::fprintf(stderr,
                     "[ERROR] system metrics monitor start failed: interval=%d csv=%s\n",
                     config.sample_interval_ms,
                     config.csv_path.c_str());
        return false;
    }

    config_ = config;
    stop_requested_ = false;
    previous_cpu_times_ = CpuTimes();
    sample_count_ = 0;
    soc_temp_summary_ = MetricSummary();
    gpu_temp_summary_ = MetricSummary();
    cpu_usage_summary_ = MetricSummary();
    cpu_freq_summary_ = MetricSummary();
    ddr_load_summary_ = MetricSummary();
    ddr_freq_summary_ = MetricSummary();
    rkvenc_enabled_summary_ = MetricSummary();
    rkvenc_freq_summary_ = MetricSummary();
    mem_available_summary_ = MetricSummary();

    discoverMetricSources();
    csv_file_.open(config_.csv_path.c_str(), std::ios::out | std::ios::trunc);
    if (!csv_file_.is_open()) {
        std::fprintf(stderr,
                     "[ERROR] system metrics monitor start failed: open CSV %s\n",
                     config_.csv_path.c_str());
        return false;
    }
    csv_file_ << "elapsed_ms,soc_temp_c,gpu_temp_c,cpu_usage_percent,cpu_freq_khz,"
                 "ddr_load_percent,ddr_freq_hz,rkvenc_enabled,rkvenc_freq_hz,"
                 "mem_available_kb\n";
    csv_file_.flush();

    try {
        worker_ = std::thread(&SystemMetricsMonitor::workerMain, this);
    } catch (...) {
        std::fprintf(stderr, "[ERROR] system metrics monitor start failed: create thread\n");
        csv_file_.close();
        return false;
    }
    running_ = true;
    std::printf("[INFO] system metrics monitor started: interval=%dms csv=%s\n",
                config_.sample_interval_ms,
                config_.csv_path.c_str());
    return true;
}

void SystemMetricsMonitor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stop_requested_ = true;
        condition_.notify_all();
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (csv_file_.is_open()) {
            csv_file_.flush();
            csv_file_.close();
        }
        running_ = false;
    }
    std::printf("[INFO] system metrics monitor stopped: samples=%llu csv=%s\n",
                static_cast<unsigned long long>(sample_count_),
                config_.csv_path.c_str());
}

void SystemMetricsMonitor::discoverMetricSources() {
    glob_t paths = {};
    size_t index = 0;
    std::string type;
    std::string type_path;
    std::string zone_path;
    int glob_ret = 0;

    soc_temp_path_.clear();
    gpu_temp_path_.clear();
    cpu_freq_path_.clear();
    ddr_load_path_.clear();
    ddr_freq_path_.clear();
    clk_summary_path_.clear();

    /* 根据thermal type发现温度节点，避免依赖thermal_zone编号。 */
    glob_ret = glob("/sys/class/thermal/thermal_zone*/type", 0, NULL, &paths);
    if (glob_ret == 0) {
        for (index = 0; index < paths.gl_pathc; ++index) {
            type_path = paths.gl_pathv[index];
            if (!readFirstLine(type_path, &type)) {
                continue;
            }
            zone_path = type_path.substr(0, type_path.size() - 4U);
            if (type == "soc-thermal") {
                soc_temp_path_ = zone_path + "temp";
            } else if (type == "gpu-thermal") {
                gpu_temp_path_ = zone_path + "temp";
            }
        }
    }
    globfree(&paths);

    if (isReadable(kCpuFrequencyPath)) {
        cpu_freq_path_ = kCpuFrequencyPath;
    }
    if (isReadable(kDdrLoadPath)) {
        ddr_load_path_ = kDdrLoadPath;
    }
    if (isReadable(kDdrFrequencyPath)) {
        ddr_freq_path_ = kDdrFrequencyPath;
    }
    if (isReadable(kClockSummaryPath)) {
        clk_summary_path_ = kClockSummaryPath;
    }

    std::printf("[INFO] metrics source: soc_temp=%s gpu_temp=%s cpu_freq=%s\n",
                soc_temp_path_.empty() ? "N/A" : soc_temp_path_.c_str(),
                gpu_temp_path_.empty() ? "N/A" : gpu_temp_path_.c_str(),
                cpu_freq_path_.empty() ? "N/A" : cpu_freq_path_.c_str());
    std::printf("[INFO] metrics source: ddr_load=%s ddr_freq=%s rkvenc_clock=%s\n",
                ddr_load_path_.empty() ? "N/A" : ddr_load_path_.c_str(),
                ddr_freq_path_.empty() ? "N/A" : ddr_freq_path_.c_str(),
                clk_summary_path_.empty() ? "N/A" : clk_summary_path_.c_str());
    if (soc_temp_path_.empty()) {
        std::fprintf(stderr, "[WARN] system metrics: soc-thermal node is unavailable\n");
    }
    if (ddr_load_path_.empty()) {
        std::fprintf(stderr, "[WARN] system metrics: DMC load node is unavailable\n");
    }
    if (clk_summary_path_.empty()) {
        std::fprintf(stderr,
                     "[WARN] system metrics: clk_summary unavailable; mount debugfs to record RKVENC\n");
    }
}

void SystemMetricsMonitor::workerMain() {
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point now;
    std::chrono::milliseconds interval(0);
    uint64_t elapsed_ms = 0;
    Sample sample = {};
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

    start_time = std::chrono::steady_clock::now();
    interval = std::chrono::milliseconds(config_.sample_interval_ms);

    while (true) {
        now = std::chrono::steady_clock::now();
        elapsed_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count());
        sample = collectSample(elapsed_ms);
        writeSample(sample);
        updateSummary(sample);

        lock.lock();
        if (condition_.wait_for(lock, interval, [this]() { return stop_requested_; })) {
            lock.unlock();
            break;
        }
        lock.unlock();
    }
}

SystemMetricsMonitor::Sample SystemMetricsMonitor::collectSample(uint64_t elapsed_ms) {
    Sample sample = {};
    CpuTimes current_cpu = {};
    std::ifstream stat_file;
    std::ifstream meminfo_file;
    std::ifstream clock_file;
    std::string label;
    std::string line;
    std::istringstream parser;
    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;
    uint64_t total_delta = 0;
    uint64_t idle_delta = 0;
    uint64_t ddr_load = 0;
    uint64_t ddr_frequency = 0;
    uint64_t rkvenc_enable = 0;
    uint64_t rkvenc_prepare = 0;
    uint64_t rkvenc_protect = 0;
    uint64_t rkvenc_frequency = 0;
    uint64_t mem_available = 0;
    char separator = '\0';
    std::string suffix;

    sample.elapsed_ms = elapsed_ms;
    sample.soc_temp_c = readIntegerMetric(soc_temp_path_, 1000.0);
    sample.gpu_temp_c = readIntegerMetric(gpu_temp_path_, 1000.0);
    sample.cpu_freq_khz = readIntegerMetric(cpu_freq_path_, 1.0);

    /* 通过相邻两次CPU累计时间差计算整个系统的平均CPU使用率。 */
    stat_file.open(kProcStatPath);
    if (stat_file.is_open() &&
        (stat_file >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) &&
        label == "cpu") {
        current_cpu.valid = true;
        current_cpu.idle = idle + iowait;
        current_cpu.total = user + nice + system + idle + iowait + irq + softirq + steal;
        if (previous_cpu_times_.valid && current_cpu.total > previous_cpu_times_.total) {
            total_delta = current_cpu.total - previous_cpu_times_.total;
            idle_delta = current_cpu.idle >= previous_cpu_times_.idle
                ? current_cpu.idle - previous_cpu_times_.idle : 0;
            sample.cpu_usage_percent.valid = true;
            sample.cpu_usage_percent.value =
                100.0 * static_cast<double>(total_delta - std::min(idle_delta, total_delta)) /
                static_cast<double>(total_delta);
        }
        previous_cpu_times_ = current_cpu;
    }

    /* RK3568 DMC节点格式为“负载百分比@当前频率Hz”。 */
    if (readFirstLine(ddr_load_path_, &line)) {
        parser.clear();
        parser.str(line);
        if ((parser >> ddr_load >> separator >> ddr_frequency >> suffix) &&
            separator == '@') {
            sample.ddr_load_percent.valid = true;
            sample.ddr_load_percent.value = static_cast<double>(ddr_load);
            sample.ddr_freq_hz.valid = true;
            sample.ddr_freq_hz.value = static_cast<double>(ddr_frequency);
        }
    }
    if (!sample.ddr_freq_hz.valid) {
        sample.ddr_freq_hz = readIntegerMetric(ddr_freq_path_, 1.0);
    }

    /* clk_summary每行前五列依次为名称、enable、prepare、protect和rate。 */
    clock_file.open(clk_summary_path_.c_str());
    while (clock_file.is_open() && std::getline(clock_file, line)) {
        parser.clear();
        parser.str(line);
        if (!(parser >> label) || label != kRkvencClockName) {
            continue;
        }
        if (parser >> rkvenc_enable >> rkvenc_prepare >> rkvenc_protect >> rkvenc_frequency) {
            sample.rkvenc_enabled.valid = true;
            sample.rkvenc_enabled.value = rkvenc_enable > 0 ? 1.0 : 0.0;
            sample.rkvenc_freq_hz.valid = true;
            sample.rkvenc_freq_hz.value = static_cast<double>(rkvenc_frequency);
        }
        break;
    }

    meminfo_file.open(kProcMeminfoPath);
    while (meminfo_file.is_open() && std::getline(meminfo_file, line)) {
        parser.clear();
        parser.str(line);
        if ((parser >> label >> mem_available >> suffix) && label == "MemAvailable:") {
            sample.mem_available_kb.valid = true;
            sample.mem_available_kb.value = static_cast<double>(mem_available);
            break;
        }
    }
    return sample;
}

void SystemMetricsMonitor::writeSample(const Sample &sample) {
    if (!csv_file_.is_open()) {
        return;
    }

    csv_file_ << sample.elapsed_ms << ',';
    writeOptionalMetric(&csv_file_, sample.soc_temp_c, 2);
    csv_file_ << ',';
    writeOptionalMetric(&csv_file_, sample.gpu_temp_c, 2);
    csv_file_ << ',';
    writeOptionalMetric(&csv_file_, sample.cpu_usage_percent, 2);
    csv_file_ << ',';
    writeOptionalMetric(&csv_file_, sample.cpu_freq_khz, 0);
    csv_file_ << ',';
    writeOptionalMetric(&csv_file_, sample.ddr_load_percent, 0);
    csv_file_ << ',';
    writeOptionalMetric(&csv_file_, sample.ddr_freq_hz, 0);
    csv_file_ << ',';
    writeOptionalMetric(&csv_file_, sample.rkvenc_enabled, 0);
    csv_file_ << ',';
    writeOptionalMetric(&csv_file_, sample.rkvenc_freq_hz, 0);
    csv_file_ << ',';
    writeOptionalMetric(&csv_file_, sample.mem_available_kb, 0);
    csv_file_ << '\n';
    csv_file_.flush();
    ++sample_count_;
}

void SystemMetricsMonitor::updateSummary(const Sample &sample) {
    updateMetricSummary(&soc_temp_summary_, sample.soc_temp_c);
    updateMetricSummary(&gpu_temp_summary_, sample.gpu_temp_c);
    updateMetricSummary(&cpu_usage_summary_, sample.cpu_usage_percent);
    updateMetricSummary(&cpu_freq_summary_, sample.cpu_freq_khz);
    updateMetricSummary(&ddr_load_summary_, sample.ddr_load_percent);
    updateMetricSummary(&ddr_freq_summary_, sample.ddr_freq_hz);
    updateMetricSummary(&rkvenc_enabled_summary_, sample.rkvenc_enabled);
    updateMetricSummary(&rkvenc_freq_summary_, sample.rkvenc_freq_hz);
    updateMetricSummary(&mem_available_summary_, sample.mem_available_kb);
}

void SystemMetricsMonitor::updateMetricSummary(MetricSummary *summary,
                                                const OptionalMetric &metric) {
    if (summary == NULL || !metric.valid) {
        return;
    }
    if (summary->count == 0) {
        summary->minimum = metric.value;
        summary->maximum = metric.value;
    } else {
        summary->minimum = std::min(summary->minimum, metric.value);
        summary->maximum = std::max(summary->maximum, metric.value);
    }
    summary->sum += metric.value;
    ++summary->count;
}

void SystemMetricsMonitor::printMetricSummary(const char *name,
                                               const char *unit,
                                               const MetricSummary &summary) {
    double average = 0.0;

    if (name == NULL || unit == NULL) {
        std::fprintf(stderr, "[ERROR] print metric summary failed: null argument\n");
        return;
    }
    if (summary.count == 0) {
        std::printf("  %-22s N/A\n", name);
        return;
    }
    average = summary.sum / static_cast<double>(summary.count);
    std::printf("  %-22s min=%.2f avg=%.2f max=%.2f %s samples=%llu\n",
                name,
                summary.minimum,
                average,
                summary.maximum,
                unit,
                static_cast<unsigned long long>(summary.count));
}

void SystemMetricsMonitor::printSummary() const {
    std::printf("\n========== SYSTEM METRICS ==========\n");
    std::printf("  samples                %llu\n",
                static_cast<unsigned long long>(sample_count_));
    printMetricSummary("soc_temperature", "C", soc_temp_summary_);
    printMetricSummary("gpu_temperature", "C", gpu_temp_summary_);
    printMetricSummary("cpu_usage", "%", cpu_usage_summary_);
    printMetricSummary("cpu_frequency", "kHz", cpu_freq_summary_);
    printMetricSummary("ddr_load", "%", ddr_load_summary_);
    printMetricSummary("ddr_frequency", "Hz", ddr_freq_summary_);
    printMetricSummary("rkvenc_enabled", "ratio", rkvenc_enabled_summary_);
    printMetricSummary("rkvenc_frequency", "Hz", rkvenc_freq_summary_);
    printMetricSummary("mem_available", "kB", mem_available_summary_);
}
