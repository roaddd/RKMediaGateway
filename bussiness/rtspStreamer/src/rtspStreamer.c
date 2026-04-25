#include "rtspStreamer.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>

#include "rtsp_server_api.h"

#define DEFAULT_RTSP_SESSION "live"
#define DEFAULT_RTSP_IP "0.0.0.0"
#define DEFAULT_RTSP_PORT 8554
#define DEFAULT_RTSP_USER "admin"
#define DEFAULT_RTSP_PASSWORD "123456"
#define DEFAULT_ENCODE_FPS 30
#define DEFAULT_ENCODE_BITRATE (2 * 1024 * 1024)
#define DEFAULT_ENCODE_GOP 30
#define DEFAULT_RC_MODE MPP_ENC_RC_MODE_CBR
#define DEFAULT_H264_PROFILE 100
#define DEFAULT_H264_LEVEL 40
#define DEFAULT_H264_CABAC_EN 1
#define DEFAULT_LOW_LATENCY_MODE 1
#define DEFAULT_STATS_INTERVAL_SEC 1
#define DEFAULT_CAPTURE_RETRY_MS 5
#define DEFAULT_MAX_CONSECUTIVE_FAILURES 30
#define DEFAULT_RECORD_FLUSH_INTERVAL_FRAMES 30
#define H264_NAL_LOG_EVERY_N_FRAMES 60

typedef struct {
    RtspStreamerCtx *ctx;  /* 回指主 RTSP streamer 上下文，供服务线程读取配置。 */
    int ret;               /* 服务线程退出时保存 rtspStartServer() 返回值。 */
} RtspServerThreadArgs;

/**
 * @description: 获取当前单调时钟时间，单位微秒
 * @return {static uint64_t}
 */
static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 获取当前实时时钟时间，单位微秒
 * @return {static uint64_t}
 */
static uint64_t get_realtime_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 将业务配置转换为编码器参数
 * @param {const RtspStreamerConfig *} cfg
 * @param {MppEncoderOptions *} opt
 * @return {static void}
 */
static void build_encoder_options(const RtspStreamerConfig *cfg, MppEncoderOptions *opt) {
    if (!cfg || !opt) {
        return;
    }
    memset(opt, 0, sizeof(*opt));
    opt->rc_mode = cfg->rc_mode;
    opt->h264_profile = cfg->h264_profile;
    opt->h264_level = cfg->h264_level;
    opt->h264_cabac_en = cfg->h264_cabac_en;
}

/**
 * @description: 重新初始化编码器实例
 * @param {RtspStreamerCtx *} ctx
 * @return {static int}
 */
static int reset_encoder(RtspStreamerCtx *ctx) {
    MppEncoderOptions options;
    if (!ctx) {
        return -1;
    }
    build_encoder_options(&ctx->config, &options);

    if (ctx->encoder_ready) {
        mpp_encoder_deinit(&ctx->encoder);
        ctx->encoder_ready = 0;
    }

    // 编码器异常后在这里重建，尽量沿用当前配置，避免 RTSP 主流程直接退出。
    if (mpp_encoder_init(&ctx->encoder,
                         CAPTURE_WIDTH,
                         CAPTURE_HEIGHT,
                         ctx->config.fps,
                         ctx->config.bitrate,
                         ctx->config.gop,
                         &options) < 0) {
        return -1;
    }

    ctx->encoder_ready = 1;
    return 0;
}

/**
 * @description: 判断当前位置是否为 H264 起始码并返回长度
 * @param {const uint8_t *} data
 * @param {size_t} len
 * @return {static int}
 */
static int start_code_len(const uint8_t *data, size_t len) {
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        return 4;
    }
    if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return 3;
    }
    return 0;
}

/**
 * @description: 查找下一个 H264 起始码位置
 * @param {const uint8_t *} data
 * @param {size_t} len
 * @param {size_t} offset
 * @param {size_t *} pos
 * @param {int *} code_len
 * @return {static int}
 */
static int find_start_code(const uint8_t *data, size_t len, size_t offset, size_t *pos, int *code_len) {
    for (size_t i = offset; i + 3 <= len; ++i) {
        int cur_len = start_code_len(data + i, len - i);
        if (cur_len > 0) {
            *pos = i;
            *code_len = cur_len;
            return 0;
        }
    }
    return -1;
}

