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

    // 閿熸枻鎷烽敓鏂ゆ嫹璇撮敓鏂ゆ嫹閿熸枻鎷烽敓鏂ゆ嫹閿熸枻鎷峰け閿熸澃鐚存嫹鎵ч敓鏂ゆ嫹閿熸枻鎷烽敓鎴枻鎷烽敓鏂ゆ嫹閿熸枻鎷烽敓鏂ゆ嫹閿熸枻鎷烽敓鍙鎷� RTSP 閿熸枻鎷烽敓鏂ゆ嫹閿熺绋嬧槄鎷�
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

    // 鏉╂瑩鍣烽幐澶嗏偓婊冨礋娑擄拷 NALU閳ユ繂褰傞柅浣虹舶 rtsp_server閵嗭拷
    // 閸樼喎娲滈弰锟� MPP 鏉堟挸鍤柅姘埗閺勶拷 Annex-B閿涘奔绔存稉顏嗙处閸愭煡鍣烽崣顖濆厴鐢箑顦挎稉锟� NALU閵嗭拷
    // 婵″倹鐏夐弫鏉戞健閻╁瓨甯撮柅浣风瑓閸樹紮绱濇惔鏇炵湴閸欘垵鍏橀幐澶婂礋娑擄拷 NALU 婢跺嫮鎮婇敍灞筋嚤閼凤拷 RTP 閹垫挸瀵樻潏鍦櫕闁挎瑨顕ら妴锟�
    // 閸ョ姵顒濋崗鍫熷閸栧拑绱濋崘宥夆偓鎰嚋鐠嬪啰鏁� sessionSendVideoData()閿涘苯鍚嬬€硅鈧勬纯缁嬬偨鈧拷
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
        // 妤傛﹢顣堕弮銉ョ箶娴兼岸妯嗘繅鐐茬杽閺冨墎鍤庣粙瀣剁礉姒涙ǹ顓绘禒鍛ⅵ閸楁澘鍙ч柨顔兼姎/SPS/PPS閸滃苯鎳嗛張鐔糕偓褔鍣伴弽閿嬫）韫囨ぜ鈧拷
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
            // 閸欘亙绻氶悾娆忓彠闁款喛鐦栭弬顓熸）韫囨绱濋柆鍨帳 I/O 閹躲垹宕扮紓鏍垳/閸欐垿鈧焦妞傞梻瀵稿閵嗭拷
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

    // 鐎电懓顦婚崗浣筋啅閸欘亙绱堕垾婊堝劥閸掑棝鍘ょ純顔光偓婵撶礉鏉╂瑩鍣烽幎濠冩弓婵夘偄鍟撶€涙顔岀悰銉﹀灇姒涙ǹ顓婚崐绗衡偓锟�
    // 鏉╂瑦鐗辨稉濠傜湴娴肩姳绔存稉顏堟祩閸掓繂顫愰崠鏍波閺嬪嫪缍嬮敍灞肩瘍閼宠棄鍘涚捄鎴︹偓姘付鐏忓繑甯瑰ù渚€鎽肩捄顖樷偓锟�
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

    // rtspStartServer() 閸愬懘鍎存导姘舵▎婵夌偛婀惄鎴濇儔/accept 瀵邦亞骞嗛柌宀嬬礉
    // 閹碘偓娴犮儲鏂侀崷銊у殠缁嬪鍣烽敍宀勪缉閸忓秴宕辨担蹇庡瘜缁捐法鈻奸惃鍕櫚闂嗗棗鎷扮紓鏍垳閵嗭拷
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

    // 缁楊兛绔村銉窗閸忓牆鍨垫慨瀣闁插洭娉︾粩顖樷偓锟�
    // 閸氬海鐢荤紓鏍垳閸滃本甯瑰ù渚€鍏樻笟婵婄闁插洭娉﹂崚鎵畱 NV12 閸樼喎顫愮敮褝绱濋幍鈧禒銉╁櫚闂嗗棗绻€妞よ鍘涢崣顖滄暏閵嗭拷
    if (v4l2_capture_init(&ctx->capture) < 0) {
        fprintf(stderr, "[ERROR] v4l2_capture_init failed\n");
        goto fail;
    }
    ctx->capture_ready = 1;

    {
        // 缁楊兛绨╁銉窗閸掓繂顫愰崠锟� MPP 缂傛牜鐖滈崳銊ｂ偓锟�
        // 鏉╂瑩鍣烽崶鍝勭暰娴ｈ法鏁ゆ稉搴ㄥ櫚闂嗗棔绔撮懛瀵告畱閸掑棜椴搁悳鍥风礉闁灝鍘ょ亸鍝勵嚟娑撳秳绔撮懛瀛樻閸戣櫣骞� stride 闁挎瑨顕ら幋鏍翻閸忋儰绗夐崠褰掑帳閵嗭拷
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

    // 缁楊兛绗佸銉窗閸掓繂顫愰崠锟� RTSP 濡€虫健閵嗭拷
    // 閺€鎯ф躬闁插洭娉﹂崪宀€绱惍浣风閸氬函绱濋柆鍨帳閸撳秹娼版径杈Е閺冨墎鏆€娑撳绔存稉顏冪瑝閸欘垳鏁ら惃锟� RTSP 閺堝秴濮熼悩鑸碘偓浣碘偓锟�
    if (rtspModuleInit() < 0) {
        fprintf(stderr, "[ERROR] rtspModuleInit failed\n");
        goto fail;
    }
    ctx->rtsp_module_ready = 1;

    // 閸掓稑缂撻懛顏勭暰娑旓拷 session閿涘苯顓归幋椋庮伂闁俺绻� rtsp://ip:port/session_name 閹峰绁﹂妴锟�
    ctx->rtsp_session = rtspAddSession(ctx->config.session_name);
    if (!ctx->rtsp_session) {
        fprintf(stderr, "[ERROR] rtspAddSession failed\n");
        goto fail;
    }

    // 瑜版挸澧犻柧鎹愮熅閸欘亝甯� H264 鐟欏棝顣堕敍灞惧娴犮儴绻栭柌灞藉涧濞夈劌鍞界憴鍡涱暥婵帊缍嬬猾璇茬€烽妴锟�
    if (sessionAddVideo(ctx->rtsp_session, VIDEO_H264) < 0) {
        fprintf(stderr, "[ERROR] sessionAddVideo failed\n");
        goto fail;
    }

    if (ctx->config.record_file_path && ctx->config.record_file_path[0] != '\0') {
        // 閿熸枻鎷烽敓鏂ゆ嫹璇撮敓鏂ゆ嫹閿熸枻鎷烽敓鏂ゆ嫹閿熸枻鎷峰綍閿熸枻鎷烽敓鏂ゆ嫹甯介敓鏂ゆ嫹閿熸枻鎷� Annex-B 閿熸枻鎷烽敓鏂ゆ嫹鐩村啓閿熸枻鎷烽敓鏂ゆ嫹閿熻В寮€閿熸枻鎷峰皬閿熸枻鎷�
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
    // 缂佺喍绔寸挧棰佺娑擃亜銇戠拹銉︾閻炲棗鍤崣锝忕礉闁灝鍘ゅВ蹇庨嚋婢惰精瑙﹂崚鍡樻暜闁姤澧滈崘娆撳櫞閺€楣冣偓鏄忕帆閵嗭拷
    // 鏉╂瑦鐗遍崥搴ｇ敾缂佈呯敾閹碘晛鐫嶉崝鐔诲厴閺冭绱濇稉宥咁啇閺勬挻绱＄挧鍕爱閵嗭拷
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

    // RTSP 閺堝秴濮熺痪璺ㄢ柤鐠愮喕鐭楅惄鎴濇儔鐎广垺鍩涚粩顖濈箾閹恒儯鈧拷
    // 娑撹崵鍤庣粙瀣涧鐠愮喕鐭楅垾婊堝櫚闂嗭拷 -> 缂傛牜鐖� -> 闁焦绁﹂垾婵撶礉閼卞矁鐭楅弴瀛樼閺呭府绱濇稊鐔间缉閸忓秳绨伴惄鎼佹▎婵夌偑鈧拷
    ret = pthread_create(&server_tid, NULL, rtsp_server_thread, &thread_args);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] pthread_create rtsp server failed: %d\n", ret);
        return -1;
    }

    // 娑撳鎽肩捄顖ょ窗
    // 1. 娴狅拷 V4L2 閹舵挸褰� NV12 閸樼喎顫愮敮锟�
    // 2. 闁礁鍙� MPP 缂傛牜鐖滈幋锟� H264 Annex-B
    // 3. 閹峰棙鍨� NALU 閸氬孩甯圭紒锟� RTSP session
    while (ctx->running) {
        uint8_t *h264_data = NULL;
        size_t h264_len = 0;
        uint64_t dqbuf_ts_us = 0;
        uint64_t driver_to_dqbuf_us = 0;
        uint64_t encode_put_ts_us = 0;
        uint64_t encode_get_ts_us = 0;
        uint64_t send_video_before_ts_us = 0;

        // 鏉╂瑩鍣锋笟婵婄 v4l2_capture_frame() 閻ㄥ嫮鏁撻崨钘夋噯閺堢喍鎱ㄦ径宥忕窗
        // raw_frame 閻滄澘婀弰顖氼槻閸掕泛鍩岄悽銊﹀煕閹拷 frame_cache 閻ㄥ嫮菙鐎规碍鏆熼幑顕嗙礉
        // 娑撳秵妲告す鍗炲З闁絽娼℃导姘愁潶瀵邦亞骞嗘径宥囨暏閻拷 mmap 閸樼喎顫愮紓鎾冲暱閵嗭拷
        // 閹碘偓娴犮儱宓嗘担鍨俺鐏炲倻绱﹂崘鎻掑嚒缂佸繘鍣搁弬锟� QBUF 閸ョ偤鈹嶉崝顭掔礉
        // 瑜版挸澧犳潻娆庡敜 raw_frame 閸︺劍婀版潪顔剧椽閻胶绮ㄩ弶鐔峰娴犲秶鍔ч崣顖氱暔閸忋劏顕伴崣鏍电礉娑撳秳绱扮悮顐ｆ煀鐢嗩洬閻╂牓鈧拷
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
            // 閿熸枻鎷烽敓鏂ゆ嫹璇撮敓鏂ゆ嫹閿熸枻鎷烽敓缂寸》鎷峰け閿熸澃鐚存嫹閿熸枻鎷烽敓鏂ゆ嫹閿熸枻鎷峰崯閿熸枻鎷烽敓鏂ゆ嫹瑁曢敓鏂ゆ嫹閿熸枻鎷烽敓鏂ゆ嫹鏂愶綇鎷锋寚閿熸枻鎷烽敓鏂ゆ嫹閿熸枻鎷烽敓锟�
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
            // 缂傛牜鐖滈崳銊ュ灠閸氼垰濮╅弮璺哄讲閼虫垝绱伴張澶岀叚閺嗗倻绱︾€涙﹢妯佸▓纰夌礉閺嗗倹妞傚▽鈩冩箒鏉堟挸鍤稉宥囩暬绾剟鏁婄拠顖樷偓锟�
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

    // 鏉╂瑩鍣烽崣顏呮暭鏉╂劘顢戦弽鍥х箶閿涘奔绗夐惄瀛樺复闁插﹥鏂佺挧鍕爱閵嗭拷
    // 鐠у嫭绨紒鐔剁閸︼拷 deinit() 娑擃厼顦╅悶鍡礉闁灝鍘ゆ径姘槱楠炶泛褰傚〒鍛倞閵嗭拷
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

    // 閹稿鈧粏鐨濋幋鎰閸掓繂顫愰崠鏍箖閿涘矁鐨濈亸閬嶅櫞閺€閿偓婵堟畱閸樼喎鍨〒鍛倞閵嗭拷
    // 鏉╂瑦鐗遍崡鍏呭▏ init() 娑擃參鈧柨銇戠拹銉礉娑旂喎褰叉禒銉ョ暔閸忋劌顦查悽銊ユ倱娑撯偓婵傛鍣撮弨楣冣偓鏄忕帆閵嗭拷
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
