#include <stdio.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <pthread.h>

#include "defValue.h"
#include "logger.h"

extern "C"
{
#include "mediaGateway.h"
#include "debugCommandServer.h"
#include "systemDebug.h"
}

/**
 * @brief 网关进程信号等待线程的共享上下文。
 * @note SIGINT/SIGTERM 由主线程预先屏蔽并交给专用线程同步等待，避免在异步信号
 *       处理函数中执行日志、加锁或网关停止操作。
 */
struct GatewaySignalContext
{
    MediaGatewayCtx *gateway;             /* 收到退出信号后需要停止的网关实例。 */
    std::atomic<bool> shutdown_requested; /* 主流程是否已自行结束，要求信号线程退出。 */

    GatewaySignalContext()
        : gateway(NULL), shutdown_requested(false)
    {
    }
};

/**
 * @brief 同步等待进程退出信号并请求网关优雅停止。
 * @param[in] arg 指向 GatewaySignalContext 的线程参数。
 * @return 始终返回 NULL。
 * @note 本线程运行在普通线程上下文，收到信号后可以安全记录日志并调用
 *       media_gateway_stop()；具体资源仍由网关主流程按依赖顺序释放。
 */
static void *gateway_signal_thread_main(void *arg)
{
    GatewaySignalContext *context = static_cast<GatewaySignalContext *>(arg);
    sigset_t wait_set;
    struct timespec wait_timeout = {0, 200000000};
    int signal_number = 0;

    if (!context || !context->gateway)
    {
        LOG_ERROR("gateway signal thread failed: invalid context");
        return NULL;
    }

    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGINT);
    sigaddset(&wait_set, SIGTERM);

    /*
     * 使用带超时的同步等待：收到 Ctrl-C 时立即停止网关；如果网关因其他错误自行
     * 返回，线程最多等待 200 ms 就能观察退出标志，避免主线程永久阻塞在 join。
     */
    while (!context->shutdown_requested.load())
    {
        signal_number = sigtimedwait(&wait_set, NULL, &wait_timeout);
        if (signal_number == SIGINT || signal_number == SIGTERM)
        {
            LOG_INFO("gateway received signal=%d, requesting graceful shutdown", signal_number);
            media_gateway_stop(context->gateway);
            break;
        }
        if (signal_number < 0 && (errno == EAGAIN || errno == EINTR))
        {
            continue;
        }
        if (signal_number < 0)
        {
            LOG_ERROR("gateway signal wait failed: errno=%d", errno);
            break;
        }
    }
    return NULL;
}

/* 打印配置快照 */
static void log_main_config_snapshot(const MediaGatewayConfig *config)
{
    const MediaGatewayAudioEncoderGroupConfig *audio_group = NULL;
    const MediaGatewayStreamConfig *s = NULL;
    int i = 0;

    if (!config)
    {
        return;
    }

    LOG_INFO("[MAIN_CFG] source=%s loaded=%d",
             def_value_source_path(),
             def_value_loaded());
    LOG_INFO("[MAIN_CFG] parsed stream_count=%d bench(enable=%d sample_every=%d print_interval_sec=%d) log_level=%d",
             config->video.stream_count,
             config->system.bench.enabled,
             config->system.bench.sample_every,
             config->system.bench.print_interval_sec,
             config->system.log.level);
    LOG_INFO("[MAIN_CFG] parsed light_fps enable=%d normal=%d low_light=%d bright=%d min_switch_ms=%d eval_ms=%d",
             config->policy.light_fps.enabled,
             config->policy.light_fps.targets.normal_fps,
             config->policy.light_fps.targets.low_light_fps,
             config->policy.light_fps.targets.bright_fps,
             config->policy.light_fps.timing.min_switch_interval_ms,
             config->policy.light_fps.timing.evaluate_interval_ms);
    LOG_INFO("[MAIN_CFG] parsed audio enabled=%d device=%s rate=%d capture_channels=%d period_frames=%d encoder_groups=%d",
             config->audio.source.enabled,
             config->audio.source.capture.device_name ? config->audio.source.capture.device_name : "unknown",
             config->audio.source.capture.sample_rate,
             config->audio.source.capture.channels,
             config->audio.source.capture.period_frames,
             config->audio.source.encoder_group_count);
    LOG_INFO("[MAIN_CFG] parsed audio mixer enabled=%d card=%s control=%s value=%s",
             config->audio.source.capture.mixer.enabled,
             config->audio.source.capture.mixer.card_name ? config->audio.source.capture.mixer.card_name : "unknown",
             config->audio.source.capture.mixer.control_name ? config->audio.source.capture.mixer.control_name : "unknown",
             config->audio.source.capture.mixer.value_name ? config->audio.source.capture.mixer.value_name : "unknown");
    LOG_INFO("[MAIN_CFG] parsed audio talkback enabled=%d device=%s rate=%d decoder_channels=%d playback_channels=%d frame_samples=%d prebuffer=%d max_packets=%d mixer=%d/%s",
             config->audio.talkback.enabled,
             config->audio.talkback.module.playback_device ? config->audio.talkback.module.playback_device : "unknown",
             config->audio.talkback.module.sample_rate,
             config->audio.talkback.module.decoder_channels,
             config->audio.talkback.module.playback_channels,
             config->audio.talkback.module.frame_samples_per_channel,
             config->audio.talkback.module.jitter_prebuffer_packets,
             config->audio.talkback.module.jitter_max_packets,
             config->audio.talkback.module.mixer.enabled,
             config->audio.talkback.module.mixer.value_name ? config->audio.talkback.module.mixer.value_name : "unknown");
    for (i = 0; i < config->audio.source.encoder_group_count; ++i)
    {
        audio_group = &config->audio.source.encoder_groups[i];
        LOG_INFO("[MAIN_CFG] audio_encoder_group index=%d name=%s codec=%d rate=%d channels=%d input_channel=%d",
                 i,
                 audio_group->name,
                 audio_group->encoder.codec,
                 audio_group->encoder.sample_rate,
                 audio_group->encoder.channels,
                 audio_group->encoder.input_channel);
    }

    for (i = 0; i < config->video.stream_count && i < MEDIA_GATEWAY_MAX_STREAMS; ++i)
    {
        s = &config->video.streams[i];
        LOG_INFO("[MAIN_CFG] parsed stream=%d name=%s enabled=%d source=%d size=%dx%d fps=%d bitrate=%d rc=%d out(rtsp=%d rtmp=%d gb28181=%d webrtc=%d)",
                 i,
                 s->name ? s->name : "unknown",
                 s->enabled,
                 s->source_index,
                 s->width,
                 s->height,
                 s->fps,
                 s->bitrate,
                 s->rc_mode,
                 s->enable_rtsp,
                 s->enable_rtmp,
                 s->enable_gb28181,
                 s->enable_webrtc);
        LOG_INFO("[MAIN_CFG] audio_bindings stream=%d rtsp=%s rtmp=%s gb28181=%s webrtc=%s",
                 i,
                 s->rtsp_audio.encoder_group,
                 s->rtmp_audio.encoder_group,
                 s->gb28181_audio.encoder_group,
                 s->webrtc_audio.encoder_group);
    }
}

