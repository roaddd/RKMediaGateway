#include "mediaPacket.h"

#include <stdlib.h>
#include <string.h>

/**
 * @description: 创建媒体缓冲区并拷贝输入数据
 * @param {const uint8_t *} data
 * @param {size_t} size
 * @param {MediaBuffer **} out_buffer
 * @return {int}
 */
int media_buffer_create_copy(const uint8_t *data, size_t size, MediaBuffer **out_buffer) {
    MediaBuffer *buffer;

    if (!data || size == 0 || !out_buffer) {
        return -1;
    }

    buffer = (MediaBuffer *)calloc(1, sizeof(*buffer));
    if (!buffer) {
        return -1;
    }
    /* TODO:这里每帧都要malloc吗 */
    buffer->data = (uint8_t *)malloc(size);
    if (!buffer->data) {
        free(buffer);
        return -1;
    }

    memcpy(buffer->data, data, size);
    buffer->size = size;
    /* 初始引用属于当前生产者；后续每入一个 sink 队列会额外 +1。 */
    buffer->ref_count = 1;
    pthread_mutex_init(&buffer->lock, NULL);
    *out_buffer = buffer;
    return 0;
}

/**
 * @description: 增加媒体缓冲区引用计数
 * @param {MediaBuffer *} buffer
 * @return {void}
 */
void media_buffer_retain(MediaBuffer *buffer) {
    if (!buffer) {
        return;
    }

    pthread_mutex_lock(&buffer->lock);
    buffer->ref_count++;
    pthread_mutex_unlock(&buffer->lock);
}

/**
 * @description: 减少媒体缓冲区引用计数并在归零时释放资源
 * @param {MediaBuffer *} buffer
 * @return {void}
 */
void media_buffer_release(MediaBuffer *buffer) {
    int should_free = 0;

    if (!buffer) {
        return;
    }

    pthread_mutex_lock(&buffer->lock);
    buffer->ref_count--;
    should_free = (buffer->ref_count == 0);
    pthread_mutex_unlock(&buffer->lock);

    if (!should_free) {
        return;
    }

    pthread_mutex_destroy(&buffer->lock);
    free(buffer->data);
    free(buffer);
}

/**
 * @description: 初始化媒体包结构体
 * @param {MediaPacket *} packet
 * @return {void}
 */
void media_packet_init(MediaPacket *packet) {
    if (!packet) {
        return;
    }
    memset(packet, 0, sizeof(*packet));
}

/**
 * @description: 复制媒体包元数据并共享底层缓冲区引用
 * @param {MediaPacket *} dst
 * @param {const MediaPacket *} src
 * @return {void}
 */
void media_packet_copy_ref(MediaPacket *dst, const MediaPacket *src) {
    if (!dst || !src) {
        return;
    }

    *dst = *src;
    if (dst->buffer) {
        /* 这里只复制包头，底层 buffer 通过引用计数共享。 */
        media_buffer_retain(dst->buffer);
    }
}

/**
 * @description: 重置媒体包并释放其持有的缓冲区引用
 * @param {MediaPacket *} packet
 * @return {void}
 */
void media_packet_reset(MediaPacket *packet) {
    if (!packet) {
        return;
    }

    if (packet->buffer) {
        /* 每个持有该 buffer 的 packet 在 reset 时都会释放自己那一份引用。 */
        media_buffer_release(packet->buffer);
        packet->buffer = NULL;
    }
    memset(packet, 0, sizeof(*packet));
}

