#include "rtmpSink.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ENABLE_RTMP_LIBRTMP)
#include "librtmp/rtmp.h"
#include "librtmp/amf.h"
#endif

#define RTMP_VIDEO_CHANNEL 0x04
#define RTMP_INFO_CHANNEL 0x03
#define FLV_VIDEO_CODEC_AVC 7
#define FLV_FRAME_KEY 1
#define FLV_FRAME_INTER 2
#define FLV_AVC_SEQ_HEADER 0
#define FLV_AVC_NALU 1
#define DEFAULT_RTMP_NAME "rtmp"
#define DEFAULT_RTMP_QUEUE_CAPACITY 64
#define DEFAULT_RTMP_RECONNECT_INTERVAL_MS 1000
#define DEFAULT_RTMP_CONNECT_TIMEOUT_MS 3000

typedef struct {
    uint8_t *data; /* 指向去掉 Annex-B 起始码后的单个 NALU 负载视图，不拥有底层内存。 */
    size_t size;   /* 当前 NALU 负载长度。 */
} NaluView;

typedef struct {
    RtmpSinkConfig config;    /* RTMP sink 配置副本，避免依赖外部配置对象生命周期。 */
    int connected;            /* 当前是否已经完成 RTMP 连接并进入可发送状态。 */
    int metadata_sent;        /* onMetaData 是否已经在本次会话中发送过。 */
    int sequence_header_sent; /* AVC sequence header 是否已经在本次会话中发送过。 */
    uint32_t stream_id;       /* librtmp 返回的 stream id，用于组装发送包。 */
    uint32_t last_rtmp_ts_ms; /* 最近一次成功发送的视频时间戳，便于日志排查。 */
    uint8_t *sps;             /* 缓存的 SPS 数据，用于 sequence header 和重连恢复。 */
    size_t sps_len;           /* SPS 数据长度。 */
    uint8_t *pps;             /* 缓存的 PPS 数据，用于 sequence header 和重连恢复。 */
    size_t pps_len;           /* PPS 数据长度。 */
#if defined(ENABLE_RTMP_LIBRTMP)
    RTMP *rtmp;               /* librtmp 连接句柄。 */
#endif
} RtmpSinkImpl;

/**
 * @description: 返回 RTMP 帧类型描述字符串
 * @param {int} is_key_frame
 * @return {static const char *}
 */
static const char *rtmp_frame_kind(int is_key_frame) {
    return is_key_frame ? "key" : "inter";
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
    size_t i;

    /* 在 Annex-B 字节流中继续查找下一个 3 字节或 4 字节起始码。 */
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

/**
 * @description: 按大端序写入 32 位整数
 * @param {uint8_t *} dst
 * @param {uint32_t} value
 * @return {static void}
 */
static void write_be32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)((value >> 24) & 0xFF);
    dst[1] = (uint8_t)((value >> 16) & 0xFF);
    dst[2] = (uint8_t)((value >> 8) & 0xFF);
    dst[3] = (uint8_t)(value & 0xFF);
}

/**
 * @description: 按大端序写入 16 位整数
 * @param {uint8_t *} dst
 * @param {uint16_t} value
 * @return {static void}
 */
static void write_be16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)((value >> 8) & 0xFF);
    dst[1] = (uint8_t)(value & 0xFF);
}

#if defined(ENABLE_RTMP_LIBRTMP)
/**
 * @description: 按 AMF0 格式写入 double 数值
 * @param {uint8_t *} dst
 * @param {double} value
 * @return {static void}
 */
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

/**
 * @description: 按 AMF0 格式写入字符串值
 * @param {uint8_t *} dst
 * @param {const char *} str
 * @return {static uint8_t *}
 */
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

/**
 * @description: 按 AMF0 格式写入命名数值字段
 * @param {uint8_t *} dst
 * @param {const char *} name
 * @param {double} value
 * @return {static uint8_t *}
 */
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

/**
 * @description: 按 AMF0 格式写入命名布尔字段
 * @param {uint8_t *} dst
 * @param {const char *} name
 * @param {int} value
 * @return {static uint8_t *}
 */
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

