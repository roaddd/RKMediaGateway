#include "../inc/rtmpOutput.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"

#if defined(ENABLE_RTMP_LIBRTMP)
#include "librtmp/rtmp.h"
#include "librtmp/amf.h"
#endif

#define RTMP_VIDEO_CHANNEL 0x04
#define RTMP_AUDIO_CHANNEL 0x05
#define RTMP_INFO_CHANNEL 0x03
#define FLV_VIDEO_CODEC_AVC 7
#define FLV_FRAME_KEY 1
#define FLV_FRAME_INTER 2
#define FLV_AVC_SEQ_HEADER 0
#define FLV_AVC_NALU 1
#define FLV_AUDIO_CODEC_G711A 7
#define FLV_AUDIO_CODEC_G711U 8
#define DEFAULT_RTMP_NAME "rtmp"
#define DEFAULT_RTMP_QUEUE_CAPACITY 64
#define DEFAULT_RTMP_RECONNECT_INTERVAL_MS 1000
#define DEFAULT_RTMP_CONNECT_TIMEOUT_MS 3000

/* Annex-B 码流拆分后的 NALU 视图，只引用原始帧内存。 */
typedef struct {
    uint8_t *data;
    size_t size;
} NaluView;

/* RTMP 输出私有状态，负责 librtmp 连接以及 H.264 参数集缓存。 */
typedef struct {
    MediaOutputRtmpConfig config;
    int connected;
    int metadata_sent;
    int sequence_header_sent;
    uint32_t stream_id;
    uint32_t last_rtmp_ts_ms;
    uint8_t *sps;
    size_t sps_len;
    uint8_t *pps;
    size_t pps_len;
#if defined(ENABLE_RTMP_LIBRTMP)
    RTMP *rtmp;
#endif
} RtmpOutputImpl;


#if defined(ENABLE_RTMP_LIBRTMP)
static const char *rtmp_frame_kind(int is_key_frame) {
    return is_key_frame ? "key" : "inter";
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
    size_t i;

    /* Annex-B 起始码可能是 3 字节或 4 字节。 */
    for (i = offset; i + 3 <= len; ++i) {
        int cur_len = start_code_len(data + i, len - i);
        if (cur_len > 0) {
            *pos = i;
            *code_len = cur_len;
            return 0;
        }
    }
    return -1;
}


static void write_be32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)((value >> 24) & 0xFF);
    dst[1] = (uint8_t)((value >> 16) & 0xFF);
    dst[2] = (uint8_t)((value >> 8) & 0xFF);
    dst[3] = (uint8_t)(value & 0xFF);
}


static void write_be16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)((value >> 8) & 0xFF);
    dst[1] = (uint8_t)(value & 0xFF);
}
#endif

#if defined(ENABLE_RTMP_LIBRTMP)

static void write_amf_double(uint8_t *dst, double value) {
    union {
        double d;
        uint8_t b[8];
    } num;
    int i;

    num.d = value;
    for (i = 0; i < 8; ++i) {
        dst[i] = num.b[7 - i];
    }
}


static uint8_t *amf_write_string(uint8_t *dst, const char *str) {
    size_t len = str ? strlen(str) : 0;

    *dst++ = AMF_STRING;
    write_be16(dst, (uint16_t)len);
    dst += 2;
    if (len > 0) {
        memcpy(dst, str, len);
        dst += len;
    }
    return dst;
}


static uint8_t *amf_write_named_number(uint8_t *dst, const char *name, double value) {
    size_t name_len = name ? strlen(name) : 0;

    write_be16(dst, (uint16_t)name_len);
    dst += 2;
    if (name_len > 0) {
        memcpy(dst, name, name_len);
        dst += name_len;
    }
    *dst++ = AMF_NUMBER;
    write_amf_double(dst, value);
    dst += 8;
    return dst;
}


static uint8_t *amf_write_named_bool(uint8_t *dst, const char *name, int value) {
    size_t name_len = name ? strlen(name) : 0;

    write_be16(dst, (uint16_t)name_len);
    dst += 2;
    if (name_len > 0) {
        memcpy(dst, name, name_len);
        dst += name_len;
    }
    *dst++ = AMF_BOOLEAN;
    *dst++ = value ? 1 : 0;
    return dst;
}


