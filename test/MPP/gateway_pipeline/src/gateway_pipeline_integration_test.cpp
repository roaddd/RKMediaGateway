/**
 * @file gateway_pipeline_integration_test.cpp
 * @brief 在RK3568上运行正式网关视频链路并校验关键累计统计。
 *
 * 本程序不复制V4L2或MPP实现，而是直接启动MediaGateway生产链路。测试结束后通过
 * 对外快照接口验证采集、latest-frame发布和MPP编码确实发生，并打印后续问题分析
 * 所需的sequence gap、覆盖、deadline miss及MPP分阶段耗时。
 */

#include "defValue.h"
#include "logger.h"

extern "C"
{
#include "mediaGateway.h"
}

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/** 正式网关运行线程的输入与返回结果。 */
typedef struct
{
    MediaGatewayCtx *gateway; /* 被测生产网关实例。 */
    int run_result;           /* media_gateway_run返回值。 */
} GatewayPipelineTestRuntime;

/**
 * @brief 在线程中运行正式MediaGateway主循环。
 * @param arg GatewayPipelineTestRuntime指针。
 * @return 始终返回NULL，运行结果写入runtime。
 */
static void *gateway_pipeline_run_thread(void *arg)
{
    GatewayPipelineTestRuntime *runtime = NULL;

    if (!arg)
    {
        LOG_ERROR("gateway_pipeline_run_thread failed: arg is NULL");
        return NULL;
    }
    runtime = static_cast<GatewayPipelineTestRuntime *>(arg);
    if (!runtime->gateway)
    {
        LOG_ERROR("gateway_pipeline_run_thread failed: gateway is NULL");
        runtime->run_result = MEDIA_ERR_INVALID_PARAM;
        return NULL;
    }

    runtime->run_result = media_gateway_run(runtime->gateway);
    return NULL;
}

/**
 * @brief 打印并校验一个采集源的正式链路统计。
 * @return 统计有效返回MEDIA_OK，否则返回对应错误码。
 */
static MediaResult verify_capture_source(MediaGatewayCtx *gateway, int source_idx)
{
    MediaFrameSourceStats stats = {0};
    MediaResult result = MEDIA_OK;

    result = media_gateway_get_capture_source_stats(gateway, source_idx, &stats);
    if (result != MEDIA_OK)
    {
        LOG_ERROR("verify_capture_source failed: get stats source=%d ret=%d",
                  source_idx,
                  result);
        return result;
    }

    printf("capture[%d] captured=%" PRIu64 " gap_events=%" PRIu64
           " gap_frames=%" PRIu64 " published=%" PRIu64
           " overwritten=%" PRIu64 " no_slot=%" PRIu64
           " acquired=%" PRIu64 "\n",
           source_idx,
           stats.captured_frames,
           stats.sequence_gap_events,
           stats.sequence_gap_frames,
           stats.published_frames,
           stats.overwritten_frames,
           stats.no_writable_slot_drops,
           stats.acquired_frames);
    if (stats.captured_frames == 0U || stats.published_frames == 0U ||
        stats.acquired_frames == 0U)
    {
        LOG_ERROR("verify_capture_source failed: inactive production path source=%d captured=%" PRIu64
                  " published=%" PRIu64 " acquired=%" PRIu64,
                  source_idx,
                  stats.captured_frames,
                  stats.published_frames,
                  stats.acquired_frames);
        return MEDIA_ERR;
    }
    return MEDIA_OK;
}

/**
 * @brief 打印并校验一个视频流的正式编码统计。
 * @return 统计有效返回MEDIA_OK，否则返回对应错误码。
 */
static MediaResult verify_video_stream(MediaGatewayCtx *gateway, int stream_idx)
{
    MediaGatewayVideoPipelineStats stats = {0};
    MediaResult result = MEDIA_OK;
    double average_encode_us = 0.0;
    double average_packet_wait_us = 0.0;

    result = media_gateway_get_video_pipeline_stats(gateway, stream_idx, &stats);
    if (result != MEDIA_OK)
    {
        LOG_ERROR("verify_video_stream failed: get stats stream=%d ret=%d",
                  stream_idx,
                  result);
        return result;
    }
    if (stats.encoded_frames > 0U)
    {
        average_encode_us = static_cast<double>(stats.encode_total_us) /
                            static_cast<double>(stats.encoded_frames);
        average_packet_wait_us = static_cast<double>(stats.packet_wait_total_us) /
                                 static_cast<double>(stats.encoded_frames);
    }

    printf("stream[%d] acquired=%" PRIu64 " encoded=%" PRIu64
           " failures=%" PRIu64 " busy_overwrite=%" PRIu64
           " deadline_miss=%" PRIu64 " max_streak=%" PRIu64
           " avg_us=%.1f max_us=%" PRIu64 " packet_wait_avg_us=%.1f\n",
           stream_idx,
           stats.acquired_frames,
           stats.encoded_frames,
           stats.encode_failures,
           stats.overwrites_while_encoder_busy,
           stats.deadline_miss_frames,
           stats.max_consecutive_deadline_misses,
           average_encode_us,
           stats.encode_max_us,
           average_packet_wait_us);
    if (stats.acquired_frames == 0U || stats.encoded_frames == 0U)
    {
        LOG_ERROR("verify_video_stream failed: inactive production path stream=%d acquired=%" PRIu64
                  " encoded=%" PRIu64,
                  stream_idx,
                  stats.acquired_frames,
                  stats.encoded_frames);
        return MEDIA_ERR;
    }
    return MEDIA_OK;
}

/**
 * @brief 启动正式视频链路、持续指定秒数并校验运行统计。
 *
 * 用法：gateway_pipeline_integration_test [配置文件] [持续秒数]
 */
