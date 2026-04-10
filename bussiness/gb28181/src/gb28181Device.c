#include "gb28181Device.h"

#include <arpa/inet.h>
#include <eXosip2/eXosip.h>
#include <netinet/in.h>
#include <osipparser2/osip_message.h>
#include <osipparser2/osip_parser.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define GB28181_DEFAULT_SERVER_IP "192.168.1.1"
#define GB28181_DEFAULT_SERVER_PORT 5060
#define GB28181_DEFAULT_SERVER_DOMAIN "3402000000"
#define GB28181_DEFAULT_SERVER_ID "34020000002000000001"
#define GB28181_DEFAULT_DEVICE_ID "34020000001320000001"
#define GB28181_DEFAULT_DEVICE_PASSWORD "12345678"
#define GB28181_DEFAULT_BIND_IP "0.0.0.0"
#define GB28181_DEFAULT_CONTACT_IP "127.0.0.1"
#define GB28181_DEFAULT_MEDIA_IP "127.0.0.1"
#define GB28181_DEFAULT_MEDIA_PORT 30000
#define GB28181_DEFAULT_REGISTER_EXPIRES 3600
#define GB28181_DEFAULT_KEEPALIVE_INTERVAL 60
#define GB28181_DEFAULT_REGISTER_RETRY_INTERVAL 5
#define GB28181_DEFAULT_DEVICE_NAME "RK3568 Camera"
#define GB28181_DEFAULT_MANUFACTURER "Topeet"
#define GB28181_DEFAULT_MODEL "RKMediaGateway"
#define GB28181_DEFAULT_FIRMWARE "1.0.0"
#define GB28181_DEFAULT_USER_AGENT "RKMediaGateway-GB28181/1.0"
#define GB28181_DEFAULT_FPS 25
#define GB28181_DEFAULT_BITRATE (2 * 1024 * 1024)
#define GB28181_DEFAULT_GOP 25
#define GB28181_DEFAULT_H264_PROFILE 100
#define GB28181_DEFAULT_H264_LEVEL 40
#define GB28181_DEFAULT_H264_CABAC_EN 1
#define GB28181_RTP_PAYLOAD_TYPE 96
#define GB28181_RTP_MAX_PAYLOAD 1400
#define GB28181_PS_STREAM_ID_VIDEO 0xE0
#define GB28181_PS_BUFFER_INIT_SIZE (2 * 1024 * 1024)

/*
 * 本文件实现 GB28181 设备端核心能力，包含两条主线：
 * 1. SIP 信令：REGISTER、鉴权、Keepalive、Catalog、DeviceInfo、INVITE/BYE；
 * 2. 媒体发送：H264(Annex-B) -> PS 封装 -> RTP 分包发送。
 *
 * 运行模式：
 * - 内部媒体模式：本模块自行初始化 V4L2 + MPP，并由 media_thread 抓帧编码后发送；
 * - 外部媒体模式：由外部模块调用 gb28181_device_send_h264() 注入编码帧。
 */

/* 动态缓存：用于组装 PS 帧，避免频繁临时 malloc/free。 */
typedef struct
{
    uint8_t *data;
    size_t size;
    size_t capacity;
} Gb28181Buffer;

/* NALU 描述：记录 Annex-B 一帧中每个 NALU 的偏移与长度。 */
typedef struct
{
    size_t offset;
    size_t length;
    uint8_t type;
} Gb28181NaluUnit;

static const char *h264_nalu_type_name(uint8_t type)
{
    switch (type)
    {
    case 1:
        return "NON_IDR";
    case 5:
        return "IDR";
    case 6:
        return "SEI";
    case 7:
        return "SPS";
    case 8:
        return "PPS";
    case 9:
        return "AUD";
    default:
        return "OTHER";
    }
}

static void log_h264_nalu_summary(const Gb28181NaluUnit *nalus, size_t nalu_count, int is_key_frame, uint64_t pts_us, uint64_t pts_90k)
{
    char type_log[512];
    size_t pos = 0;
    size_t i = 0;
    if (!nalus)
        return;
    type_log[0] = '\0';
    for (i = 0; i < nalu_count; ++i)
    {
        int written = 0;
        if (i > 0 && pos < sizeof(type_log) - 1)
            type_log[pos++] = ',';
        written = snprintf(type_log + pos,
                           sizeof(type_log) - pos,
                           "%u(%s)",
                           (unsigned int)nalus[i].type,
                           h264_nalu_type_name(nalus[i].type));
        if (written < 0)
            break;
        if ((size_t)written >= sizeof(type_log) - pos)
        {
            pos = sizeof(type_log) - 1;
            break;
        }
        pos += (size_t)written;
    }
    type_log[sizeof(type_log) - 1] = '\0';
    printf("[GB28181][H264] pts_us=%llu pts_90k=%llu key=%d nalu_count=%zu types=%s\n",
           (unsigned long long)pts_us,
           (unsigned long long)pts_90k,
           is_key_frame ? 1 : 0,
           nalu_count,
           (type_log[0] != '\0') ? type_log : "N/A");
}

/* 获取当前毫秒时间戳（单调递增相对时间）。 */
static long long get_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

/* 周期性 re-REGISTER：在过期前预留 5 秒；最小不低于失败重试周期。 */
static long long get_register_refresh_interval_ms(const Gb28181DeviceCtx *ctx)
{
    int refresh_sec = 1;
    if (!ctx)
        return 1000LL;
    refresh_sec = ctx->config.register_expires - 5;
    if (refresh_sec < ctx->config.register_retry_interval_sec)
        refresh_sec = ctx->config.register_retry_interval_sec;
    if (refresh_sec <= 0)
        refresh_sec = 1;
    return (long long)refresh_sec * 1000LL;
}

/* 字符串兜底：当 value 为空时返回 fallback。 */
static const char *safe_str(const char *value, const char *fallback)
{
    return (value && value[0] != '\0') ? value : fallback;
}

/* 重置媒体会话到“无会话”状态。 */
static void reset_media_session(Gb28181MediaSession *session)
{
    if (!session)
        return;
    memset(session, 0, sizeof(*session));
    session->rtp_socket_fd = -1;
}