static uint8_t *amf_write_named_string(uint8_t *dst, const char *name, const char *value) {
    size_t name_len = name ? strlen(name) : 0;
    size_t value_len = value ? strlen(value) : 0;

    write_be16(dst, (uint16_t)name_len);
    dst += 2;
    if (name_len > 0) {
        memcpy(dst, name, name_len);
        dst += name_len;
    }
    *dst++ = AMF_STRING;
    write_be16(dst, (uint16_t)value_len);
    dst += 2;
    if (value_len > 0) {
        memcpy(dst, value, value_len);
        dst += value_len;
    }
    return dst;
}


static uint8_t *amf_write_object_end(uint8_t *dst) {
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = AMF_OBJECT_END;
    return dst + 3;
}
#endif


static void free_parameter_set(uint8_t **data, size_t *size) {
    if (*data) {
        free(*data);
        *data = NULL;
    }
    *size = 0;
}


#if defined(ENABLE_RTMP_LIBRTMP)
static int update_parameter_set(uint8_t **dst, size_t *dst_len, const uint8_t *src, size_t src_len, int *changed) {
    uint8_t *copy;

    if (!dst || !dst_len || !src || src_len == 0) {
        LOG_ERROR("[RTMP][ERROR] update_parameter_set invalid args\n");
        return -1;
    }

    if (*dst && *dst_len == src_len && memcmp(*dst, src, src_len) == 0) {
        if (changed) {
            *changed = 0;
        }
        return 0;
    }

    copy = (uint8_t *)malloc(src_len);
    if (!copy) {
        LOG_ERROR("[RTMP][ERROR] update_parameter_set alloc failed size=%zu\n", src_len);
        return -1;
    }
    memcpy(copy, src, src_len);

    free_parameter_set(dst, dst_len);
    *dst = copy;
    *dst_len = src_len;
    if (changed) {
        *changed = 1;
    }
    return 0;
}


/* 解析 Annex-B 帧，输出不带起始码的 NALU 列表。 */
static int annexb_split_nalus(const uint8_t *data, size_t len, NaluView **out_nalus, size_t *out_count) {
    size_t pos = 0;
    size_t first = 0;
    int code_len = 0;
    NaluView *nalus = NULL;
    size_t count = 0;
    size_t capacity = 0;

    if (!data || len == 0 || !out_nalus || !out_count) {
        LOG_ERROR("[RTMP][ERROR] annexb_split_nalus invalid args len=%zu\n", len);
        return -1;
    }

    
    if (find_start_code(data, len, 0, &first, &code_len) != 0) {
        nalus = (NaluView *)calloc(1, sizeof(*nalus));
        if (!nalus) {
            LOG_ERROR("[RTMP][ERROR] annexb_split_nalus alloc fallback nalu failed\n");
            return -1;
        }
        nalus[0].data = (uint8_t *)data;
        nalus[0].size = len;
        *out_nalus = nalus;
        *out_count = 1;
        return 0;
    }

    
    pos = first;
    while (pos < len) {
        size_t payload_start = pos + (size_t)code_len;
        size_t next = len;
        int next_code_len = 0;
        NaluView *tmp;

        if (payload_start >= len) {
            break;
        }
        find_start_code(data, len, payload_start, &next, &next_code_len);
        if (next > payload_start) {
            if (count == capacity) {
                capacity = (capacity == 0) ? 4 : capacity * 2;
                tmp = (NaluView *)realloc(nalus, capacity * sizeof(*nalus));
                if (!tmp) {
                    LOG_ERROR("[RTMP][ERROR] annexb_split_nalus realloc failed capacity=%zu\n", capacity);
                    free(nalus);
                    return -1;
                }
                nalus = tmp;
            }
            nalus[count].data = (uint8_t *)(data + payload_start);
            nalus[count].size = next - payload_start;
            count++;
        }
        if (next >= len) {
            break;
        }
        pos = next;
        code_len = next_code_len;
    }

    *out_nalus = nalus;
    *out_count = count;
    return 0;
}


