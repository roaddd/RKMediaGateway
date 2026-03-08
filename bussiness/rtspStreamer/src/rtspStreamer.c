#include "rtspStreamer.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "rtsp_server_api.h"

#define DEFAULT_RTSP_SESSION "live"
#define DEFAULT_RTSP_IP "0.0.0.0"
#define DEFAULT_RTSP_PORT 8554
#define DEFAULT_RTSP_USER "admin"
#define DEFAULT_RTSP_PASSWORD "123456"
#define DEFAULT_ENCODE_FPS 30
#define DEFAULT_ENCODE_BITRATE (2 * 1024 * 1024) /* 默认码率：2Mbps */
#define DEFAULT_ENCODE_GOP 30
#define H264_NAL_LOG_EVERY_N_FRAMES 60

typedef struct {
    RtspStreamerCtx *ctx;
    int ret;
} RtspServerThreadArgs;

static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int start_code_len(const uint8_t *data, size_t len) {
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        return 4;
    }
    if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return 3;
    }
    return 0;
}

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

static int send_h264_annexb(void *session,
                            uint8_t *data,
                            size_t len,
                            uint64_t frame_id,
                            uint64_t *send_video_before_ts_us) {
    static unsigned long long frame_seq = 0;
    size_t nalu_start = 0;
    int code_len = 0;
    int nalu_count = 0;
    char nalu_log[256];
    size_t nalu_log_len = 0;

    nalu_log[0] = '\0';
    {
        uint64_t ts = get_now_us();
        if (send_video_before_ts_us) {
            *send_video_before_ts_us = ts;
        }
        printf("[TRACE] frame=%" PRIu64 " step=before_sessionSendVideoData ts_us=%" PRIu64 "\n",
               frame_id, ts);
    }

    // 这里按“单个 NALU”发送给 rtsp_server。
    // 原因是 MPP 输出通常是 Annex-B，一个缓冲里可能带多个 NALU。
    // 如果整块直接送下去，底层可能按单个 NALU 处理，导致 RTP 打包边界错误。
    // 因此先拆包，再逐个调用 sessionSendVideoData()，兼容性更稳。
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
        // 低延迟思路：
        // 高频日志会阻塞实时线程，默认仅打印关键帧/SPS/PPS和周期性采样日志。
        if (should_log) {
            printf("[H264] frame=%llu nalu_count=%d types=%s\n",
                   frame_seq, nalu_count, nalu_log);
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
        if (should_log) {
            // 低延迟思路：
            // 只保留关键诊断日志，避免 I/O 抢占编码/发送时间片。
            printf("[H264] frame=%llu nalu_count=%d types=%s\n",
                   frame_seq, nalu_count, nalu_count > 0 ? nalu_log : "none");
        }
        frame_seq++;
    }
    return 0;
}

static void fill_default_config(RtspStreamerConfig *dst, const RtspStreamerConfig *src) {
    memset(dst, 0, sizeof(*dst));
    if (src) {
        *dst = *src;
    }

    // 对外允许只传“部分配置”，这里把未填写字段补成默认值。
    // 这样上层传一个零初始化结构体，也能先跑通最小推流链路。
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
    // 低延迟思路：
    // GOP 越大，关键帧间隔越长；中途入会/丢包后恢复会更慢，体感就是“延迟大、追帧慢”。
    // 这里把过大的 GOP 收敛到 1 秒以内（gop <= fps），优先保证实时性。
    if (dst->gop > dst->fps) {
        dst->gop = dst->fps;
    }
}

static void *rtsp_server_thread(void *arg) {
    RtspServerThreadArgs *thread_args = (RtspServerThreadArgs *)arg;
    RtspStreamerCtx *ctx = thread_args->ctx;

    // rtspStartServer() 内部会阻塞在监听/accept 循环里，
    // 所以放在线程里，避免卡住主线程的采集和编码。
    thread_args->ret = rtspStartServer(ctx->config.auth_enable,
                                       ctx->config.server_ip,
                                       ctx->config.server_port,
                                       ctx->config.user,
                                       ctx->config.password);
    return NULL;
}