/* 将用户输入配置补齐为可运行的完整配置。 */
static void fill_default_config(Gb28181DeviceConfig *dst, const Gb28181DeviceConfig *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src)
        *dst = *src;
    dst->server_ip = safe_str(dst->server_ip, GB28181_DEFAULT_SERVER_IP);
    if (dst->server_port <= 0)
        dst->server_port = GB28181_DEFAULT_SERVER_PORT;
    dst->server_domain = safe_str(dst->server_domain, GB28181_DEFAULT_SERVER_DOMAIN);
    dst->server_id = safe_str(dst->server_id, GB28181_DEFAULT_SERVER_ID);
    dst->device_id = safe_str(dst->device_id, GB28181_DEFAULT_DEVICE_ID);
    dst->device_domain = safe_str(dst->device_domain, dst->server_domain);
    dst->device_password = safe_str(dst->device_password, GB28181_DEFAULT_DEVICE_PASSWORD);
    dst->bind_ip = safe_str(dst->bind_ip, GB28181_DEFAULT_BIND_IP);
    if (dst->local_sip_port <= 0)
        dst->local_sip_port = GB28181_DEFAULT_SERVER_PORT;
    dst->sip_contact_ip = safe_str(dst->sip_contact_ip, GB28181_DEFAULT_CONTACT_IP);
    dst->media_ip = safe_str(dst->media_ip, dst->sip_contact_ip);
    if (dst->media_port <= 0)
        dst->media_port = GB28181_DEFAULT_MEDIA_PORT;
    if (dst->register_expires <= 0)
        dst->register_expires = GB28181_DEFAULT_REGISTER_EXPIRES;
    if (dst->keepalive_interval_sec <= 0)
        dst->keepalive_interval_sec = GB28181_DEFAULT_KEEPALIVE_INTERVAL;
    if (dst->register_retry_interval_sec <= 0)
        dst->register_retry_interval_sec = GB28181_DEFAULT_REGISTER_RETRY_INTERVAL;
    dst->device_name = safe_str(dst->device_name, GB28181_DEFAULT_DEVICE_NAME);
    dst->manufacturer = safe_str(dst->manufacturer, GB28181_DEFAULT_MANUFACTURER);
    dst->model = safe_str(dst->model, GB28181_DEFAULT_MODEL);
    dst->firmware = safe_str(dst->firmware, GB28181_DEFAULT_FIRMWARE);
    dst->channel_id = safe_str(dst->channel_id, dst->device_id);
    dst->user_agent = safe_str(dst->user_agent, GB28181_DEFAULT_USER_AGENT);
    if (dst->fps <= 0)
        dst->fps = GB28181_DEFAULT_FPS;
    if (dst->bitrate <= 0)
        dst->bitrate = GB28181_DEFAULT_BITRATE;
    if (dst->gop <= 0)
        dst->gop = GB28181_DEFAULT_GOP;
    if (dst->h264_profile <= 0)
        dst->h264_profile = GB28181_DEFAULT_H264_PROFILE;
    if (dst->h264_level <= 0)
        dst->h264_level = GB28181_DEFAULT_H264_LEVEL;
    if (dst->h264_cabac_en < 0)
        dst->h264_cabac_en = GB28181_DEFAULT_H264_CABAC_EN;
}

/* 构建 REGISTER 所需的 from/proxy/contact 三个 URI。 */
static void build_register_identity(const Gb28181DeviceCtx *ctx, char *from_uri, size_t from_size, char *proxy_uri, size_t proxy_size, char *contact_uri, size_t contact_size)
{
    snprintf(from_uri, from_size, "sip:%s@%s", ctx->config.device_id, ctx->config.device_domain);
    snprintf(proxy_uri, proxy_size, "sip:%s@%s:%d", ctx->config.server_id, ctx->config.server_ip, ctx->config.server_port);
    snprintf(contact_uri, contact_size, "sip:%s@%s:%d", ctx->config.device_id, ctx->config.sip_contact_ip, ctx->config.local_sip_port);
}

/* 构建服务器 SIP URI（MESSAGE 请求目标）。 */
static void build_server_target_uri(const Gb28181DeviceCtx *ctx, char *server_uri, size_t server_uri_size)
{
    snprintf(server_uri, server_uri_size, "sip:%s@%s:%d", ctx->config.server_id, ctx->config.server_ip, ctx->config.server_port);
}

/* 从 XML 文本中提取 <tag>value</tag>。 */
static int extract_tag_value(const char *text, const char *tag, char *value, size_t value_size)
{
    char open_tag[64];
    char close_tag[64];
    const char *start = NULL;
    const char *end = NULL;
    size_t len = 0;
    if (!text || !tag || !value || value_size == 0)
        return -1;
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    start = strstr(text, open_tag);
    if (!start)
        return -1;
    start += strlen(open_tag);
    end = strstr(start, close_tag);
    if (!end || end <= start)
        return -1;
    len = (size_t)(end - start);
    if (len >= value_size)
        len = value_size - 1;
    memcpy(value, start, len);
    value[len] = '\0';
    return 0;
}

/* 从文本中提取“prefix 后面的整行”。 */
static int extract_line_after_prefix(const char *text, const char *prefix, char *value, size_t value_size)
{
    const char *start = NULL;
    const char *end = NULL;
    size_t len = 0;
    if (!text || !prefix || !value || value_size == 0)
        return -1;
    start = strstr(text, prefix);
    if (!start)
        return -1;
    start += strlen(prefix);
    end = strstr(start, "\n");
    if (!end)
        end = start + strlen(start);
    while (end > start && (end[-1] == '\r' || end[-1] == '\n'))
        end--;
    len = (size_t)(end - start);
    if (len >= value_size)
        len = value_size - 1;
    memcpy(value, start, len);
    value[len] = '\0';
    return 0;
}

/* 解析 INVITE SDP，提取对端媒体地址、端口、SSRC。 */
static int parse_invite_sdp(const char *sdp_body, Gb28181MediaSession *session)
{
    char media_line[128];
    if (!sdp_body || !session)
        return -1;
    memset(session->remote_ip, 0, sizeof(session->remote_ip));
    memset(session->remote_ssrc, 0, sizeof(session->remote_ssrc));
    memset(session->transport, 0, sizeof(session->transport));
    session->remote_port = 0;
    extract_line_after_prefix(sdp_body, "c=IN IP4 ", session->remote_ip, sizeof(session->remote_ip));
    if (extract_line_after_prefix(sdp_body, "m=video ", media_line, sizeof(media_line)) == 0)
    {
        if (sscanf(media_line, "%d %31s", &session->remote_port, session->transport) < 2)
            return -1;
    }
    extract_line_after_prefix(sdp_body, "y=", session->remote_ssrc, sizeof(session->remote_ssrc));
    return (session->remote_port > 0) ? 0 : -1;
}

/* 生成本端会话 SSRC（字符串与数值同时保存）。 */
static void generate_local_ssrc(Gb28181MediaSession *session)
{
    unsigned int value = (unsigned int)((time(NULL) ^ getpid()) & 0x3FFFFFFF);
    if (value == 0)
        value = 1;
    session->rtp_ssrc = value;
    snprintf(session->local_ssrc, sizeof(session->local_ssrc), "%010u", value);
}

/* 优先使用 INVITE SDP 的 y= 作为发送 SSRC，失败时回退随机 SSRC。 */
static int use_invite_ssrc_if_valid(Gb28181MediaSession *session)
{
    const char *p = NULL;
    char *end = NULL;
    unsigned long long value = 0;
    if (!session)
        return -1;
    if (session->remote_ssrc[0] == '\0')
        return -1;

    p = session->remote_ssrc;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return -1;

    value = strtoull(p, &end, 10);
    if (end == p)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    if (*end != '\0' || value == 0)
        return -1;

    /* RTP SSRC 头字段是 32bit，这里按低 32bit 写入。 */
    session->rtp_ssrc = (unsigned int)(value & 0xFFFFFFFFULL);
    snprintf(session->local_ssrc, sizeof(session->local_ssrc), "%s", session->remote_ssrc);
    return 0;
}

/* 初始化可增长缓存。 */
static int gb_buffer_init(Gb28181Buffer *buffer, size_t initial_capacity)
{
    if (!buffer)
        return -1;
    memset(buffer, 0, sizeof(*buffer));
    buffer->data = (uint8_t *)malloc(initial_capacity);
    if (!buffer->data)
        return -1;
    buffer->capacity = initial_capacity;
    return 0;
}