/* 缓存 SPS/PPS，后续用于发送 AVCDecoderConfigurationRecord。 */
static int rtmp_cache_parameter_sets(RtmpOutputImpl *impl, const NaluView *nalus, size_t count) {
    size_t i;
    int changed = 0;

    for (i = 0; i < count; ++i) {
        uint8_t nalu_type;

        if (!nalus[i].data || nalus[i].size == 0) {
            continue;
        }
        nalu_type = (uint8_t)(nalus[i].data[0] & 0x1F);
        
        if (nalu_type == 7) {
            if (update_parameter_set(&impl->sps, &impl->sps_len, nalus[i].data, nalus[i].size, &changed) != 0) {
                LOG_ERROR("[RTMP][ERROR] cache SPS failed size=%zu\n", nalus[i].size);
                return -1;
            }
            if (changed) {
                impl->sequence_header_sent = 0;
            }
        } else if (nalu_type == 8) {
            if (update_parameter_set(&impl->pps, &impl->pps_len, nalus[i].data, nalus[i].size, &changed) != 0) {
                LOG_ERROR("[RTMP][ERROR] cache PPS failed size=%zu\n", nalus[i].size);
                return -1;
            }
            if (changed) {
                impl->sequence_header_sent = 0;
            }
        }
    }
    return 0;
}

static uint32_t packet_timestamp_ms(const MediaPacket *packet);

/* 发送 RTMP 信息类消息，例如 onMetaData。 */
static int rtmp_send_info_body(RtmpOutputImpl *impl, const uint8_t *body, size_t body_size, uint32_t timestamp_ms) {
    RTMPPacket packet;
    int ret;

    if (!impl || !impl->rtmp || !body || body_size == 0) {
        LOG_ERROR("[RTMP][ERROR] send_info_body invalid args body_size=%zu\n", body_size);
        return -1;
    }

    RTMPPacket_Reset(&packet);
    if (!RTMPPacket_Alloc(&packet, (int)body_size)) {
        LOG_ERROR("[RTMP][ERROR] send_info_body packet alloc failed size=%zu\n", body_size);
        return -1;
    }

    packet.m_packetType = RTMP_PACKET_TYPE_INFO;
    packet.m_nChannel = RTMP_INFO_CHANNEL;
    packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
    packet.m_hasAbsTimestamp = 0;
    packet.m_nTimeStamp = timestamp_ms;
    packet.m_nInfoField2 = impl->stream_id;
    packet.m_nBodySize = (uint32_t)body_size;
    memcpy(packet.m_body, body, body_size);

    ret = RTMP_SendPacket(impl->rtmp, &packet, 1);
    RTMPPacket_Free(&packet);
    if (!ret) {
        LOG_ERROR("[RTMP] event=send_info_failed stream_id=%u body_size=%zu ts_ms=%u\n",
                impl->stream_id,
                body_size,
                timestamp_ms);
    }
    return ret ? 0 : -1;
}


/* 发送 RTMP 视频消息，payload 是 FLV video tag body。 */
static int rtmp_send_video_body(RtmpOutputImpl *impl, const uint8_t *body, size_t body_size, uint32_t timestamp_ms) {
    RTMPPacket packet;
    int ret;

    if (!impl || !impl->rtmp || !body || body_size == 0) {
        LOG_ERROR("[RTMP][ERROR] send_video_body invalid args body_size=%zu\n", body_size);
        return -1;
    }

    RTMPPacket_Reset(&packet);
    if (!RTMPPacket_Alloc(&packet, (int)body_size)) {
        LOG_ERROR("[RTMP][ERROR] send_video_body packet alloc failed size=%zu\n", body_size);
        return -1;
    }

    packet.m_packetType = RTMP_PACKET_TYPE_VIDEO;
    packet.m_nChannel = RTMP_VIDEO_CHANNEL;
    packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
    packet.m_hasAbsTimestamp = 0;
    packet.m_nTimeStamp = timestamp_ms;
    packet.m_nInfoField2 = impl->stream_id;
    packet.m_nBodySize = (uint32_t)body_size;
    memcpy(packet.m_body, body, body_size);

    ret = RTMP_SendPacket(impl->rtmp, &packet, 1);
    RTMPPacket_Free(&packet);
    if (!ret) {
        LOG_ERROR("[RTMP] event=send_video_failed stream_id=%u body_size=%zu ts_ms=%u\n",
                impl->stream_id,
                body_size,
                timestamp_ms);
    }
    return ret ? 0 : -1;
}