int main(int argc, char **argv)
{
    MediaGatewayCtx gateway = {0};
    MediaGatewayConfig config = {0};
    GatewayPipelineTestRuntime runtime = {0};
    pthread_t run_thread = {0};
    const char *config_path = "media_gateway.toml";
    int duration_sec = 30;
    int thread_ret = 0;
    int source_idx = 0;
    int stream_idx = 0;
    int rtsp_output_selected = 0;
    int test_failed = 0;

    if (argc > 1 && argv[1] && argv[1][0] != '\0')
        config_path = argv[1];
    if (argc > 2)
        duration_sec = atoi(argv[2]);
    if (duration_sec <= 0)
    {
        fprintf(stderr, "invalid duration: %d\n", duration_sec);
        return EXIT_FAILURE;
    }

    if (def_value_init(config_path) != MEDIA_OK)
    {
        fprintf(stderr, "failed to load TOML config: %s\n", config_path);
        return EXIT_FAILURE;
    }
    def_value_get_media_gateway_config(&config);
    /*
     * 本测试隔离验证正式视频采集、编码和分发链路。关闭音频及外部推送协议，只为
     * 第一条启用的视频流保留本地RTSP监听；无需连接客户端，但满足生产网关至少存在
     * 一个输出通道的运行约束。
     */
    config.audio.source.enabled = 0;
    config.audio.talkback.enabled = 0;
    config.output.switches.enable_rtsp = 1;
    config.output.switches.enable_rtmp = 0;
    config.output.switches.enable_gb28181 = 0;
    config.output.switches.enable_webrtc = 0;
    for (stream_idx = 0; stream_idx < config.video.stream_count; ++stream_idx)
    {
        config.video.streams[stream_idx].enable_rtsp = 0;
        config.video.streams[stream_idx].enable_rtmp = 0;
        config.video.streams[stream_idx].enable_gb28181 = 0;
        config.video.streams[stream_idx].enable_webrtc = 0;
        /* 音频已关闭，所有协议的音频编码组绑定也必须显式清空。 */
        config.video.streams[stream_idx].rtsp_audio.encoder_group = "";
        config.video.streams[stream_idx].rtmp_audio.encoder_group = "";
        config.video.streams[stream_idx].gb28181_audio.encoder_group = "";
        config.video.streams[stream_idx].webrtc_audio.encoder_group = "";
        if (!rtsp_output_selected && config.video.streams[stream_idx].enabled)
        {
            config.video.streams[stream_idx].enable_rtsp = 1;
            rtsp_output_selected = 1;
        }
    }
    if (!rtsp_output_selected)
    {
        fprintf(stderr, "no enabled video stream in config: %s\n", config_path);
        return EXIT_FAILURE;
    }
    log_set_level(static_cast<LogLevel>(config.system.log.level));
    log_set_immediate_flush(1);

    printf("gateway pipeline integration test: duration=%ds capture_sources=%d streams=%d\n",
           duration_sec,
           config.input.capture_source_count,
           config.video.stream_count);
    for (stream_idx = 0; stream_idx < config.video.stream_count; ++stream_idx)
    {
        if (!config.video.streams[stream_idx].enabled)
            continue;
        printf("  stream[%d] source=%d resolution=%dx%d fps=%d bitrate=%d rtsp=%s\n",
               stream_idx,
               config.video.streams[stream_idx].source_index,
               config.video.streams[stream_idx].width,
               config.video.streams[stream_idx].height,
               config.video.streams[stream_idx].fps,
               config.video.streams[stream_idx].bitrate,
               config.video.streams[stream_idx].enable_rtsp ? "on" : "off");
    }

    if (media_gateway_init(&gateway, &config) != MEDIA_OK)
    {
        LOG_ERROR("gateway pipeline integration test failed: init config=%s",
                  config_path);
        return EXIT_FAILURE;
    }

    runtime.gateway = &gateway;
    runtime.run_result = MEDIA_ERR;
    thread_ret = pthread_create(&run_thread,
                                NULL,
                                gateway_pipeline_run_thread,
                                &runtime);
    if (thread_ret != 0)
    {
        LOG_ERROR("gateway pipeline integration test failed: pthread_create ret=%d",
                  thread_ret);
        media_gateway_deinit(&gateway);
        return EXIT_FAILURE;
    }

    sleep(static_cast<unsigned int>(duration_sec));
    media_gateway_stop(&gateway);
    thread_ret = pthread_join(run_thread, NULL);
    if (thread_ret != 0)
    {
        LOG_ERROR("gateway pipeline integration test failed: pthread_join ret=%d",
                  thread_ret);
        media_gateway_deinit(&gateway);
        return EXIT_FAILURE;
    }
    if (runtime.run_result != MEDIA_OK)
    {
        LOG_ERROR("gateway pipeline integration test failed: gateway run ret=%d",
                  runtime.run_result);
        test_failed = 1;
    }

    /* 只校验配置中启用的采集源和视频流，避免把未启用槽位误判为故障。 */
    for (source_idx = 0;
         source_idx < config.input.capture_source_count;
         ++source_idx)
    {
        if (verify_capture_source(&gateway, source_idx) != MEDIA_OK)
            test_failed = 1;
    }
    for (stream_idx = 0; stream_idx < config.video.stream_count; ++stream_idx)
    {
        if (!config.video.streams[stream_idx].enabled)
            continue;
        if (verify_video_stream(&gateway, stream_idx) != MEDIA_OK)
            test_failed = 1;
    }

    media_gateway_deinit(&gateway);
    if (test_failed)
        return EXIT_FAILURE;

    printf("gateway pipeline integration test passed\n");
    return EXIT_SUCCESS;
}