/* 释放可增长缓存。 */
static void gb_buffer_deinit(Gb28181Buffer *buffer)
{
    if (!buffer)
        return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

/* 确保缓存至少还能追加 append_size 字节。 */
static int gb_buffer_reserve(Gb28181Buffer *buffer, size_t append_size)
{
    size_t need_size = 0;
    size_t new_capacity = 0;
    uint8_t *new_data = NULL;
    if (!buffer)
        return -1;
    need_size = buffer->size + append_size;
    if (need_size <= buffer->capacity)
        return 0;
    new_capacity = buffer->capacity;
    while (new_capacity < need_size)
        new_capacity *= 2;
    new_data = (uint8_t *)realloc(buffer->data, new_capacity);
    if (!new_data)
        return -1;
    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return 0;
}

/* 向缓存尾部追加数据。 */
static int gb_buffer_append(Gb28181Buffer *buffer, const void *data, size_t len)
{
    if (!buffer || !data)
        return -1;
    if (gb_buffer_reserve(buffer, len) != 0)
        return -1;
    memcpy(buffer->data + buffer->size, data, len);
    buffer->size += len;
    return 0;
}

/* 清空缓存长度（保留容量）。 */
static void gb_buffer_reset(Gb28181Buffer *buffer)
{
    if (buffer)
        buffer->size = 0;
}

/* 判断当前位置是否是 H264 起始码并返回长度（3/4 字节）。 */
static int h264_start_code_len(const uint8_t *data, size_t len)
{
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
        return 4;
    if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return 3;
    return 0;
}

/* 解析 Annex-B 一帧，输出 NALU 列表。 */
static int parse_annexb_nalus(const uint8_t *annexb_data, size_t annexb_len, Gb28181NaluUnit *nalus, size_t max_nalus, size_t *nalu_count)
{
    size_t pos = 0;
    size_t count = 0;
    if (!annexb_data || !nalus || !nalu_count)
        return -1;
    *nalu_count = 0;
    while (pos + 3 < annexb_len)
    {
        int code_len = h264_start_code_len(annexb_data + pos, annexb_len - pos);
        size_t payload_start = 0;
        size_t next = 0;
        if (code_len == 0)
        {
            pos++;
            continue;
        }
        payload_start = pos + (size_t)code_len;
        next = payload_start;
        while (next + 3 < annexb_len)
        {
            if (h264_start_code_len(annexb_data + next, annexb_len - next) > 0)
                break;
            next++;
        }
        if (next + 3 >= annexb_len)
            next = annexb_len;
        if (payload_start < next && count < max_nalus)
        {
            nalus[count].offset = payload_start;
            nalus[count].length = next - payload_start;
            nalus[count].type = annexb_data[payload_start] & 0x1F;
            count++;
        }
        pos = next;
    }
    *nalu_count = count;
    return (count > 0) ? 0 : -1;
}

/* 写入 PS pack header。 */
static int ps_write_pack_header(Gb28181Buffer *buffer, uint64_t scr_90k)
{
    uint8_t pack[14];
    uint64_t scr = scr_90k & 0x1FFFFFFFFULL;
    pack[0] = 0x00;
    pack[1] = 0x00;
    pack[2] = 0x01;
    pack[3] = 0xBA;
    pack[4] = (uint8_t)(0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 0x03));
    pack[5] = (uint8_t)(scr >> 20);
    pack[6] = (uint8_t)(((scr >> 12) & 0xF8) | 0x04 | ((scr >> 13) & 0x03));
    pack[7] = (uint8_t)(scr >> 5);
    pack[8] = (uint8_t)(((scr << 3) & 0xF8) | 0x04);
    pack[9] = 0x01;
    pack[10] = 0x89;
    pack[11] = 0xC3;
    pack[12] = 0xF8;
    pack[13] = 0x00;
    return gb_buffer_append(buffer, pack, sizeof(pack));
}

/* 写入 PS system header（关键帧前附带）。 */
static int ps_write_system_header(Gb28181Buffer *buffer)
{
    const uint8_t system_header[] = {0x00, 0x00, 0x01, 0xBB, 0x00, 0x0C, 0x80, 0x04, 0x04, 0xE1, 0x7F, 0xE0, 0xE0, 0xE8, 0xC0, 0x20, 0xBD, 0xE0};
    return gb_buffer_append(buffer, system_header, sizeof(system_header));
}

/* 写入 PS program stream map（关键帧前附带）。 */
static int ps_write_program_stream_map(Gb28181Buffer *buffer)
{
    const uint8_t psm[] = {0x00, 0x00, 0x01, 0xBC, 0x00, 0x12, 0xE0, 0xFF, 0x00, 0x00, 0x00, 0x08, 0x1B, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0xBD, 0xDC, 0xF4};
    return gb_buffer_append(buffer, psm, sizeof(psm));
}

/* 写入 PTS 字段（5 字节格式）。 */
static void ps_write_pts_field(uint8_t *dst, uint8_t prefix, uint64_t pts)
{
    uint64_t value = pts & 0x1FFFFFFFFULL;
    dst[0] = (uint8_t)((prefix << 4) | (((value >> 30) & 0x07) << 1) | 0x01);
    dst[1] = (uint8_t)(value >> 22);
    dst[2] = (uint8_t)((((value >> 15) & 0x7F) << 1) | 0x01);
    dst[3] = (uint8_t)(value >> 7);
    dst[4] = (uint8_t)(((value & 0x7F) << 1) | 0x01);
}

/* 写入视频 PES 包头与 payload。 */
static int ps_write_video_pes(Gb28181Buffer *buffer, const uint8_t *payload, size_t payload_len, uint64_t pts_90k)
{
    uint8_t header[14];
    size_t pes_packet_length = payload_len + 8;
    if (!buffer || !payload || payload_len == 0)
        return -1;
    if (pes_packet_length > 0xFFFF)
        pes_packet_length = 0;
    memset(header, 0, sizeof(header));
    header[0] = 0x00;
    header[1] = 0x00;
    header[2] = 0x01;
    header[3] = GB28181_PS_STREAM_ID_VIDEO;
    header[4] = (uint8_t)((pes_packet_length >> 8) & 0xFF);
    header[5] = (uint8_t)(pes_packet_length & 0xFF);
    header[6] = 0x80;
    header[7] = 0x80;
    header[8] = 0x05;
    ps_write_pts_field(header + 9, 0x02, pts_90k);
    if (gb_buffer_append(buffer, header, sizeof(header)) != 0)
        return -1;
    return gb_buffer_append(buffer, payload, payload_len);
}

/*
 * 将一帧 Annex-B H264 封装为 PS。
 * 关键帧时会附带 system header + PSM，提升下游识别成功率。
 */
static int build_ps_frame(const uint8_t *annexb_data, size_t annexb_len, int is_key_frame, uint64_t pts_us, Gb28181Buffer *ps_buffer)
{
    Gb28181NaluUnit nalus[64];
    size_t nalu_count = 0;
    size_t i = 0;
    uint64_t pts_90k = pts_us * 90ULL / 1000ULL;
    if (!annexb_data || annexb_len == 0 || !ps_buffer)
        return -1;
    gb_buffer_reset(ps_buffer);
    /*
     * 把一帧 Annex-B 拆成 NALU 后再封装成 PS。
     * 关键帧前附带 system header + PSM，便于 WVP/下游更快识别流类型。
     * 每个 NALU 独立作为一个 PES，逻辑简单，也能规避大帧导致的 PES 长度上限问题。
     */
    if (parse_annexb_nalus(annexb_data, annexb_len, nalus, 64, &nalu_count) != 0)
        return -1;
    log_h264_nalu_summary(nalus, nalu_count, is_key_frame, pts_us, pts_90k);
    if (ps_write_pack_header(ps_buffer, pts_90k) != 0)
        return -1;
    if (is_key_frame)
    {
        if (ps_write_system_header(ps_buffer) != 0)
            return -1;
        if (ps_write_program_stream_map(ps_buffer) != 0)
            return -1;
    }
    for (i = 0; i < nalu_count; ++i)
    {
        const uint8_t *payload = annexb_data + nalus[i].offset;
        size_t payload_len = nalus[i].length;
        if (nalus[i].type == 9)
            continue;
        if (ps_write_video_pes(ps_buffer, payload, payload_len, pts_90k) != 0)
            return -1;
    }
    return (ps_buffer->size > 0) ? 0 : -1;
}