/* 发送 RTMP 音频消息，payload 是 FLV audio tag body。 */
static int rtmp_send_audio_body(RtmpOutputImpl *impl, const uint8_t *body, size_t body_size, uint32_t timestamp_ms) {
    RTMPPacket packet;
    int ret;

    if (!impl || !impl->rtmp || !body || body_size == 0) {
        LOG_ERROR("[RTMP][ERROR] send_audio_body invalid args body_size=%zu\n", body_size);
        return -1;
    }

    RTMPPacket_Reset(&packet);
    if (!RTMPPacket_Alloc(&packet, (int)body_size)) {
        LOG_ERROR("[RTMP][ERROR] send_audio_body packet alloc failed size=%zu\n", body_size);
        return -1;
    }

    packet.m_packetType = RTMP_PACKET_TYPE_AUDIO;
    packet.m_nChannel = RTMP_AUDIO_CHANNEL;
    packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
    packet.m_hasAbsTimestamp = 0;
    packet.m_nTimeStamp = timestamp_ms;
    packet.m_nInfoField2 = impl->stream_id;
    packet.m_nBodySize = (uint32_t)body_size;
    memcpy(packet.m_body, body, body_size);

    ret = RTMP_SendPacket(impl->rtmp, &packet, 1);
    RTMPPacket_Free(&packet);
    if (!ret) {
        LOG_ERROR("[RTMP] event=send_audio_failed stream_id=%u body_size=%zu ts_ms=%u\n",
                impl->stream_id,
                body_size,
                timestamp_ms);
    }
    return ret ? 0 : -1;
}


/* 首帧前发送元数据，便于播放器获得分辨率和编码信息。 */
static int rtmp_send_on_metadata(RtmpOutputImpl *impl) {
    uint8_t body[512];
    uint8_t *p = body;

    if (!impl) {
        LOG_ERROR("[RTMP][ERROR] send_on_metadata impl is NULL\n");
        return -1;
    }

    
    p = amf_write_string(p, "onMetaData");
    *p++ = AMF_ECMA_ARRAY;
    write_be32(p, 10);
    p += 4;
    p = amf_write_named_number(p, "duration", 0.0);
    p = amf_write_named_number(p, "width", (double)impl->config.video_width);
    p = amf_write_named_number(p, "height", (double)impl->config.video_height);
    p = amf_write_named_number(p, "framerate", (double)impl->config.video_fps);
    p = amf_write_named_number(p, "videodatarate", (double)impl->config.video_bitrate / 1000.0);
    p = amf_write_named_number(p, "videocodecid", 7.0);
    p = amf_write_named_bool(p, "hasVideo", 1);
    p = amf_write_named_bool(p, "hasAudio", impl->config.audio_enabled ? 1 : 0);
    p = amf_write_named_string(p, "encoder", impl->config.encoder_name ? impl->config.encoder_name : "RKMediaGateway");
    p = amf_write_named_string(p, "videocodecname", impl->config.video_codec_name ? impl->config.video_codec_name : "H264");
    p = amf_write_object_end(p);

    if (rtmp_send_info_body(impl, body, (size_t)(p - body), 0) != 0) {
        LOG_ERROR("[RTMP][ERROR] send_on_metadata failed\n");
        return -1;
    }

    printf("[RTMP] event=metadata_sent width=%d height=%d fps=%d bitrate=%d has_audio=%d\n",
           impl->config.video_width,
           impl->config.video_height,
           impl->config.video_fps,
           impl->config.video_bitrate,
           impl->config.audio_enabled ? 1 : 0);
    impl->metadata_sent = 1;
    return 0;
}


