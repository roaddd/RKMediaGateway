/*
 * @Author: huangkelong
 * @Date: 2026-05-17 23:45:25
 * @LastEditTime: 2026-05-19 23:19:45
 * @LastEditors: huangkelong
 * @Description: 媒体输出路径指标相关函数实现
 * @FilePath: \Fork\RKMediaGateway\bussiness\mediaOutput\src\mediaOutputPathMetrics.c
 * 可以输入预定的版权声明、个性签名、空行等
 */
#include "mediaOutputPathMetrics.h"

#include "logger.h"

#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define PATH_LATENCY_AVG_SAMPLE_COUNT 30
#define PATH_LATENCY_OUTPUT_NAME_LEN 32
#define PATH_LATENCY_STREAM_NAME_LEN 32
#define PATH_LATENCY_SLOT_COUNT 32

typedef struct {
    int used;
    MediaOutputType output_type;
    char output_name[PATH_LATENCY_OUTPUT_NAME_LEN];
    MediaFrameType frame_type;
    char stream_name[PATH_LATENCY_STREAM_NAME_LEN];
    /*
     * 按 output + stream + media 类型维护一个小窗口。
     * 单帧 PATH_LATENCY 日志在发送完成时立即打印；这里额外累计最近
     * PATH_LATENCY_AVG_SAMPLE_COUNT 个采样帧，用来打印平均路径耗时，
     * 避免只看单帧日志时被偶发抖动误导。
     */
    uint64_t samples;
    uint64_t dqbuf_to_send_sum_us;
    uint64_t encode_sum_us;
    uint64_t queue_sum_us;
    uint64_t output_sum_us;
} PathLatencyWindow;

static pthread_mutex_t g_path_latency_lock = PTHREAD_MUTEX_INITIALIZER;
static PathLatencyWindow g_path_latency_windows[PATH_LATENCY_SLOT_COUNT];

static const char *media_output_metric_output_type_name(MediaOutputType output_type)
{
    switch (output_type)
    {
    case MEDIA_OUTPUT_TYPE_RTSP:
        return "rtsp";
    case MEDIA_OUTPUT_TYPE_RTMP:
        return "rtmp";
    case MEDIA_OUTPUT_TYPE_GB28181:
        return "gb28181";
    default:
        return "unknown";
    }
}

static const char *media_output_metric_media_name(MediaFrameType frame_type)
{
    switch (frame_type)
    {
    case MEDIA_FRAME_TYPE_VIDEO:
        return "video";
    case MEDIA_FRAME_TYPE_AUDIO:
        return "audio";
    default:
        return "unknown";
    }
}

static PathLatencyWindow *media_output_get_path_latency_window(const char *output_name,
                                                               MediaOutputType output_type,
                                                               const char *stream_name,
                                                               MediaFrameType frame_type)
{
    int i = 0;
    PathLatencyWindow *empty_slot = NULL;

    /*
     * 窗口按 output_name + output_type + stream_name + frame_type 区分。
     * 同一 stream 同时推 RTSP/RTMP/GB28181 时，每个输出通道单独累计，
     * 避免 queue_us/output_us 被不同协议通道混在一起。
     */
    for (i = 0; i < PATH_LATENCY_SLOT_COUNT; ++i)
    {
        PathLatencyWindow *slot = &g_path_latency_windows[i];
        if (!slot->used)
        {
            if (!empty_slot)
                empty_slot = slot;
            continue;
        }
        if (slot->output_type == output_type &&
            slot->frame_type == frame_type &&
            strncmp(slot->output_name, output_name, PATH_LATENCY_OUTPUT_NAME_LEN) == 0 &&
            strncmp(slot->stream_name, stream_name, PATH_LATENCY_STREAM_NAME_LEN) == 0)
        {
            return slot;
        }
    }

    if (!empty_slot)
        return NULL;

    empty_slot->used = 1;
    empty_slot->output_type = output_type;
    empty_slot->frame_type = frame_type;
    strncpy(empty_slot->output_name, output_name, PATH_LATENCY_OUTPUT_NAME_LEN - 1);
    empty_slot->output_name[PATH_LATENCY_OUTPUT_NAME_LEN - 1] = '\0';
    strncpy(empty_slot->stream_name, stream_name, PATH_LATENCY_STREAM_NAME_LEN - 1);
    empty_slot->stream_name[PATH_LATENCY_STREAM_NAME_LEN - 1] = '\0';
    return empty_slot;
}