/*
 * 发送 PS over RTP：
 * - 按固定 MTU 大小分片；
 * - 最后一片 marker=1；
 * - 每片发送后 sequence++。
 */
static int send_ps_over_rtp(Gb28181MediaSession *session, const uint8_t *ps_data, size_t ps_len, uint32_t rtp_timestamp)
{
    struct sockaddr_in remote_addr;
    size_t offset = 0;
    if (!session || session->rtp_socket_fd < 0 || !ps_data || ps_len == 0)
        return -1;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons((uint16_t)session->remote_port);
    if (inet_aton(session->remote_ip, &remote_addr.sin_addr) == 0)
        return -1;
    /*
     * GB28181 这里走最常见的 PS over RTP。
     * 一帧 PS 会被拆成多个 RTP 包，最后一个包带 marker=1。
     */
    while (offset < ps_len)
    {
        uint8_t packet[12 + GB28181_RTP_MAX_PAYLOAD];
        size_t chunk = ps_len - offset;
        unsigned short seq = session->rtp_sequence;
        int marker = 0;
        ssize_t sent = 0;
        if (chunk > GB28181_RTP_MAX_PAYLOAD)
            chunk = GB28181_RTP_MAX_PAYLOAD;
        marker = ((offset + chunk) >= ps_len) ? 1 : 0;
        packet[0] = 0x80;
        packet[1] = (uint8_t)((marker ? 0x80 : 0x00) | GB28181_RTP_PAYLOAD_TYPE);
        packet[2] = (uint8_t)((session->rtp_sequence >> 8) & 0xFF);
        packet[3] = (uint8_t)(session->rtp_sequence & 0xFF);
        packet[4] = (uint8_t)((rtp_timestamp >> 24) & 0xFF);
        packet[5] = (uint8_t)((rtp_timestamp >> 16) & 0xFF);
        packet[6] = (uint8_t)((rtp_timestamp >> 8) & 0xFF);
        packet[7] = (uint8_t)(rtp_timestamp & 0xFF);
        packet[8] = (uint8_t)((session->rtp_ssrc >> 24) & 0xFF);
        packet[9] = (uint8_t)((session->rtp_ssrc >> 16) & 0xFF);
        packet[10] = (uint8_t)((session->rtp_ssrc >> 8) & 0xFF);
        packet[11] = (uint8_t)(session->rtp_ssrc & 0xFF);
        memcpy(packet + 12, ps_data + offset, chunk);
        if (seq == 0)
        {
            printf("[GB28181][RTP] first_packet remote=%s:%d ps_len=%zu chunk=%zu seq=%u ts=%u ssrc=%u marker=%d\n",
                   session->remote_ip, session->remote_port, ps_len, chunk, seq, rtp_timestamp, session->rtp_ssrc, marker);
        }
        sent = sendto(session->rtp_socket_fd, packet, (int)(12 + chunk), 0, (const struct sockaddr *)&remote_addr, sizeof(remote_addr));
        if (sent < 0)
        {
            fprintf(stderr, "[GB28181][RTP] sendto failed remote=%s:%d seq=%u ts=%u errno=%d(%s)\n",
                    session->remote_ip, session->remote_port, seq, rtp_timestamp, errno, strerror(errno));
            return -1;
        }
        session->rtp_sequence++;
        offset += chunk;
    }
    session->last_rtp_timestamp = rtp_timestamp;
    return 0;
}

/* 创建并绑定本地 RTP UDP socket。 */
static int setup_rtp_socket(Gb28181MediaSession *session, const Gb28181DeviceConfig *config)
{
    struct sockaddr_in local_addr;
    int socket_fd = -1;
    int reuse_addr = 1;
    if (!session || !config)
        return -1;
    if (session->rtp_socket_fd >= 0)
        return 0;
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
        return -1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons((uint16_t)config->media_port);
    if (strcmp(config->bind_ip, "0.0.0.0") == 0)
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_aton(config->bind_ip, &local_addr.sin_addr) == 0)
    {
        close(socket_fd);
        return -1;
    }
    if (bind(socket_fd, (const struct sockaddr *)&local_addr, sizeof(local_addr)) != 0)
    {
        close(socket_fd);
        return -1;
    }
    session->rtp_socket_fd = socket_fd;
    return 0;
}

/* 关闭 RTP socket。 */
static void close_rtp_socket(Gb28181MediaSession *session)
{
    if (!session)
        return;
    if (session->rtp_socket_fd >= 0)
        close(session->rtp_socket_fd);
    session->rtp_socket_fd = -1;
}

/* 补齐 GB28181 XML 声明头。 */
static int build_xml_body(char *buffer, size_t buffer_size, const char *xml_body)
{
    int written = snprintf(buffer, buffer_size, "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n%s", xml_body);
    return (written < 0 || (size_t)written >= buffer_size) ? -1 : 0;
}

/* 发送 SIP MESSAGE（用于 Keepalive/Catalog/DeviceInfo 应答）。 */
static int send_message_request(Gb28181DeviceCtx *ctx, const char *content_type, const char *body)
{
    char from_uri[128];
    char server_uri[128];
    osip_message_t *message = NULL;
    if (!ctx || !ctx->sip_context || !content_type || !body)
        return -1;
    snprintf(from_uri, sizeof(from_uri), "sip:%s@%s", ctx->config.device_id, ctx->config.device_domain);
    build_server_target_uri(ctx, server_uri, sizeof(server_uri));
    eXosip_lock(ctx->sip_context);
    if (eXosip_message_build_request(ctx->sip_context, &message, "MESSAGE", server_uri, from_uri, NULL) != 0)
    {
        eXosip_unlock(ctx->sip_context);
        return -1;
    }
    osip_message_set_content_type(message, content_type);
    osip_message_set_body(message, body, strlen(body));
    if (eXosip_message_send_request(ctx->sip_context, message) != 0)
    {
        eXosip_unlock(ctx->sip_context);
        return -1;
    }
    eXosip_unlock(ctx->sip_context);
    return 0;
}

/* 发送 Keepalive。 */
static int send_keepalive(Gb28181DeviceCtx *ctx)
{
    char inner_xml[512];
    char xml_body[640];
    int written = snprintf(inner_xml, sizeof(inner_xml),
                           "<Notify>\r\n  <CmdType>Keepalive</CmdType>\r\n  <SN>%u</SN>\r\n  <DeviceID>%s</DeviceID>\r\n  <Status>OK</Status>\r\n</Notify>\r\n",
                           ++ctx->xml_sn, ctx->config.device_id);
    if (written < 0 || (size_t)written >= sizeof(inner_xml))
        return -1;
    if (build_xml_body(xml_body, sizeof(xml_body), inner_xml) != 0)
        return -1;
    return send_message_request(ctx, "Application/MANSCDP+xml", xml_body);
}