/* 发送 AVC sequence header，播放器依赖它初始化 H.264 解码器。 */
static int rtmp_send_avc_sequence_header(RtmpOutputImpl *impl, uint32_t timestamp_ms) {
    uint8_t *body;
    size_t body_size;
    size_t offset = 0;

    if (!impl || !impl->sps || !impl->pps || impl->sps_len < 4 || impl->pps_len == 0) {
        LOG_ERROR("[RTMP][ERROR] send_avc_sequence_header SPS/PPS not ready sps=%zu pps=%zu\n",
                impl ? impl->sps_len : 0,
                impl ? impl->pps_len : 0);
        return -1;
    }

    body_size = 5 + 6 + 2 + impl->sps_len + 1 + 2 + impl->pps_len;
    body = (uint8_t *)malloc(body_size);
    if (!body) {
        LOG_ERROR("[RTMP][ERROR] send_avc_sequence_header alloc failed size=%zu\n", body_size);
        return -1;
    }

    
    body[offset++] = (uint8_t)((FLV_FRAME_KEY << 4) | FLV_VIDEO_CODEC_AVC);
    body[offset++] = FLV_AVC_SEQ_HEADER;
    body[offset++] = 0;
    body[offset++] = 0;
    body[offset++] = 0;

    body[offset++] = 1;
    body[offset++] = impl->sps[1];
    body[offset++] = impl->sps[2];
    body[offset++] = impl->sps[3];
    body[offset++] = 0xFF;
    body[offset++] = 0xE1;
    write_be16(body + offset, (uint16_t)impl->sps_len);
    offset += 2;
    memcpy(body + offset, impl->sps, impl->sps_len);
    offset += impl->sps_len;
    body[offset++] = 1;
    write_be16(body + offset, (uint16_t)impl->pps_len);
    offset += 2;
    memcpy(body + offset, impl->pps, impl->pps_len);
    offset += impl->pps_len;

    if (offset != body_size) {
        LOG_ERROR("[RTMP][ERROR] send_avc_sequence_header size mismatch offset=%zu body_size=%zu\n",
                offset, body_size);
        free(body);
        return -1;
    }

    if (rtmp_send_video_body(impl, body, body_size, timestamp_ms) != 0) {
        LOG_ERROR("[RTMP][ERROR] send_avc_sequence_header send failed ts_ms=%u\n", timestamp_ms);
        free(body);
        return -1;
    }

    printf("[RTMP] event=sequence_header_sent sps=%zu pps=%zu ts_ms=%u\n",
           impl->sps_len,
           impl->pps_len,
           timestamp_ms);
    impl->sequence_header_sent = 1;
    free(body);
    return 0;
}


/* 将 Annex-B NALU 打包为 FLV/AVC length-prefixed payload 后发送。 */
static int rtmp_send_avc_nalus(RtmpOutputImpl *impl,
                               const NaluView *nalus,
                               size_t count,
                               uint32_t timestamp_ms,
                               int is_key_frame) {
    uint8_t *body;
    size_t body_size = 5;
    size_t offset = 0;
    size_t i;

    
    for (i = 0; i < count; ++i) {
        uint8_t nalu_type;

        if (!nalus[i].data || nalus[i].size == 0) {
            continue;
        }
        nalu_type = (uint8_t)(nalus[i].data[0] & 0x1F);
        if (nalu_type == 7 || nalu_type == 8 || nalu_type == 9) {
            continue;
        }
        body_size += 4 + nalus[i].size;
    }

    if (body_size == 5) {
        return 0;
    }

    body = (uint8_t *)malloc(body_size);
    if (!body) {
        LOG_ERROR("[RTMP][ERROR] send_avc_nalus alloc failed size=%zu\n", body_size);
        return -1;
    }

    body[offset++] = (uint8_t)(((is_key_frame ? FLV_FRAME_KEY : FLV_FRAME_INTER) << 4) | FLV_VIDEO_CODEC_AVC);
    body[offset++] = FLV_AVC_NALU;
    body[offset++] = 0;
    body[offset++] = 0;
    body[offset++] = 0;

    for (i = 0; i < count; ++i) {
        uint8_t nalu_type;

        if (!nalus[i].data || nalus[i].size == 0) {
            continue;
        }
        nalu_type = (uint8_t)(nalus[i].data[0] & 0x1F);
        if (nalu_type == 7 || nalu_type == 8 || nalu_type == 9) {
            continue;
        }
        write_be32(body + offset, (uint32_t)nalus[i].size);
        offset += 4;
        memcpy(body + offset, nalus[i].data, nalus[i].size);
        offset += nalus[i].size;
    }

    if (offset != body_size) {
        LOG_ERROR("[RTMP][ERROR] send_avc_nalus size mismatch offset=%zu body_size=%zu\n",
                offset, body_size);
        free(body);
        return -1;
    }

    if (rtmp_send_video_body(impl, body, body_size, timestamp_ms) != 0) {
        LOG_ERROR("[RTMP][ERROR] send_avc_nalus send failed ts_ms=%u\n", timestamp_ms);
        free(body);
        return -1;
    }

    if (is_key_frame) {
        printf("[RTMP] event=video_sent frame_type=%s nalu_count=%zu ts_ms=%u\n",
               rtmp_frame_kind(is_key_frame),
               count,
               timestamp_ms);
    }
    free(body);
    return 0;
}