/**
 * @description: 返回 H264 NALU 类型名称
 * @param {uint8_t} nalu_type
 * @return {static const char *}
 */
static const char *h264_nalu_type_name(uint8_t nalu_type) {
    switch (nalu_type) {
        case 1: return "NON_IDR";
        case 5: return "IDR";
        case 6: return "SEI";
        case 7: return "SPS";
        case 8: return "PPS";
        case 9: return "AUD";
        default: return "OTHER";
    }
}

/**
 * @description: 拆分并发送 H264 Annex-B 码流
 * @param {void *} session
 * @param {uint8_t *} data
 * @param {size_t} len
 * @param {uint64_t} frame_id
 * @param {uint64_t *} send_video_before_ts_us
 * @param {int} low_latency_mode
 * @return {static int}
 */
static int send_h264_annexb(void *session,
                            uint8_t *data,
                            size_t len,
                            uint64_t frame_id,
                            uint64_t *send_video_before_ts_us,
                            int low_latency_mode) {
    static unsigned long long frame_seq = 0;
    size_t nalu_start = 0;
    int code_len = 0;
    int nalu_count = 0;
    char nalu_log[256];
    size_t nalu_log_len = 0;

    nalu_log[0] = '\0';

    // 这里把 Annex-B 码流拆成独立 NALU 后逐个送给 rtsp_server。
    // MPP 输出的是带起始码的 H264 Annex-B，需要先找到每个 NALU 的边界。
    // 发送时去掉起始码，只把真正的 NALU payload 交给 RTP 打包模块。
    // 如果没有找到起始码，就退化为把整块数据直接送给 sessionSendVideoData()。
    if (find_start_code(data, len, 0, &nalu_start, &code_len) != 0) {
        uint8_t nalu_type = (len > 0) ? (data[0] & 0x1F) : 0;
        int should_log = (nalu_type == 5 || nalu_type == 7 || nalu_type == 8 ||
                          ((frame_seq % H264_NAL_LOG_EVERY_N_FRAMES) == 0));
        snprintf(nalu_log, sizeof(nalu_log), "%u(%s)",
                 (unsigned)nalu_type,
                 h264_nalu_type_name(nalu_type));
        nalu_count = (len > 0) ? 1 : 0;
        if (sessionSendVideoData(session, data, (int)len) < 0) {
            fprintf(stderr, "[ERROR] sessionSendVideoData failed\n");
            return -1;
        }
        // 单 NALU 场景下也保留关键帧日志，便于确认 IDR/SPS/PPS 是否正常输出。
        if (should_log && !low_latency_mode) {
            printf("[H264] frame=%llu nalu_count=%d types=%s\n", frame_seq, nalu_count, nalu_log);
        }
        frame_seq++;
        return 0;
    }

    while (nalu_start < len) {
        size_t payload_start = nalu_start + (size_t)code_len;
        size_t next_start = len;
        int next_code_len = 0;

        if (payload_start >= len) {
            break;
        }

        find_start_code(data, len, payload_start, &next_start, &next_code_len);

        if (next_start > payload_start) {
            uint8_t nalu_type = data[payload_start] & 0x1F;
            int written = 0;

            if (nalu_log_len < sizeof(nalu_log)) {
                written = snprintf(nalu_log + nalu_log_len,
                                   sizeof(nalu_log) - nalu_log_len,
                                   "%s%u(%s)",
                                   (nalu_count == 0) ? "" : ",",
                                   (unsigned)nalu_type,
                                   h264_nalu_type_name(nalu_type));
                if (written > 0) {
                    nalu_log_len += (size_t)written;
                    if (nalu_log_len >= sizeof(nalu_log)) {
                        nalu_log_len = sizeof(nalu_log) - 1;
                    }
                }
            }
            nalu_count++;

            if (sessionSendVideoData(session,
                                     data + payload_start,
                                     (int)(next_start - payload_start)) < 0) {
                fprintf(stderr, "[ERROR] sessionSendVideoData failed\n");
                return -1;
            }
        }

        if (next_start >= len) {
            break;
        }

        nalu_start = next_start;
        code_len = next_code_len;
    }

    {
        int has_key = (strstr(nalu_log, "5(IDR)") != NULL) ||
                      (strstr(nalu_log, "7(SPS)") != NULL) ||
                      (strstr(nalu_log, "8(PPS)") != NULL);
        int should_log = has_key || ((frame_seq % H264_NAL_LOG_EVERY_N_FRAMES) == 0);
        if (should_log && !low_latency_mode) {
            // 日志打印本身有 I/O 开销，只在关键帧或抽样帧输出，避免刷屏。
            printf("[H264] frame=%llu nalu_count=%d types=%s\n",
                   frame_seq, nalu_count, nalu_count > 0 ? nalu_log : "none");
        }
        frame_seq++;
    }

    {
        uint64_t ts = get_now_us();
        if (send_video_before_ts_us) {
            *send_video_before_ts_us = ts;
        }
        // printf("[TRACE] frame=%" PRIu64 " step=before_sessionSendVideoData ts_us=%" PRIu64 "\n",
        //        frame_id, ts);
    }

    return 0;
}

