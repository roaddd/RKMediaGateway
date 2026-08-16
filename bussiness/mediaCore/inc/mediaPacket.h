#ifndef __MEDIA_PACKET_H__
#define __MEDIA_PACKET_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_FRAME_TYPE_VIDEO = 0, /* 视频帧 */
    MEDIA_FRAME_TYPE_AUDIO = 1  /* 音频帧 */
} MediaFrameType;

typedef enum {
    MEDIA_CODEC_NONE = 0,      /* 未指定编码类型 */
    MEDIA_CODEC_H264 = 1,      /* H264 视频编码 */
    MEDIA_CODEC_AAC = 2,       /* AAC 音频编码 */
    MEDIA_CODEC_PCM_S16LE = 3, /* PCM S16LE 原始音频 */
    MEDIA_CODEC_G711A = 4,     /* G.711 A-law 音频 */
    MEDIA_CODEC_G711U = 5,     /* G.711 mu-law 音频 */
    MEDIA_CODEC_OPUS = 6       /* Opus 音频编码 */
} MediaCodecType;

typedef struct MediaBuffer {
    uint8_t *data;        /* 实际媒体数据起始地址 */
    size_t size;          /* 缓冲区字节数 */
    size_t capacity;      /* 当前 data 已分配容量，用于池化复用 */
    int ref_count;        /* 引用计数，通过原子操作更新 */
    struct MediaBuffer *pool_next; /* buffer 池空闲链表指针 */
} MediaBuffer;

typedef struct {
    uint64_t enqueue_ts_us;    /* 进入输出队列的时间戳，单位微秒，用于统计队列等待。 */
    uint64_t dqbuf_to_encode_start_us; /* DQBUF 返回到开始送入编码器的耗时，单位微秒。 */
    uint64_t encode_us;        /* 编码阶段总耗时，单位微秒，用于路径延时日志。 */
    const char *stream_name;   /* 所属码流名称，仅引用配置中的静态字符串。 */
    int sample;                /* 是否打印本包的路径延时采样日志。 */
    uint64_t audio_capture_done_us;         /* ALSA period 采集完成时刻。 */
    uint64_t audio_capture_interval_us;     /* 相邻 period 实际采集完成间隔。 */
    uint64_t audio_capture_read_us;         /* 本 period 的 ALSA read 总耗时。 */
    uint64_t audio_source_publish_us;       /* 发布到音频帧源 ring 的时刻。 */
    uint64_t audio_source_acquire_us;       /* gateway 从帧源 ring 取出的时刻。 */
    uint64_t audio_encode_queue_enqueue_us; /* 进入音频编码 FIFO 的时刻。 */
    uint64_t audio_encode_queue_dequeue_us; /* 编码线程从音频 FIFO 取出的时刻。 */
    uint64_t audio_encode_start_us;         /* 音频编码调用开始时刻。 */
    uint64_t audio_encode_done_us;          /* 音频编码调用完成时刻。 */
} MediaPacketPathMetrics;

typedef struct {
    MediaFrameType frame_type; /* 帧类型，区分音视频 */
    MediaCodecType codec;      /* 当前帧使用的编码格式 */
    MediaBuffer *buffer;       /* 指向实际负载数据的共享缓冲区 */
    uint64_t frame_id;         /* 帧序号，便于日志和排查问题 */
    uint64_t pts_us;           /* 显示时间戳，单位微秒 */
    uint64_t dts_us;           /* 解码时间戳，单位微秒 */
    MediaPacketPathMetrics path_metrics; /* 路径延时采样信息；不影响媒体包核心语义。 */
    int is_key_frame;          /* 是否为关键帧，便于丢帧和重连恢复 */
} MediaPacket;

/**
 * @description: 创建媒体缓冲区，并把输入数据拷贝到新分配的缓冲区中。
 * @param {const uint8_t *} data 输入媒体数据地址。
 * @param {size_t} size 输入媒体数据字节数。
 * @param {MediaBuffer **} out_buffer 输出创建成功的缓冲区，初始引用计数为 1。
 * @return {int} 0 成功，-1 失败。
 */
int media_buffer_create_copy(const uint8_t *data, size_t size, MediaBuffer **out_buffer);

/**
 * @description: 增加媒体缓冲区引用计数，用于多个 MediaPacket 共享同一份数据。
 * @param {MediaBuffer *} buffer 需要增加引用的缓冲区，允许为 NULL。
 * @return {void}
 */
void media_buffer_retain(MediaBuffer *buffer);

/**
 * @description: 减少媒体缓冲区引用计数，引用归零时释放数据和缓冲区对象。
 * @param {MediaBuffer *} buffer 需要释放引用的缓冲区，允许为 NULL。
 * @return {void}
 */
void media_buffer_release(MediaBuffer *buffer);

/**
 * @description: 初始化媒体包结构体，把所有字段清零。
 * @param {MediaPacket *} packet 需要初始化的媒体包，允许为 NULL。
 * @return {void}
 */
void media_packet_init(MediaPacket *packet);

/**
 * @description: 复制媒体包元数据，并增加底层 MediaBuffer 的引用计数。
 * @param {MediaPacket *} dst 目标媒体包。
 * @param {const MediaPacket *} src 源媒体包。
 * @return {void}
 */
void media_packet_copy_ref(MediaPacket *dst, const MediaPacket *src);

/**
 * @description: 重置媒体包，并释放其持有的 MediaBuffer 引用。
 * @param {MediaPacket *} packet 需要重置的媒体包，允许为 NULL。
 * @return {void}
 */
void media_packet_reset(MediaPacket *packet);

#ifdef __cplusplus
}
#endif

#endif