/**
 * @description: 按 AMF0 格式写入命名字符串字段
 * @param {uint8_t *} dst
 * @param {const char *} name
 * @param {const char *} value
 * @return {static uint8_t *}
 */
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

/**
 * @description: 写入 AMF 对象结束标记
 * @param {uint8_t *} dst
 * @return {static uint8_t *}
 */
static uint8_t *amf_write_object_end(uint8_t *dst) {
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = AMF_OBJECT_END;
    return dst + 3;
}
#endif

/**
 * @description: 释放缓存的 SPS 或 PPS 数据
 * @param {uint8_t **} data
 * @param {size_t *} size
 * @return {static void}
 */
static void free_parameter_set(uint8_t **data, size_t *size) {
    if (*data) {
        free(*data);
        *data = NULL;
    }
    *size = 0;
}

/**
 * @description: 更新缓存的 SPS 或 PPS 数据
 * @param {uint8_t **} dst
 * @param {size_t *} dst_len
 * @param {const uint8_t *} src
 * @param {size_t} src_len
 * @param {int *} changed
 * @return {static int}
 */
static int update_parameter_set(uint8_t **dst, size_t *dst_len, const uint8_t *src, size_t src_len, int *changed) {
    uint8_t *copy;

    if (!dst || !dst_len || !src || src_len == 0) {
        fprintf(stderr, "[RTMP][ERROR] update_parameter_set invalid args\n");
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
        fprintf(stderr, "[RTMP][ERROR] update_parameter_set alloc failed size=%zu\n", src_len);
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

/**
 * @description: 将 Annex-B 码流拆分为多个 NALU 视图
 * @param {const uint8_t *} data
 * @param {size_t} len
 * @param {NaluView **} out_nalus
 * @param {size_t *} out_count
 * @return {static int}
 */
static int annexb_split_nalus(const uint8_t *data, size_t len, NaluView **out_nalus, size_t *out_count) {
    size_t pos = 0;
    size_t first = 0;
    int code_len = 0;
    NaluView *nalus = NULL;
    size_t count = 0;
    size_t capacity = 0;

    if (!data || len == 0 || !out_nalus || !out_count) {
        fprintf(stderr, "[RTMP][ERROR] annexb_split_nalus invalid args len=%zu\n", len);
        return -1;
    }

    /* 某些编码器可能直接输出单个裸 NALU，没有起始码，这里也兼容这种情况。 */
    if (find_start_code(data, len, 0, &first, &code_len) != 0) {
        nalus = (NaluView *)calloc(1, sizeof(*nalus));
        if (!nalus) {
            fprintf(stderr, "[RTMP][ERROR] annexb_split_nalus alloc fallback nalu failed\n");
            return -1;
        }
        nalus[0].data = (uint8_t *)data;
        nalus[0].size = len;
        *out_nalus = nalus;
        *out_count = 1;
        return 0;
    }

    /* 将一帧 Annex-B 数据拆成多个 NALU 片段视图，避免重复拷贝负载数据。 */
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
                    fprintf(stderr, "[RTMP][ERROR] annexb_split_nalus realloc failed capacity=%zu\n", capacity);
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

/**
 * @description: 从 NALU 列表中缓存 SPS 和 PPS 参数集
 * @param {RtmpSinkImpl *} impl
 * @param {const NaluView *} nalus
 * @param {size_t} count
 * @return {static int}
 */
static int rtmp_cache_parameter_sets(RtmpSinkImpl *impl, const NaluView *nalus, size_t count) {
    size_t i;
    int changed = 0;

    for (i = 0; i < count; ++i) {
        uint8_t nalu_type;

        if (!nalus[i].data || nalus[i].size == 0) {
            continue;
        }
        nalu_type = (uint8_t)(nalus[i].data[0] & 0x1F);
        /* SPS/PPS 可能会随着 IDR 帧重复出现，只有内容真正变化时才刷新缓存。 */
        if (nalu_type == 7) {
            if (update_parameter_set(&impl->sps, &impl->sps_len, nalus[i].data, nalus[i].size, &changed) != 0) {
                fprintf(stderr, "[RTMP][ERROR] cache SPS failed size=%zu\n", nalus[i].size);
                return -1;
            }
            if (changed) {
                impl->sequence_header_sent = 0;
            }
        } else if (nalu_type == 8) {
            if (update_parameter_set(&impl->pps, &impl->pps_len, nalus[i].data, nalus[i].size, &changed) != 0) {
                fprintf(stderr, "[RTMP][ERROR] cache PPS failed size=%zu\n", nalus[i].size);
                return -1;
            }
            if (changed) {
                impl->sequence_header_sent = 0;
            }
        }
    }
    return 0;
}

#if defined(ENABLE_RTMP_LIBRTMP)
/**
 * @description: 发送 RTMP 信息消息体
 * @param {RtmpSinkImpl *} impl
 * @param {const uint8_t *} body
 * @param {size_t} body_size
 * @param {uint32_t} timestamp_ms
 * @return {static int}
 */
static int rtmp_send_info_body(RtmpSinkImpl *impl, const uint8_t *body, size_t body_size, uint32_t timestamp_ms) {
    RTMPPacket packet;
    int ret;

    if (!impl || !impl->rtmp || !body || body_size == 0) {
        fprintf(stderr, "[RTMP][ERROR] send_info_body invalid args body_size=%zu\n", body_size);
        return -1;
    }

    RTMPPacket_Reset(&packet);
    if (!RTMPPacket_Alloc(&packet, (int)body_size)) {
        fprintf(stderr, "[RTMP][ERROR] send_info_body packet alloc failed size=%zu\n", body_size);
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
        fprintf(stderr, "[RTMP] event=send_info_failed stream_id=%u body_size=%zu ts_ms=%u\n",
                impl->stream_id,
                body_size,
                timestamp_ms);
    }
    return ret ? 0 : -1;
}

/**
 * @description: 发送 RTMP 视频消息体
 * @param {RtmpSinkImpl *} impl
 * @param {const uint8_t *} body
 * @param {size_t} body_size
 * @param {uint32_t} timestamp_ms
 * @return {static int}
 */
static int rtmp_send_video_body(RtmpSinkImpl *impl, const uint8_t *body, size_t body_size, uint32_t timestamp_ms) {
    RTMPPacket packet;
    int ret;

    if (!impl || !impl->rtmp || !body || body_size == 0) {
        fprintf(stderr, "[RTMP][ERROR] send_video_body invalid args body_size=%zu\n", body_size);
        return -1;
    }

    RTMPPacket_Reset(&packet);
    if (!RTMPPacket_Alloc(&packet, (int)body_size)) {
        fprintf(stderr, "[RTMP][ERROR] send_video_body packet alloc failed size=%zu\n", body_size);
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
        fprintf(stderr, "[RTMP] event=send_video_failed stream_id=%u body_size=%zu ts_ms=%u\n",
                impl->stream_id,
                body_size,
                timestamp_ms);
    }
    return ret ? 0 : -1;
}

/**
 * @description: 发送 RTMP onMetaData 元数据消息
 * @param {RtmpSinkImpl *} impl
 * @return {static int}
 */
static int rtmp_send_on_metadata(RtmpSinkImpl *impl) {
    uint8_t body[512];
    uint8_t *p = body;

    if (!impl) {
        fprintf(stderr, "[RTMP][ERROR] send_on_metadata impl is NULL\n");
        return -1;
    }

    /* 在 AVC 解码配置和视频帧之前先发送标准 AMF0 onMetaData 消息。
     * 很多 RTMP 服务端和播放器会依赖这些字段做流信息展示、解码器预热，
     * 或在控制台中显示分辨率、帧率、码率等运行信息。
     */
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
        fprintf(stderr, "[RTMP][ERROR] send_on_metadata failed\n");
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

/**
 * @description: 发送 AVC Sequence Header
 * @param {RtmpSinkImpl *} impl
 * @param {uint32_t} timestamp_ms
 * @return {static int}
 */
static int rtmp_send_avc_sequence_header(RtmpSinkImpl *impl, uint32_t timestamp_ms) {
    uint8_t *body;
    size_t body_size;
    size_t offset = 0;

    if (!impl || !impl->sps || !impl->pps || impl->sps_len < 4 || impl->pps_len == 0) {
        fprintf(stderr, "[RTMP][ERROR] send_avc_sequence_header SPS/PPS not ready sps=%zu pps=%zu\n",
                impl ? impl->sps_len : 0,
                impl ? impl->pps_len : 0);
        return -1;
    }

    body_size = 5 + 6 + 2 + impl->sps_len + 1 + 2 + impl->pps_len;
    body = (uint8_t *)malloc(body_size);
    if (!body) {
        fprintf(stderr, "[RTMP][ERROR] send_avc_sequence_header alloc failed size=%zu\n", body_size);
        return -1;
    }

    /* FLV 的 AVC sequence header 内部承载 AVCDecoderConfigurationRecord，
     * 包含版本、profile、compatibility、level 以及原始 SPS/PPS 内容。
     * RTMP 接收端必须先拿到这段信息，后续才能正确解码 AVCPacketType=1 的视频负载。
     */
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
        fprintf(stderr, "[RTMP][ERROR] send_avc_sequence_header size mismatch offset=%zu body_size=%zu\n",
                offset, body_size);
        free(body);
        return -1;
    }

    if (rtmp_send_video_body(impl, body, body_size, timestamp_ms) != 0) {
        fprintf(stderr, "[RTMP][ERROR] send_avc_sequence_header send failed ts_ms=%u\n", timestamp_ms);
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

/**
 * @description: 发送 RTMP AVC 视频负载
 * @param {RtmpSinkImpl *} impl
 * @param {const NaluView *} nalus
 * @param {size_t} count
 * @param {uint32_t} timestamp_ms
 * @param {int} is_key_frame
 * @return {static int}
 */
static int rtmp_send_avc_nalus(RtmpSinkImpl *impl,
                               const NaluView *nalus,
                               size_t count,
                               uint32_t timestamp_ms,
                               int is_key_frame) {
    uint8_t *body;
    size_t body_size = 5;
    size_t offset = 0;
    size_t i;

    /* 把 Annex-B 帧负载转换成 FLV/AVC 负载格式：
     * 每个媒体 NALU 会被编码成 [4 字节大端长度][nalu 数据]。
     * SPS/PPS/AUD 不再重复写入，因为它们已经在 sequence header 中单独发送过。
     */
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
        fprintf(stderr, "[RTMP][ERROR] send_avc_nalus alloc failed size=%zu\n", body_size);
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
        fprintf(stderr, "[RTMP][ERROR] send_avc_nalus size mismatch offset=%zu body_size=%zu\n",
                offset, body_size);
        free(body);
        return -1;
    }

    if (rtmp_send_video_body(impl, body, body_size, timestamp_ms) != 0) {
        fprintf(stderr, "[RTMP][ERROR] send_avc_nalus send failed ts_ms=%u\n", timestamp_ms);
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
#endif

/**
 * @description: 将媒体包时间戳转换为毫秒
 * @param {const MediaPacket *} packet
 * @return {static uint32_t}
 */
static uint32_t packet_timestamp_ms(const MediaPacket *packet) {
    uint64_t ts_us;

    if (!packet) {
        return 0;
    }
    ts_us = packet->dts_us ? packet->dts_us : packet->pts_us;
    return (uint32_t)(ts_us / 1000ULL);
}

/**
 * @description: 启动 RTMP 推流通道
 * @param {MediaSink *} sink
 * @return {static int}
 */
static int rtmp_sink_start(MediaSink *sink) {
    RtmpSinkImpl *impl = (RtmpSinkImpl *)sink->impl;

    /* start() 只检查静态配置是否合法；真正建立网络连接放在 connect() 中。
     * 这样断线重连时可以复用同一套状态机，而不必重建整个 sink 对象。
     */
    if (!impl->config.publish_url || impl->config.publish_url[0] == '\0') {
        fprintf(stderr, "[WARN] RTMP sink disabled: publish_url is empty\n");
        return -1;
    }

    printf("[INFO] RTMP sink configured: %s\n", impl->config.publish_url);
    printf("[INFO] RTMP audio path reserved, current audio_enabled=%d\n", impl->config.audio_enabled);
    return 0;
}

/**
 * @description: 建立 RTMP 推流连接
 * @param {MediaSink *} sink
 * @return {static int}
 */
static int rtmp_sink_connect(MediaSink *sink) {
    RtmpSinkImpl *impl = (RtmpSinkImpl *)sink->impl;

#if defined(ENABLE_RTMP_LIBRTMP)
    /* 每次重连都重新创建一个全新的 RTMP 会话。
     * 这样恢复逻辑更简单，也能确保服务端状态从一次完整握手开始。
     */
    if (!impl) {
        fprintf(stderr, "[RTMP][ERROR] connect failed: impl is NULL\n");
        return -1;
    }
    if (impl->rtmp) {
        RTMP_Close(impl->rtmp);
        RTMP_Free(impl->rtmp);
        impl->rtmp = NULL;
    }

    impl->rtmp = RTMP_Alloc();
    if (!impl->rtmp) {
        fprintf(stderr, "[RTMP][ERROR] connect failed: RTMP_Alloc\n");
        return -1;
    }
    RTMP_Init(impl->rtmp);
    impl->rtmp->Link.timeout = (impl->config.connect_timeout_ms > 0)
        ? ((impl->config.connect_timeout_ms + 999) / 1000)
        : 3;

    if (!RTMP_SetupURL(impl->rtmp, (char *)impl->config.publish_url)) {
        fprintf(stderr, "[RTMP] event=setup_url_failed url=%s\n", impl->config.publish_url);
        RTMP_Free(impl->rtmp);
        impl->rtmp = NULL;
        return -1;
    }
    RTMP_EnableWrite(impl->rtmp);
    RTMP_SetBufferMS(impl->rtmp, 0);

    if (!RTMP_Connect(impl->rtmp, NULL)) {
        fprintf(stderr, "[RTMP] event=connect_failed url=%s timeout_s=%d\n",
                impl->config.publish_url,
                impl->rtmp->Link.timeout);
        RTMP_Close(impl->rtmp);
        RTMP_Free(impl->rtmp);
        impl->rtmp = NULL;
        return -1;
    }
    if (!RTMP_ConnectStream(impl->rtmp, 0)) {
        fprintf(stderr, "[RTMP] event=connect_stream_failed url=%s\n", impl->config.publish_url);
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
    (void)sink;
    fprintf(stderr, "[WARN] RTMP sink requested but librtmp support is not compiled in\n");
    return -1;
#endif
}

/**
 * @description: 发送一个媒体包到 RTMP 通道
 * @param {MediaSink *} sink
 * @param {const MediaPacket *} packet
 * @return {static int}
 */
static int rtmp_sink_send_packet(MediaSink *sink, const MediaPacket *packet) {
    RtmpSinkImpl *impl = (RtmpSinkImpl *)sink->impl;
    NaluView *nalus = NULL;
    size_t nalu_count = 0;
    uint32_t timestamp_ms;
    int ret = -1;

    if (!impl || !impl->connected || !packet || !packet->buffer) {
        fprintf(stderr, "[RTMP][ERROR] send_packet invalid args connected=%d packet=%p buffer=%p\n",
                (impl && impl->connected) ? 1 : 0,
                (void *)packet,
                (void *)(packet ? packet->buffer : NULL));
        return -1;
    }
    if (packet->frame_type != MEDIA_FRAME_TYPE_VIDEO || packet->codec != MEDIA_CODEC_H264) {
        return 0;
    }

#if defined(ENABLE_RTMP_LIBRTMP)
    /* RTMP sink 直接接收编码器输出的 Annex-B 数据，并在本地完成 RTMP/FLV 封装转换，
     * 这样不会影响其他 sink 的输入格式和处理逻辑。
     */
    if (annexb_split_nalus(packet->buffer->data, packet->buffer->size, &nalus, &nalu_count) != 0) {
        fprintf(stderr, "[RTMP][ERROR] send_packet split annexb failed frame=%" PRIu64 " size=%zu\n",
                packet->frame_id,
                packet->buffer->size);
        return -1;
    }
    if (rtmp_cache_parameter_sets(impl, nalus, nalu_count) != 0) {
        goto done;
    }

    timestamp_ms = packet_timestamp_ms(packet);
    /* onMetaData 只需要在每次 RTMP 会话建立后发送一次，并且要早于媒体头和视频帧。 */
    if (!impl->metadata_sent) {
        if (rtmp_send_on_metadata(impl) != 0) {
            fprintf(stderr, "[RTMP] event=metadata_send_failed frame=%" PRIu64 "\n", packet->frame_id);
            goto done;
        }
    }
    if (!impl->sequence_header_sent) {
        if (!impl->sps || !impl->pps) {
            /* 如果当前还没有拿到 SPS/PPS，就先跳过该帧，等待后续关键帧补齐参数集。 */
            fprintf(stderr, "[WARN] RTMP skip frame=%" PRIu64 " because SPS/PPS not ready\n", packet->frame_id);
            ret = 0;
            goto done;
        }
        if (rtmp_send_avc_sequence_header(impl, timestamp_ms) != 0) {
            fprintf(stderr, "[RTMP] event=sequence_header_send_failed frame=%" PRIu64 "\n", packet->frame_id);
            goto done;
        }
    }

    ret = rtmp_send_avc_nalus(impl, nalus, nalu_count, timestamp_ms, packet->is_key_frame);
    if (ret == 0) {
        impl->last_rtmp_ts_ms = timestamp_ms;
    } else {
        fprintf(stderr, "[RTMP] event=video_payload_send_failed frame=%" PRIu64 " frame_type=%s ts_ms=%u\n",
                packet->frame_id,
                rtmp_frame_kind(packet->is_key_frame),
                timestamp_ms);
    }
done:
    free(nalus);
    return ret;
#else
    (void)sink;
    (void)packet;
    fprintf(stderr, "[RTMP][ERROR] send_packet failed: librtmp support is not compiled in\n");
    return -1;
#endif
}

/**
 * @description: 断开 RTMP 推流连接
 * @param {MediaSink *} sink
 * @return {static void}
 */
static void rtmp_sink_disconnect(MediaSink *sink) {
    RtmpSinkImpl *impl = (RtmpSinkImpl *)sink->impl;

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

/**
 * @description: 停止 RTMP 推流通道
 * @param {MediaSink *} sink
 * @return {static void}
 */
static void rtmp_sink_stop(MediaSink *sink) {
    RtmpSinkImpl *impl = (RtmpSinkImpl *)sink->impl;

    if (!impl) {
        return;
    }

    rtmp_sink_disconnect(sink);
    free_parameter_set(&impl->sps, &impl->sps_len);
    free_parameter_set(&impl->pps, &impl->pps_len);
}

/**
 * @description: 根据配置创建 RTMP 推流通道
 * @param {MediaSink *} sink
 * @param {const RtmpSinkConfig *} config
 * @return {int}
 */
int rtmp_sink_setup(MediaSink *sink, const RtmpSinkConfig *config) {
    static const MediaSinkVTable vtable = {
        rtmp_sink_start,
        rtmp_sink_connect,
        rtmp_sink_send_packet,
        rtmp_sink_disconnect,
        rtmp_sink_stop
    };
    MediaSinkConfig sink_config;
    RtmpSinkImpl *impl;

    if (!sink) {
        fprintf(stderr, "[RTMP][ERROR] sink_setup failed: sink is NULL\n");
        return -1;
    }

    impl = (RtmpSinkImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        fprintf(stderr, "[RTMP][ERROR] sink_setup failed: impl alloc\n");
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

    memset(&sink_config, 0, sizeof(sink_config));
    sink_config.name = impl->config.name;
    sink_config.queue_capacity = impl->config.queue_capacity;
    sink_config.reconnect_interval_ms = impl->config.reconnect_interval_ms;
    sink_config.drop_until_keyframe_after_reconnect = 1;

    if (media_sink_init(sink, &sink_config, &vtable, impl) != 0) {
        fprintf(stderr, "[RTMP][ERROR] sink_setup failed: media_sink_init name=%s\n",
                impl->config.name ? impl->config.name : "unknown");
        free(impl);
        return -1;
    }
    return 0;
}