/**
 * @description: 填充默认配置项
 * @param {RtspStreamerConfig *} dst
 * @param {const RtspStreamerConfig *} src
 * @return {static void}
 */
static void fill_default_config(RtspStreamerConfig *dst, const RtspStreamerConfig *src) {
    memset(dst, 0, sizeof(*dst));
    if (src) {
        *dst = *src;
    }

    // 先复制用户传入配置，再把空值或非法值补成默认值。
    // 这样调用方可以只覆盖少数字段，剩余参数保持稳定可用。
    if (!dst->session_name) {
        dst->session_name = DEFAULT_RTSP_SESSION;
    }
    if (!dst->server_ip) {
        dst->server_ip = DEFAULT_RTSP_IP;
    }
    if (dst->server_port <= 0) {
        dst->server_port = DEFAULT_RTSP_PORT;
    }
    if (!dst->user) {
        dst->user = DEFAULT_RTSP_USER;
    }
    if (!dst->password) {
        dst->password = DEFAULT_RTSP_PASSWORD;
    }
    if (dst->fps <= 0) {
        dst->fps = DEFAULT_ENCODE_FPS;
    }
    if (dst->bitrate <= 0) {
        dst->bitrate = DEFAULT_ENCODE_BITRATE;
    }
    if (dst->gop <= 0) {
        dst->gop = DEFAULT_ENCODE_GOP;
    }
    if (dst->rc_mode <= 0) {
        dst->rc_mode = DEFAULT_RC_MODE;
    }
    if (dst->h264_profile <= 0) {
        dst->h264_profile = DEFAULT_H264_PROFILE;
    }
    if (dst->h264_level <= 0) {
        dst->h264_level = DEFAULT_H264_LEVEL;
    }
    if (dst->h264_cabac_en < 0) {
        dst->h264_cabac_en = DEFAULT_H264_CABAC_EN;
    }
    if (dst->low_latency_mode <= 0) {
        dst->low_latency_mode = DEFAULT_LOW_LATENCY_MODE;
    }
    if (dst->stats_interval_sec <= 0) {
        dst->stats_interval_sec = DEFAULT_STATS_INTERVAL_SEC;
    }
    if (dst->capture_retry_ms <= 0) {
        dst->capture_retry_ms = DEFAULT_CAPTURE_RETRY_MS;
    }
    if (dst->max_consecutive_failures <= 0) {
        dst->max_consecutive_failures = DEFAULT_MAX_CONSECUTIVE_FAILURES;
    }
    if (dst->record_flush_interval_frames <= 0) {
        dst->record_flush_interval_frames = DEFAULT_RECORD_FLUSH_INTERVAL_FRAMES;
    }
    if (dst->gop > dst->fps) {
        dst->gop = dst->fps;
    }
}

/**
 * @description: RTSP 服务线程入口函数
 * @param {void *} arg
 * @return {static void *}
 */
