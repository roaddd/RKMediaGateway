#ifndef __MEDIA_PACKET_H__
#define __MEDIA_PACKET_H__

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_FRAME_TYPE_VIDEO = 0, /* 视频帧 */
    MEDIA_FRAME_TYPE_AUDIO = 1  /* 音频帧 */
} MediaFrameType;

typedef enum {
    MEDIA_CODEC_NONE = 0, /* 未指定编码类型 */
    MEDIA_CODEC_H264 = 1, /* H264 视频编码 */
    MEDIA_CODEC_AAC = 2   /* AAC 音频编码 */
} MediaCodecType;

typedef struct {
    uint8_t *data;        /* 实际媒体数据起始地址 */
    size_t size;          /* 缓冲区字节数 */
    int ref_count;        /* 引用计数，用于共享底层数据 */
    pthread_mutex_t lock; /* 保护引用计数的互斥锁 */
} MediaBuffer;

typedef struct {
    MediaFrameType frame_type; /* 帧类型，区分音视频 */
    MediaCodecType codec;      /* 当前帧使用的编码格式 */
    MediaBuffer *buffer;       /* 指向实际负载数据的共享缓冲区 */
    uint64_t frame_id;         /* 帧序号，便于日志和排查问题 */
    uint64_t pts_us;           /* 显示时间戳，单位微秒 */
    uint64_t dts_us;           /* 解码时间戳，单位微秒 */
    int is_key_frame;          /* 是否为关键帧，便于丢帧和重连恢复 */
} MediaPacket;

int media_buffer_create_copy(const uint8_t *data, size_t size, MediaBuffer **out_buffer);
void media_buffer_retain(MediaBuffer *buffer);
void media_buffer_release(MediaBuffer *buffer);
void media_packet_init(MediaPacket *packet);
void media_packet_copy_ref(MediaPacket *dst, const MediaPacket *src);
void media_packet_reset(MediaPacket *packet);

#ifdef __cplusplus
}
#endif

#endif