static void media_output_update_path_latency_window(const char *output_name,
                                                    MediaOutputType output_type,
                                                    const char *stream_name,
                                                    MediaFrameType frame_type,
                                                    uint64_t dqbuf_to_send_us,
                                                    uint64_t encode_us,
                                                    uint64_t queue_us,
                                                    uint64_t output_us)
{
    PathLatencyWindow *window = NULL;
    uint64_t samples = 0;
    uint64_t dqbuf_to_send_sum_us = 0;
    uint64_t encode_sum_us = 0;
    uint64_t queue_sum_us = 0;
    uint64_t output_sum_us = 0;
    int should_log = 0;

    /*
     * 多个输出线程可能同时发送不同流的包，因此窗口更新需要加锁。
     * 日志内容先在锁内快照出来，真正 LOG_WARN 放到锁外执行，减少锁占用。
     */
    pthread_mutex_lock(&g_path_latency_lock);
    window = media_output_get_path_latency_window(output_name, output_type, stream_name, frame_type);
    if (window)
    {
        window->samples++;
        window->dqbuf_to_send_sum_us += dqbuf_to_send_us;
        window->encode_sum_us += encode_us;
        window->queue_sum_us += queue_us;
        window->output_sum_us += output_us;

        /*
         * 满一个统计窗口后打印一次平均值，并清零重新开始下一个窗口。
         * 这里统计的是“被采样”的包，不是每一帧；采样开关和采样间隔由
         * packet->path_metrics.sample 的生产方决定。
         */
        if (window->samples >= PATH_LATENCY_AVG_SAMPLE_COUNT)
        {
            samples = window->samples;
            dqbuf_to_send_sum_us = window->dqbuf_to_send_sum_us;
            encode_sum_us = window->encode_sum_us;
            queue_sum_us = window->queue_sum_us;
            output_sum_us = window->output_sum_us;
            window->samples = 0;
            window->dqbuf_to_send_sum_us = 0;
            window->encode_sum_us = 0;
            window->queue_sum_us = 0;
            window->output_sum_us = 0;
            should_log = 1;
        }
    }
    pthread_mutex_unlock(&g_path_latency_lock);

    if (!should_log || samples == 0)
        return;

    LOG_WARN("[PATH_LATENCY_AVG] output=%s output_type=%s stream=%s media=%s samples=%" PRIu64
             " avg_dqbuf_to_send_us=%.2f"
             " avg_encode_us=%.2f"
             " avg_queue_us=%.2f"
             " avg_output_us=%.2f",
             output_name,
             media_output_metric_output_type_name(output_type),
             stream_name,
             media_output_metric_media_name(frame_type),
             samples,
             (double)dqbuf_to_send_sum_us / (double)samples,
             (double)encode_sum_us / (double)samples,
             (double)queue_sum_us / (double)samples,
             (double)output_sum_us / (double)samples);
}

uint64_t media_output_metrics_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void media_output_log_path_latency(const MediaOutputPathLatencySample *sample)
{
    const MediaPacket *packet = NULL;
    const char *output_name = NULL;
    const char *stream_name = NULL;
    uint64_t dqbuf_to_send_us = 0; /* 从v4l2缓冲区取出该帧到发送完成的时间 */
    uint64_t queue_us = 0; /* 该帧从输出队列中取出到发送完成的时间 */
    uint64_t output_us = 0; /* 该帧发送开始到发送完成的时间 */

    /*
     * 调试打印的入口：
     * 1. 只有 packet->path_metrics.sample 为真才打印，普通包直接跳过。
     *    当前 sample 通常由 bench_enable 和 bench_sample_every 控制。
     * 2. 每个采样包先打印一条 PATH_LATENCY 单帧日志。
     * 3. 然后把该样本累加进 output + stream + media 对应的窗口，满 30 个样本
     *    再打印一条 PATH_LATENCY_AVG 平均日志。
     */
    if (!sample || !sample->packet || !sample->packet->path_metrics.sample)
        return;
    packet = sample->packet;

    /*
     * 时间口径：
     * - dqbuf_to_send_us: 采集帧时间戳到输出发送完成，近似端到端路径耗时。
     * - encode_us: 编码阶段耗时，由 gateway 入队前写入。
     * - queue_us: 进入输出队列到真正开始发送的等待时间。
     * - output_us: 输出模块实际发送调用耗时。
     */
    dqbuf_to_send_us = (sample->send_done_us >= packet->pts_us) ? (sample->send_done_us - packet->pts_us) : 0;
    queue_us = (sample->send_start_us >= packet->path_metrics.enqueue_ts_us)
                   ? (sample->send_start_us - packet->path_metrics.enqueue_ts_us)
                   : 0;
    output_us = sample->send_done_us - sample->send_start_us;
    output_name = sample->output_name ? sample->output_name : "unknown";
    stream_name = packet->path_metrics.stream_name ? packet->path_metrics.stream_name : "unknown";

    LOG_INFO("[PATH_LATENCY] output=%s output_type=%s stream=%s media=%s frame_id=%" PRIu64
             " dqbuf_to_send_us=%" PRIu64
             " encode_us=%" PRIu64
             " queue_us=%" PRIu64
             " output_us=%" PRIu64,
             output_name,
             media_output_metric_output_type_name(sample->output_type),
             stream_name,
             media_output_metric_media_name(packet->frame_type),
             packet->frame_id,
             dqbuf_to_send_us,
             packet->path_metrics.encode_us,
             queue_us,
             output_us);

    media_output_update_path_latency_window(output_name,
                                            sample->output_type,
                                            stream_name,
                                            packet->frame_type,
                                            dqbuf_to_send_us,
                                            packet->path_metrics.encode_us,
                                            queue_us,
                                            output_us);
}