static void *rtsp_server_thread(void *arg) {
    RtspServerThreadArgs *thread_args = (RtspServerThreadArgs *)arg;
    RtspStreamerCtx *ctx = thread_args->ctx;

    // rtspStartServer() 会阻塞在内部循环/accept 上，因此放到独立线程中运行。
    // 主线程继续负责采集、编码和推流，避免整个流程被服务端启动逻辑卡住。
    thread_args->ret = rtspStartServer(ctx->config.auth_enable,
                                       ctx->config.server_ip,
                                       ctx->config.server_port,
                                       ctx->config.user,
                                       ctx->config.password);
    return NULL;
}

/**
 * @description: 初始化 RTSP 推流模块
 * @param {RtspStreamerCtx *} ctx
 * @param {const RtspStreamerConfig *} config
 * @return {int}
 */
int rtsp_streamer_init(RtspStreamerCtx *ctx, const RtspStreamerConfig *config) {
    if (!ctx) {
        fprintf(stderr, "[ERROR] rtsp streamer ctx is NULL\n");
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    fill_default_config(&ctx->config, config);

    // 先初始化采集模块。
    // 当前流程默认从摄像头取出 NV12 原始帧，后续直接送入编码器。
    if (v4l2_capture_init(&ctx->capture) < 0) {
        fprintf(stderr, "[ERROR] v4l2_capture_init failed\n");
        goto fail;
    }
    ctx->capture_ready = 1;

    {
        // 再初始化 MPP 编码器。
        // 这里使用固定采集分辨率，编码参数由配置控制；stride 等底层细节交给编码模块处理。
        MppEncoderOptions options;
        build_encoder_options(&ctx->config, &options);
        if (mpp_encoder_init(&ctx->encoder,
                             CAPTURE_WIDTH,
                             CAPTURE_HEIGHT,
                             ctx->config.fps,
                             ctx->config.bitrate,
                             ctx->config.gop,
                             &options) < 0) {
            fprintf(stderr, "[ERROR] mpp_encoder_init failed\n");
            goto fail;
        }
    }
    ctx->encoder_ready = 1;

    // 初始化 RTSP 模块。
    // 只有模块初始化成功后，后面的 session 创建和推流接口才可用。
    if (rtspModuleInit() < 0) {
        fprintf(stderr, "[ERROR] rtspModuleInit failed\n");
        goto fail;
    }
    ctx->rtsp_module_ready = 1;

    // 创建推流 session，客户端最终通过 rtsp://ip:port/session_name 访问。
    ctx->rtsp_session = rtspAddSession(ctx->config.session_name);
    if (!ctx->rtsp_session) {
        fprintf(stderr, "[ERROR] rtspAddSession failed\n");
        goto fail;
    }

    // 声明当前 session 的视频类型为 H264，便于 RTSP 侧生成正确描述信息。
    if (sessionAddVideo(ctx->rtsp_session, VIDEO_H264) < 0) {
        fprintf(stderr, "[ERROR] sessionAddVideo failed\n");
        goto fail;
    }

    if (ctx->config.record_file_path && ctx->config.record_file_path[0] != '\0') {
        // 如果启用了本地录制，就把编码后的 Annex-B 码流原样追加写入文件。
        ctx->record_fp = fopen(ctx->config.record_file_path, "ab");
        if (!ctx->record_fp) {
            fprintf(stderr, "[ERROR] open record file failed: %s (errno=%d)\n",
                    ctx->config.record_file_path, errno);
            goto fail;
        }
        printf("[INFO] local record enabled: %s\n", ctx->config.record_file_path);
    }

    ctx->running = 1;
    ctx->stat_last_ts_us = get_now_us();
    ctx->stat_frames = 0;
    ctx->stat_bytes = 0;

    printf("[INFO] rtsp url: rtsp://%s:%d/%s\n",
           ctx->config.server_ip,
           ctx->config.server_port,
           ctx->config.session_name);
    return 0;

fail:
    // 初始化任一步骤失败都统一走这里清理，避免资源半初始化后泄漏。
    // deinit() 按 ready 标记逆序释放，所以可以安全处理部分初始化成功的情况。
    rtsp_streamer_deinit(ctx);
    return -1;
}

/**
 * @description: 运行 RTSP 推流主循环
 * @param {RtspStreamerCtx *} ctx
 * @return {int}
 */
int rtsp_streamer_run(RtspStreamerCtx *ctx) {
    if (!ctx || !ctx->running || !ctx->rtsp_session) {
        return -1;
    }

    pthread_t server_tid;
    RtspServerThreadArgs thread_args;
    uint8_t *raw_frame = NULL;
    int raw_len = 0;
    uint64_t frame_id = 0;
    int consecutive_capture_fail = 0;
    int consecutive_encode_fail = 0;
    int ret = 0;

    memset(&thread_args, 0, sizeof(thread_args));
    thread_args.ctx = ctx;

    // RTSP 服务线程先启动起来，确保客户端连接时服务端已经就绪。
    // 主循环随后执行完整链路：采集 -> 编码 -> 拆包发送，形成持续推流。
    ret = pthread_create(&server_tid, NULL, rtsp_server_thread, &thread_args);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] pthread_create rtsp server failed: %d\n", ret);
        return -1;
    }

    // 主循环流程：
    // 1. 从 V4L2 获取 NV12 原始帧
    // 2. 送入 MPP 编码成 H264 Annex-B
    // 3. 按 NALU 拆分后发送到 RTSP session
    while (ctx->running) {
        uint8_t *h264_data = NULL;
        size_t h264_len = 0;
        uint64_t dqbuf_ts_us = 0;
        uint64_t driver_to_dqbuf_us = 0;
        uint64_t frame_copy_us = 0;
        uint64_t encode_put_ts_us = 0;
        uint64_t encode_get_ts_us = 0;
        uint64_t send_video_before_ts_us = 0;

        // v4l2_capture_frame() 返回一帧可直接编码的采集数据。
        // raw_frame 指向 capture 模块内部的 frame_cache，不需要这里额外申请或释放。
        // 采集模块内部已经处理了 mmap 缓冲区的出队与回队。
        // 调用方只需要消费结果，不需要自己再做 QBUF。
        // 如果采集失败，通常是暂时性抖动，稍等片刻后重试即可。
        if (v4l2_capture_frame(&ctx->capture,
                               &raw_frame,
                               &raw_len,
                               &frame_id,
                               &dqbuf_ts_us,
                               &driver_to_dqbuf_us,
                               &frame_copy_us) < 0) {
            consecutive_capture_fail++;
            if (consecutive_capture_fail >= ctx->config.max_consecutive_failures) {
                fprintf(stderr, "[ERROR] capture failed %d times continuously\n", consecutive_capture_fail);
                ret = -1;
                break;
            }
            // 采集失败先短暂退避，避免设备异常时空转占满 CPU。
            usleep((useconds_t)ctx->config.capture_retry_ms * 1000U);
            continue;
        }
        consecutive_capture_fail = 0;

        if (mpp_encoder_encode_frame(&ctx->encoder,
                                     raw_frame,
                                     (size_t)raw_len,
                                     frame_id,
                                     &h264_data,
                                     &h264_len,
                                     NULL,
                                     &encode_put_ts_us,
                                     &encode_get_ts_us,
                                     NULL) < 0) {
            consecutive_encode_fail++;
            if (consecutive_encode_fail >= 3) {
                fprintf(stderr, "[WARN] encoder failed %d times, try reset\n", consecutive_encode_fail);
                if (reset_encoder(ctx) < 0) {
                    ret = -1;
                    break;
                }
                consecutive_encode_fail = 0;
            }
            continue;
        }
        consecutive_encode_fail = 0;

        if (!h264_data || h264_len == 0) {
            // 某些帧可能暂时拿不到有效输出，直接跳过，等待下一帧即可。
            continue;
        }

        if (send_h264_annexb(ctx->rtsp_session,
                             h264_data,
                             h264_len,
                             frame_id,
                             &send_video_before_ts_us,
                             ctx->config.low_latency_mode) < 0) {
            ret = -1;
            break;
        }

        if (ctx->record_fp) {
            size_t written = fwrite(h264_data, 1, h264_len, ctx->record_fp);
            if (written != h264_len) {
                fprintf(stderr, "[WARN] local record write short: %zu/%zu\n", written, h264_len);
            }
            if ((frame_id % (uint64_t)ctx->config.record_flush_interval_frames) == 0) {
                fflush(ctx->record_fp);
            }
        }

        ctx->stat_frames++;
        ctx->stat_bytes += h264_len;
        {
            uint64_t now = get_now_us();
            uint64_t span_us = now - ctx->stat_last_ts_us;
            if (span_us >= (uint64_t)ctx->config.stats_interval_sec * 1000000ULL) {
                double span_sec = (double)span_us / 1000000.0;
                double fps = (span_sec > 0.0) ? ((double)ctx->stat_frames / span_sec) : 0.0;
                double kbps = (span_sec > 0.0) ? ((double)ctx->stat_bytes * 8.0 / 1000.0 / span_sec) : 0.0;
                printf("[STAT] fps=%.2f bitrate=%.2fkbps frames=%" PRIu64 " bytes=%" PRIu64 "\n",
                       fps, kbps, ctx->stat_frames, ctx->stat_bytes);
                ctx->stat_frames = 0;
                ctx->stat_bytes = 0;
                ctx->stat_last_ts_us = now;
            }
        }

        if (!ctx->config.low_latency_mode) {
            printf("[TRACE_SUM] frame=%" PRIu64
                   " driver_to_dqbuf=%" PRIu64 "us"
                   " dqbuf_to_put=%" PRIu64 "us"
                   " put_to_get=%" PRIu64 "us"
                   " get_to_send=%" PRIu64 "us"
                   " dqbuf_to_send=%" PRIu64 "us\n",
                   frame_id,
                   driver_to_dqbuf_us,
                   (encode_put_ts_us >= dqbuf_ts_us) ? (encode_put_ts_us - dqbuf_ts_us) : 0,
                   (encode_get_ts_us >= encode_put_ts_us) ? (encode_get_ts_us - encode_put_ts_us) : 0,
                   (send_video_before_ts_us >= encode_get_ts_us) ? (send_video_before_ts_us - encode_get_ts_us) : 0,
                   (send_video_before_ts_us >= dqbuf_ts_us) ? (send_video_before_ts_us - dqbuf_ts_us) : 0);
        }
    }

    ctx->running = 0;
    rtspStopServer();
    pthread_join(server_tid, NULL);

    if (thread_args.ret < 0 && ret == 0) {
        ret = thread_args.ret;
    }

    return ret;
}