int main(int argc, char **argv)
{
    MediaGatewayCtx gateway = {};
    MediaGatewayConfig config = {};
    GatewaySignalContext signal_context;
    sigset_t exit_signal_set;
    sigset_t previous_signal_set;
    pthread_t signal_thread = {};
    const char *config_path = (argc > 1 && argv[1] && argv[1][0] != '\0') ? argv[1] : "media_gateway.toml";
    int shell_ready = 0;
    int gateway_ready = 0;
    int signal_mask_ready = 0;
    int signal_thread_ready = 0;
    int pthread_ret = 0;
    int ret = 0;

    /*
     * 在创建任何工作线程前屏蔽退出信号。后续线程会继承该掩码，确保 SIGINT 和
     * SIGTERM 只由信号等待线程同步消费，而不会触发默认的立即终止行为。
     */
    sigemptyset(&exit_signal_set);
    sigaddset(&exit_signal_set, SIGINT);
    sigaddset(&exit_signal_set, SIGTERM);
    pthread_ret = pthread_sigmask(SIG_BLOCK, &exit_signal_set, &previous_signal_set);
    if (pthread_ret != 0)
    {
        fprintf(stderr, "failed to block exit signals: ret=%d\n", pthread_ret);
        return -1;
    }
    signal_mask_ready = 1;

    if (def_value_init(config_path) != 0)
    {
        fprintf(stderr, "failed to load TOML config: %s\n", config_path);
        ret = -1;
        goto out;
    }
    def_value_get_media_gateway_config(&config);
    log_set_level((LogLevel)config.system.log.level);
    LOG_INFO("loaded TOML config: %s", def_value_source_path());

    log_main_config_snapshot(&config);

    if (debug_command_server_init(NULL) == 0)
    {
        shell_ready = 1;
        if (system_debug_init() != 0)
        {
            LOG_WARN("system debug init failed");
        }
    }
    else
    {
        LOG_WARN("shell command server init failed, runtime shell debug disabled");
    }

    if (media_gateway_init(&gateway, &config) < 0)
    {
        LOG_ERROR("failed to initialize media gateway");
        ret = -1;
        goto out;
    }
    gateway_ready = 1;

    if (shell_ready && debug_command_server_start() != 0)
    {
        LOG_WARN("shell command server start failed, runtime shell debug disabled");
    }

    /*
     * 信号线程只把进程退出请求转换为 media_gateway_stop()。帧源停止、编码 worker
     * 回收、MPP 释放以及 V4L2/DMA-BUF 释放仍由原有主流程统一完成。
     */
    signal_context.gateway = &gateway;
    signal_context.shutdown_requested.store(false);
    pthread_ret = pthread_create(&signal_thread, NULL, gateway_signal_thread_main, &signal_context);
    if (pthread_ret != 0)
    {
        LOG_ERROR("failed to create gateway signal thread: ret=%d", pthread_ret);
        ret = -1;
        goto out;
    }
    signal_thread_ready = 1;

    ret = media_gateway_run(&gateway);
    /* 网关因信号或内部错误返回后，先回收信号线程，再释放初始化期资源。 */
    signal_context.shutdown_requested.store(true);
    pthread_ret = pthread_join(signal_thread, NULL);
    if (pthread_ret != 0)
    {
        LOG_ERROR("failed to join gateway signal thread: ret=%d", pthread_ret);
        ret = -1;
    }
    signal_thread_ready = 0;

out:
    if (signal_thread_ready)
    {
        signal_context.shutdown_requested.store(true);
        pthread_ret = pthread_join(signal_thread, NULL);
        if (pthread_ret != 0)
        {
            LOG_ERROR("failed to join gateway signal thread during cleanup: ret=%d", pthread_ret);
            ret = -1;
        }
    }
    if (gateway_ready)
    {
        media_gateway_deinit(&gateway);
    }
    if (shell_ready)
    {
        debug_command_server_deinit();
    }
    if (signal_mask_ready)
    {
        pthread_ret = pthread_sigmask(SIG_SETMASK, &previous_signal_set, NULL);
        if (pthread_ret != 0)
        {
            LOG_ERROR("failed to restore process signal mask: ret=%d", pthread_ret);
            ret = -1;
        }
    }
    return ret;
}
