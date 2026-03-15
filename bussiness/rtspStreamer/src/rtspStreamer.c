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
    RtspStreamerCtx *ctx;
    int ret;
} RtspServerThreadArgs;

static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t get_realtime_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

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

    // 锟斤拷锟斤拷说锟斤拷锟斤拷锟斤拷锟斤拷失锟杰猴拷执锟斤拷锟斤拷锟截斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟叫讹拷 RTSP 锟斤拷锟斤拷锟竭程★拷
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
                            uint64_t *send_video_before_ts_us,
                            int low_latency_mode) {
    static unsigned long long frame_seq = 0;
    size_t nalu_start = 0;
    int code_len = 0;
    int nalu_count = 0;
    char nalu_log[256];
    size_t nalu_log_len = 0;

    nalu_log[0] = '\0';

    // 杩欓噷鎸夆€滃崟涓� NALU鈥濆彂閫佺粰 rtsp_server銆�
    // 鍘熷洜鏄� MPP 杈撳嚭閫氬父鏄� Annex-B锛屼竴涓紦鍐查噷鍙兘甯﹀涓� NALU銆�
    // 濡傛灉鏁村潡鐩存帴閫佷笅鍘伙紝搴曞眰鍙兘鎸夊崟涓� NALU 澶勭悊锛屽鑷� RTP 鎵撳寘杈圭晫閿欒銆�
    // 鍥犳鍏堟媶鍖咃紝鍐嶉€愪釜璋冪敤 sessionSendVideoData()锛屽吋瀹规€ф洿绋炽€�
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
        // 楂橀鏃ュ織浼氶樆濉炲疄鏃剁嚎绋嬶紝榛樿浠呮墦鍗板叧閿抚/SPS/PPS鍜屽懆鏈熸€ч噰鏍锋棩蹇椼€�
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
            // 鍙繚鐣欏叧閿瘖鏂棩蹇楋紝閬垮厤 I/O 鎶㈠崰缂栫爜/鍙戦€佹椂闂寸墖銆�
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

static void fill_default_config(RtspStreamerConfig *dst, const RtspStreamerConfig *src) {
    memset(dst, 0, sizeof(*dst));
    if (src) {
        *dst = *src;
    }

    // 瀵瑰鍏佽鍙紶鈥滈儴鍒嗛厤缃€濓紝杩欓噷鎶婃湭濉啓瀛楁琛ユ垚榛樿鍊笺€�
    // 杩欐牱涓婂眰浼犱竴涓浂鍒濆鍖栫粨鏋勪綋锛屼篃鑳藉厛璺戦€氭渶灏忔帹娴侀摼璺€�
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

static void *rtsp_server_thread(void *arg) {
    RtspServerThreadArgs *thread_args = (RtspServerThreadArgs *)arg;
    RtspStreamerCtx *ctx = thread_args->ctx;

    // rtspStartServer() 鍐呴儴浼氶樆濉炲湪鐩戝惉/accept 寰幆閲岋紝
    // 鎵€浠ユ斁鍦ㄧ嚎绋嬮噷锛岄伩鍏嶅崱浣忎富绾跨▼鐨勯噰闆嗗拰缂栫爜銆�
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

    // 绗竴姝ワ細鍏堝垵濮嬪寲閲囬泦绔€�
    // 鍚庣画缂栫爜鍜屾帹娴侀兘渚濊禆閲囬泦鍒扮殑 NV12 鍘熷甯э紝鎵€浠ラ噰闆嗗繀椤诲厛鍙敤銆�
    if (v4l2_capture_init(&ctx->capture) < 0) {
        fprintf(stderr, "[ERROR] v4l2_capture_init failed\n");
        goto fail;
    }
    ctx->capture_ready = 1;

    {
        // 绗簩姝ワ細鍒濆鍖� MPP 缂栫爜鍣ㄣ€�
        // 杩欓噷鍥哄畾浣跨敤涓庨噰闆嗕竴鑷寸殑鍒嗚鲸鐜囷紝閬垮厤灏哄涓嶄竴鑷存椂鍑虹幇 stride 閿欒鎴栬緭鍏ヤ笉鍖归厤銆�
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

    // 绗笁姝ワ細鍒濆鍖� RTSP 妯″潡銆�
    // 鏀惧湪閲囬泦鍜岀紪鐮佷箣鍚庯紝閬垮厤鍓嶉潰澶辫触鏃剁暀涓嬩竴涓笉鍙敤鐨� RTSP 鏈嶅姟鐘舵€併€�
    if (rtspModuleInit() < 0) {
        fprintf(stderr, "[ERROR] rtspModuleInit failed\n");
        goto fail;
    }
    ctx->rtsp_module_ready = 1;

    // 鍒涘缓鑷畾涔� session锛屽鎴风閫氳繃 rtsp://ip:port/session_name 鎷夋祦銆�
    ctx->rtsp_session = rtspAddSession(ctx->config.session_name);
    if (!ctx->rtsp_session) {
        fprintf(stderr, "[ERROR] rtspAddSession failed\n");
        goto fail;
    }

    // 褰撳墠閾捐矾鍙帹 H264 瑙嗛锛屾墍浠ヨ繖閲屽彧娉ㄥ唽瑙嗛濯掍綋绫诲瀷銆�
    if (sessionAddVideo(ctx->rtsp_session, VIDEO_H264) < 0) {
        fprintf(stderr, "[ERROR] sessionAddVideo failed\n");
        goto fail;
    }

    if (ctx->config.record_file_path && ctx->config.record_file_path[0] != '\0') {
        // 锟斤拷锟斤拷说锟斤拷锟斤拷锟斤拷锟斤拷录锟斤拷锟斤拷帽锟斤拷锟斤拷 Annex-B 锟斤拷锟斤拷直写锟斤拷锟斤拷锟解开锟斤拷小锟斤拷
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
    // 缁熶竴璧颁竴涓け璐ユ竻鐞嗗嚭鍙ｏ紝閬垮厤姣忎釜澶辫触鍒嗘敮閮芥墜鍐欓噴鏀鹃€昏緫銆�
    // 杩欐牱鍚庣画缁х画鎵╁睍鍔熻兘鏃讹紝涓嶅鏄撴紡璧勬簮銆�
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
    int consecutive_capture_fail = 0;
    int consecutive_encode_fail = 0;
    int ret = 0;

    memset(&thread_args, 0, sizeof(thread_args));
    thread_args.ctx = ctx;

    // RTSP 鏈嶅姟绾跨▼璐熻矗鐩戝惉瀹㈡埛绔繛鎺ャ€�
    // 涓荤嚎绋嬪彧璐熻矗鈥滈噰闆� -> 缂栫爜 -> 閫佹祦鈥濓紝鑱岃矗鏇存竻鏅帮紝涔熼伩鍏嶄簰鐩搁樆濉炪€�
    ret = pthread_create(&server_tid, NULL, rtsp_server_thread, &thread_args);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] pthread_create rtsp server failed: %d\n", ret);
        return -1;
    }

    // 涓婚摼璺細
    // 1. 浠� V4L2 鎶撳彇 NV12 鍘熷甯�
    // 2. 閫佸叆 MPP 缂栫爜鎴� H264 Annex-B
    // 3. 鎷嗘垚 NALU 鍚庢帹缁� RTSP session
    while (ctx->running) {
        uint8_t *h264_data = NULL;
        size_t h264_len = 0;
        uint64_t dqbuf_ts_us = 0;
        uint64_t driver_to_dqbuf_us = 0;
        uint64_t encode_put_ts_us = 0;
        uint64_t encode_get_ts_us = 0;
        uint64_t send_video_before_ts_us = 0;

        // 杩欓噷渚濊禆 v4l2_capture_frame() 鐨勭敓鍛藉懆鏈熶慨澶嶏細
        // raw_frame 鐜板湪鏄鍒跺埌鐢ㄦ埛鎬� frame_cache 鐨勭ǔ瀹氭暟鎹紝
        // 涓嶆槸椹卞姩閭ｅ潡浼氳寰幆澶嶇敤鐨� mmap 鍘熷缂撳啿銆�
        // 鎵€浠ュ嵆浣垮簳灞傜紦鍐插凡缁忛噸鏂� QBUF 鍥為┍鍔紝
        // 褰撳墠杩欎唤 raw_frame 鍦ㄦ湰杞紪鐮佺粨鏉熷墠浠嶇劧鍙畨鍏ㄨ鍙栵紝涓嶄細琚柊甯ц鐩栥€�
        if (v4l2_capture_frame(&ctx->capture,
                               &raw_frame,
                               &raw_len,
                               &frame_id,
                               &dqbuf_ts_us,
                               &driver_to_dqbuf_us) < 0) {
            consecutive_capture_fail++;
            if (consecutive_capture_fail >= ctx->config.max_consecutive_failures) {
                fprintf(stderr, "[ERROR] capture failed %d times continuously\n", consecutive_capture_fail);
                ret = -1;
                break;
            }
            // 锟斤拷锟斤拷说锟斤拷锟斤拷锟缴硷拷失锟杰猴拷锟斤拷锟斤拷锟斤拷卟锟斤拷锟斤拷裕锟斤拷锟斤拷锟斤拷斐ｏ拷指锟斤拷锟斤拷锟斤拷锟�
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
                                     &encode_get_ts_us) < 0) {
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
            // 缂栫爜鍣ㄥ垰鍚姩鏃跺彲鑳戒細鏈夌煭鏆傜紦瀛橀樁娈碉紝鏆傛椂娌℃湁杈撳嚭涓嶇畻纭敊璇€�
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

void rtsp_streamer_stop(RtspStreamerCtx *ctx) {
    if (!ctx) {
        return;
    }

    // 杩欓噷鍙敼杩愯鏍囧織锛屼笉鐩存帴閲婃斁璧勬簮銆�
    // 璧勬簮缁熶竴鍦� deinit() 涓鐞嗭紝閬垮厤澶氬骞跺彂娓呯悊銆�
    ctx->running = 0;
}

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

    // 鎸夆€滆皝鎴愬姛鍒濆鍖栬繃锛岃皝灏遍噴鏀锯€濈殑鍘熷垯娓呯悊銆�
    // 杩欐牱鍗充娇 init() 涓€斿け璐ワ紝涔熷彲浠ュ畨鍏ㄥ鐢ㄥ悓涓€濂楅噴鏀鹃€昏緫銆�
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