/**
 * @description: 请求停止 RTSP 推流主循环
 * @param {RtspStreamerCtx *} ctx
 * @return {void}
 */
void rtsp_streamer_stop(RtspStreamerCtx *ctx) {
    if (!ctx) {
        return;
    }

    // 这里只设置停止标志，让主循环自然退出。
    // 真正的资源释放留给 deinit()，避免并发清理带来竞态。
    ctx->running = 0;
}

/**
 * @description: 释放 RTSP 推流模块资源
 * @param {RtspStreamerCtx *} ctx
 * @return {void}
 */
void rtsp_streamer_deinit(RtspStreamerCtx *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->record_fp) {
        fflush(ctx->record_fp);
        fclose(ctx->record_fp);
        ctx->record_fp = NULL;
    }

    if (ctx->rtsp_session) {
        rtspDelSession(ctx->rtsp_session);
        ctx->rtsp_session = NULL;
    }

    // 按与初始化相反的顺序释放资源，保证依赖关系正确。
    // 结合各 ready 标记，即使 init() 中途失败也能安全执行这里的清理。
    if (ctx->rtsp_module_ready) {
        rtspModuleDel();
        ctx->rtsp_module_ready = 0;
    }

    if (ctx->encoder_ready) {
        mpp_encoder_deinit(&ctx->encoder);
        ctx->encoder_ready = 0;
    }

    if (ctx->capture_ready) {
        v4l2_capture_deinit(&ctx->capture);
        ctx->capture_ready = 0;
    }

    memset(&ctx->config, 0, sizeof(ctx->config));
    ctx->running = 0;
    ctx->stat_last_ts_us = 0;
    ctx->stat_frames = 0;
    ctx->stat_bytes = 0;
}