static int rtmp_send_g711_packet(RtmpOutputImpl *impl, const MediaPacket *packet) {
    uint8_t *body;
    size_t body_size;
    uint8_t sound_format;
    uint32_t timestamp_ms;
    int ret;

    if (!impl || !packet || !packet->buffer || !packet->buffer->data || packet->buffer->size == 0) {
        LOG_ERROR("[RTMP][ERROR] send_g711 invalid args packet=%p\n", (const void *)packet);
        return -1;
    }
    if (packet->codec == MEDIA_CODEC_G711A) {
        sound_format = FLV_AUDIO_CODEC_G711A;
    } else if (packet->codec == MEDIA_CODEC_G711U) {
        sound_format = FLV_AUDIO_CODEC_G711U;
    } else {
        return 0;
    }

    body_size = packet->buffer->size + 1;
    body = (uint8_t *)malloc(body_size);
    if (!body) {
        LOG_ERROR("[RTMP][ERROR] send_g711 alloc failed size=%zu\n", body_size);
        return -1;
    }

    /*
     * FLV audio header:
     * SoundFormat=7/8(G711A/G711U), SoundRate/SoundSize 对 G711 无实际意义，SoundType=0 mono。
     */
    body[0] = (uint8_t)(sound_format << 4);
    memcpy(body + 1, packet->buffer->data, packet->buffer->size);
    timestamp_ms = packet_timestamp_ms(packet);
    ret = rtmp_send_audio_body(impl, body, body_size, timestamp_ms);
    free(body);
    if (ret != 0) {
        LOG_ERROR("[RTMP] event=audio_payload_send_failed frame=%" PRIu64 " codec=%d ts_ms=%u\n",
                  packet->frame_id,
                  packet->codec,
                  timestamp_ms);
    }
    return ret;
}
static uint32_t packet_timestamp_ms(const MediaPacket *packet) {
    uint64_t ts_us;

    if (!packet) {
        return 0;
    }
    ts_us = packet->dts_us ? packet->dts_us : packet->pts_us;
    return (uint32_t)(ts_us / 1000ULL);
}
#endif


static int rtmp_output_start(MediaOutput *output) {
    RtmpOutputImpl *impl = (RtmpOutputImpl *)output->impl;

    
    if (!impl->config.publish_url || impl->config.publish_url[0] == '\0') {
        LOG_WARN("RTMP output disabled: publish_url is empty");
        return -1;
    }

    printf("[INFO] RTMP output configured: %s\n", impl->config.publish_url);
    printf("[INFO] RTMP audio path reserved, current audio_enabled=%d\n", impl->config.audio_enabled);
    return 0;
}


static int rtmp_output_connect(MediaOutput *output) {
    RtmpOutputImpl *impl = (RtmpOutputImpl *)output->impl;

#if defined(ENABLE_RTMP_LIBRTMP)
    
    if (!impl) {
        LOG_ERROR("[RTMP][ERROR] connect failed: impl is NULL\n");
        return -1;
    }
    if (impl->rtmp) {
        RTMP_Close(impl->rtmp);
        RTMP_Free(impl->rtmp);
        impl->rtmp = NULL;
    }

    impl->rtmp = RTMP_Alloc();
    if (!impl->rtmp) {
        LOG_ERROR("[RTMP][ERROR] connect failed: RTMP_Alloc\n");
        return -1;
    }
    RTMP_Init(impl->rtmp);
    impl->rtmp->Link.timeout = (impl->config.connect_timeout_ms > 0)
        ? ((impl->config.connect_timeout_ms + 999) / 1000)
        : 3;

    if (!RTMP_SetupURL(impl->rtmp, (char *)impl->config.publish_url)) {
        LOG_ERROR("[RTMP] event=setup_url_failed url=%s\n", impl->config.publish_url);
        RTMP_Free(impl->rtmp);
        impl->rtmp = NULL;
        return -1;
    }
    RTMP_EnableWrite(impl->rtmp);
    RTMP_SetBufferMS(impl->rtmp, 0);

    if (!RTMP_Connect(impl->rtmp, NULL)) {
        LOG_ERROR("[RTMP] event=connect_failed url=%s timeout_s=%d\n",
                impl->config.publish_url,
                impl->rtmp->Link.timeout);
        RTMP_Close(impl->rtmp);
        RTMP_Free(impl->rtmp);
        impl->rtmp = NULL;
        return -1;
    }
    if (!RTMP_ConnectStream(impl->rtmp, 0)) {
        LOG_ERROR("[RTMP] event=connect_stream_failed url=%s\n", impl->config.publish_url);
        RTMP_Close(impl->rtmp);
        RTMP_Free(impl->rtmp);
        impl->rtmp = NULL;
        return -1;
    }

    impl->connected = 1;
    impl->stream_id = (uint32_t)impl->rtmp->m_stream_id;
    impl->metadata_sent = 0;
    impl->sequence_header_sent = 0;
    impl->last_rtmp_ts_ms = 0;
    printf("[RTMP] event=publish_ready url=%s stream_id=%u timeout_ms=%d\n",
           impl->config.publish_url,
           impl->stream_id,
           impl->config.connect_timeout_ms);
    return 0;
#else
    (void)impl;
    (void)output;
    LOG_WARN("RTMP output requested but librtmp support is not compiled in");
    return -1;
#endif
}