/* 发送 Catalog 应答。 */
static int send_catalog_response(Gb28181DeviceCtx *ctx, const char *sn)
{
    char inner_xml[2048];
    char xml_body[2304];
    int written = snprintf(inner_xml, sizeof(inner_xml),
                           "<Response>\r\n  <CmdType>Catalog</CmdType>\r\n  <SN>%s</SN>\r\n  <DeviceID>%s</DeviceID>\r\n  <SumNum>1</SumNum>\r\n  <DeviceList Num=\"1\">\r\n    <Item>\r\n      <DeviceID>%s</DeviceID>\r\n      <Name>%s</Name>\r\n      <Manufacturer>%s</Manufacturer>\r\n      <Model>%s</Model>\r\n      <Owner>RKMediaGateway</Owner>\r\n      <CivilCode>%s</CivilCode>\r\n      <Address>%s</Address>\r\n      <Parental>0</Parental>\r\n      <SafetyWay>0</SafetyWay>\r\n      <RegisterWay>1</RegisterWay>\r\n      <Secrecy>0</Secrecy>\r\n      <Status>ON</Status>\r\n    </Item>\r\n  </DeviceList>\r\n</Response>\r\n",
                           sn, ctx->config.device_id, ctx->config.channel_id, ctx->config.device_name, ctx->config.manufacturer, ctx->config.model, ctx->config.device_domain, ctx->config.server_ip);
    if (written < 0 || (size_t)written >= sizeof(inner_xml))
        return -1;
    if (build_xml_body(xml_body, sizeof(xml_body), inner_xml) != 0)
        return -1;
    return send_message_request(ctx, "Application/MANSCDP+xml", xml_body);
}

/* 发送 DeviceInfo 应答。 */
static int send_device_info_response(Gb28181DeviceCtx *ctx, const char *sn)
{
    char inner_xml[1024];
    char xml_body[1280];
    int written = snprintf(inner_xml, sizeof(inner_xml),
                           "<Response>\r\n  <CmdType>DeviceInfo</CmdType>\r\n  <SN>%s</SN>\r\n  <DeviceID>%s</DeviceID>\r\n  <DeviceName>%s</DeviceName>\r\n  <Manufacturer>%s</Manufacturer>\r\n  <Model>%s</Model>\r\n  <Firmware>%s</Firmware>\r\n  <Channel>1</Channel>\r\n</Response>\r\n",
                           sn, ctx->config.device_id, ctx->config.device_name, ctx->config.manufacturer, ctx->config.model, ctx->config.firmware);
    if (written < 0 || (size_t)written >= sizeof(inner_xml))
        return -1;
    if (build_xml_body(xml_body, sizeof(xml_body), inner_xml) != 0)
        return -1;
    return send_message_request(ctx, "Application/MANSCDP+xml", xml_body);
}

/* 发送 REGISTER / re-REGISTER。 */
static int send_register_request(Gb28181DeviceCtx *ctx, int expires)
{
    char from_uri[128];
    char proxy_uri[128];
    char contact_uri[128];
    osip_message_t *register_message = NULL;
    int rid = -1;
    if (!ctx || !ctx->sip_context)
        return -1;
    build_register_identity(ctx, from_uri, sizeof(from_uri), proxy_uri, sizeof(proxy_uri), contact_uri, sizeof(contact_uri));
    eXosip_lock(ctx->sip_context);
    if (ctx->rid <= 0)
    {
        rid = eXosip_register_build_initial_register(ctx->sip_context, from_uri, proxy_uri, contact_uri, expires, &register_message);
        if (rid <= 0 || !register_message)
        {
            eXosip_unlock(ctx->sip_context);
            return -1;
        }
        ctx->rid = rid;
    }
    else
    {
        if (eXosip_register_build_register(ctx->sip_context, ctx->rid, expires, &register_message) != 0 || !register_message)
        {
            eXosip_unlock(ctx->sip_context);
            return -1;
        }
    }
    if (eXosip_register_send_register(ctx->sip_context, ctx->rid, register_message) != 0)
    {
        eXosip_unlock(ctx->sip_context);
        return -1;
    }
    eXosip_unlock(ctx->sip_context);
    return 0;
}

/* 处理 401/407 鉴权挑战。 */
static void handle_auth_failure(Gb28181DeviceCtx *ctx, eXosip_event_t *event)
{
    int status_code = 0;
    if (!ctx || !event || !event->response)
        return;
    status_code = event->response->status_code;
    if (status_code == 401 || status_code == 407)
    {
        eXosip_lock(ctx->sip_context);
        eXosip_default_action(ctx->sip_context, event);
        eXosip_unlock(ctx->sip_context);
    }
}

/* 应答普通 MESSAGE/OPTIONS 请求。 */
static int answer_simple_request(Gb28181DeviceCtx *ctx, eXosip_event_t *event, int status_code)
{
    if (!ctx || !event)
        return -1;
    eXosip_lock(ctx->sip_context);
    if (eXosip_message_send_answer(ctx->sip_context, event->tid, status_code, NULL) != 0)
    {
        eXosip_unlock(ctx->sip_context);
        return -1;
    }
    eXosip_unlock(ctx->sip_context);
    return 0;
}

/* 应答 INVITE/BYE/INFO 等 call 内请求。 */
static int answer_call_request(Gb28181DeviceCtx *ctx, eXosip_event_t *event, int status_code)
{
    if (!ctx || !event)
        return -1;
    eXosip_lock(ctx->sip_context);
    if (eXosip_call_send_answer(ctx->sip_context, event->tid, status_code, NULL) != 0)
    {
        eXosip_unlock(ctx->sip_context);
        return -1;
    }
    eXosip_unlock(ctx->sip_context);
    return 0;
}

/* 处理 MESSAGE 查询（Catalog / DeviceInfo）。 */
static int handle_query_message(Gb28181DeviceCtx *ctx, eXosip_event_t *event)
{
    osip_body_t *body = NULL;
    char cmd_type[64];
    char sn[64];
    if (!ctx || !event || !event->request)
        return -1;
    memset(cmd_type, 0, sizeof(cmd_type));
    memset(sn, 0, sizeof(sn));
    if (osip_message_get_body(event->request, 0, &body) != 0 || !body || !body->body)
        return answer_simple_request(ctx, event, 200);
    if (extract_tag_value(body->body, "CmdType", cmd_type, sizeof(cmd_type)) != 0)
        return answer_simple_request(ctx, event, 200);
    extract_tag_value(body->body, "SN", sn, sizeof(sn));
    if (sn[0] == '\0')
        snprintf(sn, sizeof(sn), "%u", ++ctx->xml_sn);
    if (answer_simple_request(ctx, event, 200) != 0)
        return -1;
    if (strcmp(cmd_type, "Catalog") == 0)
        return send_catalog_response(ctx, sn);
    if (strcmp(cmd_type, "DeviceInfo") == 0)
        return send_device_info_response(ctx, sn);
    return 0;
}

/*
 * 本地媒体线程：
 * 仅在 internal mode（external_media_input=0）使用。
 * 主流程：采集 NV12 -> MPP 编码 H264 -> PS 封装 -> RTP 发送。
 */