int rtsp_streamer_init(RtspStreamerCtx *ctx, const RtspStreamerConfig *config) {
    if (!ctx) {
        fprintf(stderr, "[ERROR] rtsp streamer ctx is NULL\n");
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    fill_default_config(&ctx->config, config);

    // 第一步：先初始化采集端。
    // 后续编码和推流都依赖采集到的 NV12 原始帧，所以采集必须先可用。
    if (v4l2_capture_init(&ctx->capture) < 0) {
        fprintf(stderr, "[ERROR] v4l2_capture_init failed\n");
        goto fail;
    }
    ctx->capture_ready = 1;

    // 第二步：初始化 MPP 编码器。
    // 这里固定使用与采集一致的分辨率，避免尺寸不一致时出现 stride 错误或输入不匹配。
    if (mpp_encoder_init(&ctx->encoder,
                         CAPTURE_WIDTH,
                         CAPTURE_HEIGHT,
                         ctx->config.fps,
                         ctx->config.bitrate,
                         ctx->config.gop) < 0) {
        fprintf(stderr, "[ERROR] mpp_encoder_init failed\n");
        goto fail;
    }
    ctx->encoder_ready = 1;

    // 第三步：初始化 RTSP 模块。
    // 放在采集和编码之后，避免前面失败时留下一个不可用的 RTSP 服务状态。
    if (rtspModuleInit() < 0) {
        fprintf(stderr, "[ERROR] rtspModuleInit failed\n");
        goto fail;
    }
    ctx->rtsp_module_ready = 1;

    // 创建自定义 session，客户端通过 rtsp://ip:port/session_name 拉流。
    ctx->rtsp_session = rtspAddSession(ctx->config.session_name);
    if (!ctx->rtsp_session) {
        fprintf(stderr, "[ERROR] rtspAddSession failed\n");
        goto fail;
    }

    // 当前链路只推 H264 视频，所以这里只注册视频媒体类型。
    if (sessionAddVideo(ctx->rtsp_session, VIDEO_H264) < 0) {
        fprintf(stderr, "[ERROR] sessionAddVideo failed\n");
        goto fail;
    }

    ctx->running = 1;
    printf("[INFO] rtsp url: rtsp://%s:%d/%s\n",
           ctx->config.server_ip,
           ctx->config.server_port,
           ctx->config.session_name);
    return 0;

fail:
    // 统一走一个失败清理出口，避免每个失败分支都手写释放逻辑。
    // 这样后续继续扩展功能时，不容易漏资源。
    rtsp_streamer_deinit(ctx);
    return -1;
}

int rtsp_streamer_run(RtspStreamerCtx *ctx) {
    if (!ctx || !ctx->running || !ctx->rtsp_session) {
        return -1;
    }

    pthread_t server_tid;
    RtspServerThreadArgs thread_args;
    uint8_t *raw_frame = NULL;
    int raw_len = 0;
    uint64_t frame_id = 0;
    int ret = 0;

    memset(&thread_args, 0, sizeof(thread_args));
    thread_args.ctx = ctx;

    // RTSP 服务线程负责监听客户端连接。
    // 主线程只负责“采集 -> 编码 -> 送流”，职责更清晰，也避免互相阻塞。
    ret = pthread_create(&server_tid, NULL, rtsp_server_thread, &thread_args);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] pthread_create rtsp server failed: %d\n", ret);
        return -1;
    }

    // 主链路：
    // 1. 从 V4L2 抓取 NV12 原始帧
    // 2. 送入 MPP 编码成 H264 Annex-B
    // 3. 拆成 NALU 后推给 RTSP session
    while (ctx->running) {
        uint8_t *h264_data = NULL;
        size_t h264_len = 0;
        uint64_t dqbuf_ts_us = 0;
        uint64_t driver_to_dqbuf_us = 0;
        uint64_t encode_put_ts_us = 0;
        uint64_t encode_get_ts_us = 0;
        uint64_t send_video_before_ts_us = 0;

        // 这里依赖 v4l2_capture_frame() 的生命周期修复：
        // raw_frame 现在是复制到用户态 frame_cache 的稳定数据，
        // 不是驱动那块会被循环复用的 mmap 原始缓冲。
        // 所以即使底层缓冲已经重新 QBUF 回驱动，
        // 当前这份 raw_frame 在本轮编码结束前仍然可安全读取，不会被新帧覆盖。
        if (v4l2_capture_frame(&ctx->capture,
                               &raw_frame,
                               &raw_len,
                               &frame_id,
                               &dqbuf_ts_us,
                               &driver_to_dqbuf_us) < 0) {
            ret = -1;
            break;
        }

        if (mpp_encoder_encode_frame(&ctx->encoder,
                                     raw_frame,
                                     (size_t)raw_len,
                                     frame_id,
                                     &h264_data,
                                     &h264_len,
                                     NULL,
                                     &encode_put_ts_us,
                                     &encode_get_ts_us) < 0) {
            ret = -1;
            break;
        }

        if (!h264_data || h264_len == 0) {
            // 编码器刚启动时可能会有短暂缓存阶段，暂时没有输出不算硬错误。
            continue;
        }

        if (send_h264_annexb(ctx->rtsp_session,
                             h264_data,
                             h264_len,
                             frame_id,
                             &send_video_before_ts_us) < 0) {
            ret = -1;
            break;
        }

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

    ctx->running = 0;
    rtspStopServer();
    pthread_join(server_tid, NULL);

    if (thread_args.ret < 0 && ret == 0) {
        ret = thread_args.ret;
    }

    return ret;
}

void rtsp_streamer_stop(RtspStreamerCtx *ctx) {
    if (!ctx) {
        return;
    }

    // 这里只改运行标志，不直接释放资源。
    // 资源统一在 deinit() 中处理，避免多处并发清理。
    ctx->running = 0;
}

void rtsp_streamer_deinit(RtspStreamerCtx *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->rtsp_session) {
        rtspDelSession(ctx->rtsp_session);
        ctx->rtsp_session = NULL;
    }

    // 按“谁成功初始化过，谁就释放”的原则清理。
    // 这样即使 init() 中途失败，也可以安全复用同一套释放逻辑。
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
}