static int rtmp_output_send_packet(MediaOutput *output, const MediaPacket *packet) {
    RtmpOutputImpl *impl = (RtmpOutputImpl *)output->impl;
    NaluView *nalus = NULL;
    size_t nalu_count = 0;
    uint32_t timestamp_ms;
    int ret = -1;

    if (!impl || !impl->connected || !packet || !packet->buffer) {
        LOG_ERROR("[RTMP][ERROR] send_packet invalid args connected=%d packet=%p buffer=%p\n",
                (impl && impl->connected) ? 1 : 0,
                (void *)packet,
                (void *)(packet ? packet->buffer : NULL));
        return -1;
    }
#if defined(ENABLE_RTMP_LIBRTMP)
    if (packet->frame_type == MEDIA_FRAME_TYPE_AUDIO) {
        /* 传统 FLV/RTMP 没有通用 Opus SoundFormat，静默跳过以免输出线程误判断线。 */
        if (packet->codec == MEDIA_CODEC_OPUS)
            return MEDIA_OK;
        return rtmp_send_g711_packet(impl, packet);
    }
    if (packet->frame_type != MEDIA_FRAME_TYPE_VIDEO || packet->codec != MEDIA_CODEC_H264) {
        return 0;
    }

    if (annexb_split_nalus(packet->buffer->data, packet->buffer->size, &nalus, &nalu_count) != 0) {
        LOG_ERROR("[RTMP][ERROR] send_packet split annexb failed frame=%" PRIu64 " size=%zu\n",
                packet->frame_id,
                packet->buffer->size);
        return -1;
    }
    if (rtmp_cache_parameter_sets(impl, nalus, nalu_count) != 0) {
        goto done;
    }

    timestamp_ms = packet_timestamp_ms(packet);

    /* 建链后的首个可发送视频帧负责补齐 metadata 和 sequence header。 */
    if (!impl->metadata_sent) {
        if (rtmp_send_on_metadata(impl) != 0) {
            LOG_ERROR("[RTMP] event=metadata_send_failed frame=%" PRIu64 "\n", packet->frame_id);
            goto done;
        }
    }
    if (!impl->sequence_header_sent) {
        if (!impl->sps || !impl->pps) {
            
            LOG_WARN("RTMP skip frame=%" PRIu64 " because SPS/PPS not ready", packet->frame_id);
            ret = 0;
            goto done;
        }
        if (rtmp_send_avc_sequence_header(impl, timestamp_ms) != 0) {
            LOG_ERROR("[RTMP] event=sequence_header_send_failed frame=%" PRIu64 "\n", packet->frame_id);
            goto done;
        }
    }

    ret = rtmp_send_avc_nalus(impl, nalus, nalu_count, timestamp_ms, packet->is_key_frame);
    if (ret == 0) {
        impl->last_rtmp_ts_ms = timestamp_ms;
    } else {
        LOG_ERROR("[RTMP] event=video_payload_send_failed frame=%" PRIu64 " frame_type=%s ts_ms=%u\n",
                packet->frame_id,
                rtmp_frame_kind(packet->is_key_frame),
                timestamp_ms);
    }
done:
    free(nalus);
    return ret;
#else
    (void)impl;
    (void)nalus;
    (void)nalu_count;
    (void)timestamp_ms;
    (void)ret;
    (void)output;
    (void)packet;
    LOG_ERROR("[RTMP][ERROR] send_packet failed: librtmp support is not compiled in\n");
    return -1;
#endif
}