static void *media_thread_main(void *arg)
{
    Gb28181DeviceCtx *ctx = (Gb28181DeviceCtx *)arg;
    Gb28181Buffer ps_buffer;
    if (!ctx || gb_buffer_init(&ps_buffer, GB28181_PS_BUFFER_INIT_SIZE) != 0)
        return NULL;
    while (ctx->running)
    {
        Gb28181MediaSession session_snapshot;
        /* 没有点播会话时线程休眠，避免空转占用 CPU。 */
        pthread_mutex_lock(&ctx->session_lock);
        while (ctx->running && (!ctx->media_session.active || !ctx->media_session.established))
            pthread_cond_wait(&ctx->session_cond, &ctx->session_lock);
        if (!ctx->running)
        {
            pthread_mutex_unlock(&ctx->session_lock);
            break;
        }
        session_snapshot = ctx->media_session;
        pthread_mutex_unlock(&ctx->session_lock);
        while (ctx->running)
        {
            uint8_t *raw_frame = NULL;
            int raw_len = 0;
            uint64_t frame_id = 0;
            uint64_t dqbuf_ts_us = 0;
            uint64_t driver_to_dqbuf_us = 0;
            uint8_t *h264_data = NULL;
            size_t h264_len = 0;
            int is_key_frame = 0;
            uint32_t rtp_timestamp = 0;
            int request_idr_now = 0;
            pthread_mutex_lock(&ctx->session_lock);
            if (!ctx->media_session.active || !ctx->media_session.established)
            {
                pthread_mutex_unlock(&ctx->session_lock);
                break;
            }
            session_snapshot = ctx->media_session;
            if (ctx->pending_force_idr)
            {
                request_idr_now = 1;
                ctx->pending_force_idr = 0;
            }
            pthread_mutex_unlock(&ctx->session_lock);
            if (request_idr_now)
            {
                if (mpp_encoder_request_idr(ctx->encoder) == 0)
                {
                    printf("[GB28181] request IDR after call established (internal mode)\n");
                }
                else
                {
                    fprintf(stderr, "[GB28181] request IDR failed, will retry next frame\n");
                    pthread_mutex_lock(&ctx->session_lock);
                    ctx->pending_force_idr = 1;
                    pthread_mutex_unlock(&ctx->session_lock);
                }
            }
            /*
             * 媒体主路径：
             * 1. 从摄像头抓一帧 NV12；
             * 2. 送给 MPP 输出 Annex-B H.264；
             * 3. 将该帧封装成 PS；
             * 4. 再切成 RTP 包发往平台。
             */
            if (v4l2_capture_frame(ctx->capture, &raw_frame, &raw_len, &frame_id, &dqbuf_ts_us, &driver_to_dqbuf_us) != 0)
            {
                usleep(10000);
                continue;
            }
            if (mpp_encoder_encode_frame(ctx->encoder, raw_frame, (size_t)raw_len, frame_id, &h264_data, &h264_len, &is_key_frame, NULL, NULL) != 0)
                continue;
            if (!h264_data || h264_len == 0)
                continue;
            if (build_ps_frame(h264_data, h264_len, is_key_frame, dqbuf_ts_us, &ps_buffer) != 0)
                continue;
            /* PTS 主要给解复用/解码链路用；RTP timestamp 主要给网络抖动缓冲和同步排序用。 */
            rtp_timestamp = (uint32_t)((dqbuf_ts_us * 90ULL / 1000ULL) & 0xFFFFFFFFU);
            if (send_ps_over_rtp(&session_snapshot, ps_buffer.data, ps_buffer.size, rtp_timestamp) != 0)
            {
                usleep(10000);
                continue;
            }
            pthread_mutex_lock(&ctx->session_lock);
            if (ctx->media_session.active && ctx->media_session.established && ctx->media_session.cid == session_snapshot.cid)
            {
                ctx->media_session.rtp_sequence = session_snapshot.rtp_sequence;
                ctx->media_session.last_rtp_timestamp = session_snapshot.last_rtp_timestamp;
            }
            pthread_mutex_unlock(&ctx->session_lock);
        }
    }
    gb_buffer_deinit(&ps_buffer);
    return NULL;
}

/* 安全停止当前媒体会话并清理 RTP 资源。 */
static void stop_media_session(Gb28181DeviceCtx *ctx)
{
    if (!ctx)
        return;
    if (!ctx->sync_ready)
    {
        close_rtp_socket(&ctx->media_session);
        reset_media_session(&ctx->media_session);
        return;
    }
    pthread_mutex_lock(&ctx->session_lock);
    ctx->pending_force_idr = 0;
    close_rtp_socket(&ctx->media_session);
    reset_media_session(&ctx->media_session);
    pthread_mutex_unlock(&ctx->session_lock);
}

/*
 * 处理 INVITE：
 * 1. 解析对端 SDP；
 * 2. 分配本地 SSRC 与 RTP socket；
 * 3. 回复 200 OK（SDP 声明 PS/90000）。
 */
static int handle_invite(Gb28181DeviceCtx *ctx, eXosip_event_t *event)
{
    osip_body_t *body = NULL;
    osip_message_t *answer = NULL;
    char sdp_body[512];
    Gb28181MediaSession new_session;
    int ret = -1;
    if (!ctx || !event || !event->request)
        return -1;
    reset_media_session(&new_session);
    if (osip_message_get_body(event->request, 0, &body) != 0 || !body || !body->body)
        return answer_call_request(ctx, event, 400);
    printf("[GB28181][INVITE] raw_sdp_begin\n%s\n[GB28181][INVITE] raw_sdp_end\n", body->body);
    if (parse_invite_sdp(body->body, &new_session) != 0)
        return answer_call_request(ctx, event, 488);
    printf("[GB28181][INVITE] parsed remote=%s:%d transport=%s y=%s\n",
           new_session.remote_ip,
           new_session.remote_port,
           new_session.transport[0] ? new_session.transport : "N/A",
           new_session.remote_ssrc[0] ? new_session.remote_ssrc : "N/A");
    if (strstr(new_session.transport, "RTP/AVP") == NULL)
        return answer_call_request(ctx, event, 488);
    pthread_mutex_lock(&ctx->session_lock);
    if (ctx->media_session.active && ctx->media_session.cid != event->cid)
    {
        pthread_mutex_unlock(&ctx->session_lock);
        return answer_call_request(ctx, event, 486);
    }
    pthread_mutex_unlock(&ctx->session_lock);
    if (use_invite_ssrc_if_valid(&new_session) == 0)
        printf("[GB28181] use invite ssrc=%s rtp_ssrc=%u\n", new_session.local_ssrc, new_session.rtp_ssrc);
    else
        generate_local_ssrc(&new_session);
    if (setup_rtp_socket(&new_session, &ctx->config) != 0)
        return answer_call_request(ctx, event, 500);
    new_session.active = 1;
    new_session.established = 0;
    new_session.cid = event->cid;
    new_session.did = event->did;
    new_session.tid = event->tid;
    /*
     * 当前设备对外声明自己发送的是 PS/90000。
     * 这和后面的实际发送格式保持一致，避免 SIP/媒体面不一致。
     */
    snprintf(sdp_body, sizeof(sdp_body),
             "v=0\r\no=%s 0 0 IN IP4 %s\r\ns=Play\r\nc=IN IP4 %s\r\nt=0 0\r\nm=video %d RTP/AVP 96\r\na=sendonly\r\na=rtpmap:96 PS/90000\r\ny=%s\r\n",
             ctx->config.device_id, ctx->config.media_ip, ctx->config.media_ip, ctx->config.media_port, new_session.local_ssrc);
    eXosip_lock(ctx->sip_context);
    eXosip_call_send_answer(ctx->sip_context, event->tid, 180, NULL);
    if (eXosip_call_build_answer(ctx->sip_context, event->tid, 200, &answer) != 0 || !answer)
    {
        eXosip_unlock(ctx->sip_context);
        close_rtp_socket(&new_session);
        return -1;
    }
    osip_message_set_content_type(answer, "APPLICATION/SDP");
    osip_message_set_body(answer, sdp_body, strlen(sdp_body));
    ret = eXosip_call_send_answer(ctx->sip_context, event->tid, 200, answer);
    eXosip_unlock(ctx->sip_context);
    if (ret != 0)
    {
        close_rtp_socket(&new_session);
        return -1;
    }
    pthread_mutex_lock(&ctx->session_lock);
    close_rtp_socket(&ctx->media_session);
    ctx->media_session = new_session;
    pthread_mutex_unlock(&ctx->session_lock);
    printf("[GB28181] invite accepted remote=%s:%d transport=%s local_media=%s:%d local_ssrc=%s\n", new_session.remote_ip, new_session.remote_port, new_session.transport, ctx->config.media_ip, ctx->config.media_port, new_session.local_ssrc);
    return 0;
}