static void rtmp_output_disconnect(MediaOutput *output) {
    RtmpOutputImpl *impl = (RtmpOutputImpl *)output->impl;

    if (!impl) {
        return;
    }

#if defined(ENABLE_RTMP_LIBRTMP)
    if (impl->rtmp) {
        printf("[RTMP] event=disconnect url=%s last_ts_ms=%u\n",
               impl->config.publish_url ? impl->config.publish_url : "",
               impl->last_rtmp_ts_ms);
        RTMP_Close(impl->rtmp);
        RTMP_Free(impl->rtmp);
        impl->rtmp = NULL;
    }
#endif
    impl->connected = 0;
    impl->stream_id = 0;
    impl->metadata_sent = 0;
    impl->sequence_header_sent = 0;
    impl->last_rtmp_ts_ms = 0;
}


static void rtmp_output_stop(MediaOutput *output) {
    RtmpOutputImpl *impl = (RtmpOutputImpl *)output->impl;

    if (!impl) {
        return;
    }

    rtmp_output_disconnect(output);
    free_parameter_set(&impl->sps, &impl->sps_len);
    free_parameter_set(&impl->pps, &impl->pps_len);
}

/* 查询传统 FLV/RTMP SoundFormat 当前已经实现的音频编码集合。 */
static int rtmp_output_get_audio_capabilities(const MediaOutput *output,
                                              MediaOutputAudioCapabilities *capabilities) {
    (void)output;

    if (!capabilities) {
        LOG_ERROR("[RTMP][ERROR] get_audio_capabilities failed: capabilities is NULL\n");
        return MEDIA_ERR_INVALID_PARAM;
    }
    capabilities->codec_mask = MEDIA_OUTPUT_AUDIO_CODEC_MASK(MEDIA_CODEC_G711A) |
                               MEDIA_OUTPUT_AUDIO_CODEC_MASK(MEDIA_CODEC_G711U);
    capabilities->count = 2;
    capabilities->entries[0].codec = MEDIA_CODEC_G711A;
    capabilities->entries[0].sample_rate = 8000;
    capabilities->entries[0].min_channels = 1;
    capabilities->entries[0].max_channels = 1;
    capabilities->entries[1].codec = MEDIA_CODEC_G711U;
    capabilities->entries[1].sample_rate = 8000;
    capabilities->entries[1].min_channels = 1;
    capabilities->entries[1].max_channels = 1;
    return MEDIA_OK;
}


int media_output_setup_rtmp(MediaOutput *output, const MediaOutputRtmpConfig *config) {
    static const MediaOutputVTable vtable = {
        rtmp_output_start,
        rtmp_output_connect,
        rtmp_output_send_packet,
        rtmp_output_disconnect,
        rtmp_output_stop,
        NULL,
        NULL,
        rtmp_output_get_audio_capabilities
    };
    MediaOutputChannelConfig output_config;
    RtmpOutputImpl *impl;

    if (!output) {
        LOG_ERROR("[RTMP][ERROR] output_setup failed: output is NULL\n");
        return -1;
    }

    impl = (RtmpOutputImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        LOG_ERROR("[RTMP][ERROR] output_setup failed: impl alloc\n");
        return -1;
    }

    if (config) {
        impl->config = *config;
    }
    if (!impl->config.name) {
        impl->config.name = DEFAULT_RTMP_NAME;
    }
    if (impl->config.queue_capacity <= 0) {
        impl->config.queue_capacity = DEFAULT_RTMP_QUEUE_CAPACITY;
    }
    if (impl->config.reconnect_interval_ms <= 0) {
        impl->config.reconnect_interval_ms = DEFAULT_RTMP_RECONNECT_INTERVAL_MS;
    }
    if (impl->config.connect_timeout_ms <= 0) {
        impl->config.connect_timeout_ms = DEFAULT_RTMP_CONNECT_TIMEOUT_MS;
    }
    if (!impl->config.video_codec_name) {
        impl->config.video_codec_name = "H264";
    }
    if (!impl->config.encoder_name) {
        impl->config.encoder_name = "RKMediaGateway";
    }

    memset(&output_config, 0, sizeof(output_config));
    output_config.name = impl->config.name;
    output_config.queue_capacity = impl->config.queue_capacity;
    output_config.reconnect_interval_ms = impl->config.reconnect_interval_ms;
    output_config.drop_until_keyframe_after_reconnect = 1;

    if (media_output_init(output, &output_config, &vtable, impl) != 0) {
        LOG_ERROR("[RTMP][ERROR] output_setup failed: media_output_init name=%s\n",
                impl->config.name ? impl->config.name : "unknown");
        free(impl);
        return -1;
    }
    output->type = MEDIA_OUTPUT_TYPE_RTMP;
    return 0;
}