/* SIP 事件分发入口。 */
static void process_event(Gb28181DeviceCtx *ctx, eXosip_event_t *event)
{
    if (!ctx || !event)
        return;
    switch (event->type)
    {
    case EXOSIP_REGISTRATION_SUCCESS:
    {
        long long now_ms = get_now_ms();
        ctx->registered_ok = 1;
        ctx->next_keepalive_ms = now_ms + (long long)ctx->config.keepalive_interval_sec * 1000LL;
        ctx->next_register_retry_ms = now_ms + get_register_refresh_interval_ms(ctx);
        printf("[GB28181] register success rid=%d\n", event->rid);
        break;
    }
    case EXOSIP_REGISTRATION_FAILURE:
        ctx->registered_ok = 0;
        ctx->next_register_retry_ms = get_now_ms() + (long long)ctx->config.register_retry_interval_sec * 1000LL;
        printf("[GB28181] register failure rid=%d status=%d\n", event->rid, event->response ? event->response->status_code : 0);
        handle_auth_failure(ctx, event);
        break;
    case EXOSIP_CALL_INVITE:
        handle_invite(ctx, event);
        break;
    case EXOSIP_CALL_ACK:
        pthread_mutex_lock(&ctx->session_lock);
        if (ctx->media_session.cid == event->cid)
        {
            ctx->media_session.established = 1;
            ctx->pending_force_idr = 1;
            pthread_cond_signal(&ctx->session_cond);
            printf("[GB28181] call established cid=%d did=%d remote=%s:%d\n", ctx->media_session.cid, ctx->media_session.did, ctx->media_session.remote_ip, ctx->media_session.remote_port);
            if (ctx->config.external_media_input)
            {
                printf("[GB28181] external mode: mark pending IDR request, wait keyframe from upstream\n");
            }
        }
        pthread_mutex_unlock(&ctx->session_lock);
        break;
    case EXOSIP_CALL_CLOSED:
    case EXOSIP_CALL_RELEASED:
    case EXOSIP_CALL_CANCELLED:
    case EXOSIP_CALL_NOANSWER:
    case EXOSIP_CALL_REQUESTFAILURE:
    case EXOSIP_CALL_SERVERFAILURE:
    case EXOSIP_CALL_GLOBALFAILURE:
        pthread_mutex_lock(&ctx->session_lock);
        if (ctx->media_session.cid == event->cid)
        {
            printf("[GB28181] call closed cid=%d did=%d\n", event->cid, event->did);
            ctx->pending_force_idr = 0;
            close_rtp_socket(&ctx->media_session);
            reset_media_session(&ctx->media_session);
        }
        pthread_mutex_unlock(&ctx->session_lock);
        break;
    case EXOSIP_MESSAGE_NEW:
        if (event->request && MSG_IS_MESSAGE(event->request))
            handle_query_message(ctx, event);
        else if (event->request && MSG_IS_OPTIONS(event->request))
            answer_simple_request(ctx, event, 200);
        break;
    case EXOSIP_MESSAGE_REQUESTFAILURE:
    case EXOSIP_MESSAGE_SERVERFAILURE:
    case EXOSIP_MESSAGE_GLOBALFAILURE:
        handle_auth_failure(ctx, event);
        break;
    case EXOSIP_CALL_MESSAGE_NEW:
        if (event->request && MSG_IS_BYE(event->request))
        {
            answer_call_request(ctx, event, 200);
            stop_media_session(ctx);
        }
        else if (event->request && (MSG_IS_INFO(event->request) || MSG_IS_OPTIONS(event->request)))
        {
            answer_call_request(ctx, event, 200);
        }
        break;
    default:
        break;
    }
}

/* 初始化本地采集与编码模块（internal mode）。 */
static int init_media_modules(Gb28181DeviceCtx *ctx)
{
    MppEncoderOptions options;
    if (!ctx)
        return -1;
    /* SIP 注册成功前就把采集+编码链路准备好，这样 INVITE 建立后可以尽快开始送流。 */
    if (v4l2_capture_init(ctx->capture) != 0)
        return -1;
    ctx->capture_ready = 1;
    memset(&options, 0, sizeof(options));
    options.rc_mode = MPP_ENC_RC_MODE_CBR;
    options.h264_profile = ctx->config.h264_profile;
    options.h264_level = ctx->config.h264_level;
    options.h264_cabac_en = ctx->config.h264_cabac_en;
    if (mpp_encoder_init(ctx->encoder, CAPTURE_WIDTH, CAPTURE_HEIGHT, ctx->config.fps, ctx->config.bitrate, ctx->config.gop, &options) != 0)
        return -1;
    ctx->encoder_ready = 1;
    return 0;
}

/*
 * 初始化 GB28181 设备模块。
 * 若 external_media_input=1，将跳过 V4L2/MPP 初始化与媒体线程启动。
 */
int gb28181_device_init(Gb28181DeviceCtx *ctx, const Gb28181DeviceConfig *config)
{
    int use_rport = 1;
    int udp_keepalive = 25;
    if (!ctx)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    fill_default_config(&ctx->config, config);
    /*
     * external_media_input=1: 由外部模块提供 H264（例如 mediaGateway 的共享编码输出）；
     * external_media_input=0: 本模块自管 V4L2 + MPP。
     */
    if (!ctx->config.external_media_input)
    {
        ctx->capture = (V4L2CaptureCtx *)calloc(1, sizeof(V4L2CaptureCtx));
        ctx->encoder = (MppEncoderCtx *)calloc(1, sizeof(MppEncoderCtx));
        if (!ctx->capture || !ctx->encoder)
        {
            gb28181_device_deinit(ctx);
            return -1;
        }
    }
    pthread_mutex_init(&ctx->session_lock, NULL);
    pthread_cond_init(&ctx->session_cond, NULL);
    ctx->sync_ready = 1;
    reset_media_session(&ctx->media_session);
    ctx->rid = -1;
    ctx->xml_sn = 1;
    ctx->next_register_retry_ms = get_now_ms();
    /* 仅 internal mode 需要本地采集/编码初始化。 */
    if (!ctx->config.external_media_input)
    {
        if (init_media_modules(ctx) != 0)
        {
            gb28181_device_deinit(ctx);
            return -1;
        }
    }
    ctx->sip_context = eXosip_malloc();
    if (!ctx->sip_context)
    {
        gb28181_device_deinit(ctx);
        return -1;
    }
    if (eXosip_init(ctx->sip_context) != 0)
    {
        gb28181_device_deinit(ctx);
        return -1;
    }
    eXosip_set_option(ctx->sip_context, EXOSIP_OPT_USE_RPORT, &use_rport);
    eXosip_set_option(ctx->sip_context, EXOSIP_OPT_UDP_KEEP_ALIVE, &udp_keepalive);
    eXosip_set_option(ctx->sip_context, EXOSIP_OPT_SET_HEADER_USER_AGENT, ctx->config.user_agent);
    if (eXosip_listen_addr(ctx->sip_context, IPPROTO_UDP, ctx->config.bind_ip, ctx->config.local_sip_port, AF_INET, 0) != 0)
    {
        gb28181_device_deinit(ctx);
        return -1;
    }
    if (ctx->config.device_password && ctx->config.device_password[0] != '\0')
    {
        eXosip_add_authentication_info(ctx->sip_context, ctx->config.device_id, ctx->config.device_id, ctx->config.device_password, NULL, ctx->config.server_domain);
    }
    ctx->running = 1;
    /* 仅 internal mode 启动媒体线程。 */
    if (!ctx->config.external_media_input)
    {
        if (pthread_create(&ctx->media_thread, NULL, media_thread_main, ctx) != 0)
        {
            gb28181_device_deinit(ctx);
            return -1;
        }
        ctx->media_thread_started = 1;
    }
    if (send_register_request(ctx, ctx->config.register_expires) != 0)
    {
        gb28181_device_deinit(ctx);
        return -1;
    }
    ctx->next_register_retry_ms = get_now_ms() + get_register_refresh_interval_ms(ctx);
    printf("[GB28181] start server=%s:%d device=%s domain=%s bind=%s:%d contact_ip=%s media_ip=%s:%d fps=%d bitrate=%d gop=%d\n",
           ctx->config.server_ip, ctx->config.server_port, ctx->config.device_id, ctx->config.server_domain,
           ctx->config.bind_ip, ctx->config.local_sip_port, ctx->config.sip_contact_ip,
           ctx->config.media_ip, ctx->config.media_port, ctx->config.fps, ctx->config.bitrate, ctx->config.gop);
    return 0;
}

/* SIP 主循环（阻塞），持续处理事件与周期任务。 */
int gb28181_device_run(Gb28181DeviceCtx *ctx)
{
    if (!ctx || !ctx->sip_context || !ctx->running)
        return -1;
    while (ctx->running)
    {
        long long now_ms = get_now_ms();
        eXosip_event_t *event = eXosip_event_wait(ctx->sip_context, 0, 200);
        while (event)
        {
            process_event(ctx, event);
            eXosip_event_free(event);
            event = eXosip_event_wait(ctx->sip_context, 0, 0);
        }
        eXosip_lock(ctx->sip_context);
        eXosip_automatic_action(ctx->sip_context);
        eXosip_unlock(ctx->sip_context);
        now_ms = get_now_ms();
        if (ctx->registered_ok && now_ms >= ctx->next_keepalive_ms)
        {
            if (send_keepalive(ctx) == 0)
                ctx->next_keepalive_ms = now_ms + (long long)ctx->config.keepalive_interval_sec * 1000LL;
        }
        if (now_ms >= ctx->next_register_retry_ms)
        {
            int was_registered = ctx->registered_ok;
            if (send_register_request(ctx, ctx->config.register_expires) == 0)
            {
                if (was_registered)
                    ctx->next_register_retry_ms = now_ms + get_register_refresh_interval_ms(ctx);
                else
                    ctx->next_register_retry_ms = now_ms + (long long)ctx->config.register_retry_interval_sec * 1000LL;
            }
        }
    }
    return 0;
}

/*
 * 外部输入 H264 帧发送接口（external mode 核心）。
 * 会在函数内快照当前会话，避免长时间持锁执行网络发送。
 */
int gb28181_device_send_h264(Gb28181DeviceCtx *ctx,
                             const uint8_t *h264_data,
                             size_t h264_len,
                             int is_key_frame,
                             uint64_t pts_us)
{
    Gb28181MediaSession session_snapshot;
    Gb28181Buffer ps_buffer;
    uint32_t rtp_timestamp = 0;

    if (!ctx || !h264_data || h264_len == 0)
        return -1;

    /*
     * 发送前先快照会话，避免后续 PS 封装与 RTP 发送阶段长期占用锁，
     * 减少与 SIP 线程（会话切换）的竞争。
     */
    pthread_mutex_lock(&ctx->session_lock);
    if (!ctx->media_session.active || !ctx->media_session.established || ctx->media_session.rtp_socket_fd < 0)
    {
        pthread_mutex_unlock(&ctx->session_lock);
        return 0;
    }
    if (ctx->pending_force_idr && is_key_frame)
    {
        ctx->pending_force_idr = 0;
        printf("[GB28181] pending IDR request satisfied by upstream keyframe\n");
    }
    session_snapshot = ctx->media_session;
    pthread_mutex_unlock(&ctx->session_lock);

    if (gb_buffer_init(&ps_buffer, GB28181_PS_BUFFER_INIT_SIZE) != 0)
        return -1;
    if (build_ps_frame(h264_data, h264_len, is_key_frame, pts_us, &ps_buffer) != 0)
    {
        gb_buffer_deinit(&ps_buffer);
        return 0;
    }

    rtp_timestamp = (uint32_t)((pts_us * 90ULL / 1000ULL) & 0xFFFFFFFFU);
    if (send_ps_over_rtp(&session_snapshot, ps_buffer.data, ps_buffer.size, rtp_timestamp) != 0)
    {
        gb_buffer_deinit(&ps_buffer);
        /* 发送失败时主动关闭当前会话，促使上层重新拉起点播。 */
        pthread_mutex_lock(&ctx->session_lock);
        if (ctx->media_session.cid == session_snapshot.cid)
        {
            close_rtp_socket(&ctx->media_session);
            reset_media_session(&ctx->media_session);
        }
        pthread_mutex_unlock(&ctx->session_lock);
        return -1;
    }
    gb_buffer_deinit(&ps_buffer);

    pthread_mutex_lock(&ctx->session_lock);
    if (ctx->media_session.active && ctx->media_session.established && ctx->media_session.cid == session_snapshot.cid)
    {
        ctx->media_session.rtp_sequence = session_snapshot.rtp_sequence;
        ctx->media_session.last_rtp_timestamp = session_snapshot.last_rtp_timestamp;
    }
    pthread_mutex_unlock(&ctx->session_lock);
    return 0;
}

/* 请求停止运行。 */
void gb28181_device_stop(Gb28181DeviceCtx *ctx)
{
    if (!ctx)
        return;
    ctx->running = 0;
    if (ctx->sync_ready)
    {
        pthread_mutex_lock(&ctx->session_lock);
        pthread_cond_broadcast(&ctx->session_cond);
        pthread_mutex_unlock(&ctx->session_lock);
    }
}

/* 释放模块资源。 */
void gb28181_device_deinit(Gb28181DeviceCtx *ctx)
{
    if (!ctx)
        return;
    gb28181_device_stop(ctx);
    if (ctx->media_thread_started)
    {
        pthread_join(ctx->media_thread, NULL);
        ctx->media_thread = 0;
        ctx->media_thread_started = 0;
    }
    stop_media_session(ctx);
    if (ctx->sip_context)
    {
        eXosip_quit(ctx->sip_context);
        ctx->sip_context = NULL;
    }
    if (ctx->encoder_ready && ctx->encoder)
    {
        mpp_encoder_deinit(ctx->encoder);
        ctx->encoder_ready = 0;
    }
    if (ctx->capture_ready && ctx->capture)
    {
        v4l2_capture_deinit(ctx->capture);
        ctx->capture_ready = 0;
    }
    free(ctx->encoder);
    free(ctx->capture);
    ctx->encoder = NULL;
    ctx->capture = NULL;
    if (ctx->sync_ready)
    {
        pthread_cond_destroy(&ctx->session_cond);
        pthread_mutex_destroy(&ctx->session_lock);
        ctx->sync_ready = 0;
    }
    memset(&ctx->config, 0, sizeof(ctx->config));
    ctx->registered_ok = 0;
    ctx->rid = -1;
}

/* 读取当前会话快照（线程安全）。 */
void gb28181_device_get_media_session(const Gb28181DeviceCtx *ctx, Gb28181MediaSession *session)
{
    if (!ctx || !session)
        return;
    if (!ctx->sync_ready)
    {
        *session = ctx->media_session;
        return;
    }
    pthread_mutex_lock((pthread_mutex_t *)&ctx->session_lock);
    *session = ctx->media_session;
    pthread_mutex_unlock((pthread_mutex_t *)&ctx->session_lock);
}
