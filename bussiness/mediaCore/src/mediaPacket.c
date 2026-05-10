#include "mediaPacket.h"

#include "logger.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MEDIA_BUFFER_POOL_PREALLOC 128
#define MEDIA_BUFFER_POOL_MAX_FREE 256
#define MEDIA_BUFFER_POOL_MAX_CACHED_CAPACITY (2 * 1024 * 1024)

/*
 * MediaBuffer 池只复用 MediaBuffer 对象和其 data 容量，不改变对外生命周期语义：
 * create_copy 返回 ref_count=1，所有 MediaPacket 引用 reset 后 ref_count 归零，
 * buffer 才会回到池中等待下一帧复用。
 */
static pthread_mutex_t g_media_buffer_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static MediaBuffer *g_media_buffer_pool = NULL;
static int g_media_buffer_pool_count = 0;
static int g_media_buffer_pool_inited = 0;

/**
 * @description: 首次使用时预分配一批空 MediaBuffer 结构体。
 * @note 调用方必须持有 g_media_buffer_pool_lock。
 */
static void media_buffer_pool_prealloc_locked(void)
{
    int i;
    if (g_media_buffer_pool_inited)
        return;
    /* 预分配一批空 MediaBuffer 结构体,默认128个 */
    for (i = 0; i < MEDIA_BUFFER_POOL_PREALLOC; ++i)
    {
        /* 只分配 MediaBuffer 结构体，不分配 data payload。真正的数据内存等第一次需要具体大小时再 realloc */
        MediaBuffer *buffer = (MediaBuffer *)calloc(1, sizeof(*buffer));
        if (!buffer)
            break;
        buffer->pool_next = g_media_buffer_pool;
        g_media_buffer_pool = buffer;
        g_media_buffer_pool_count++;
    }
    g_media_buffer_pool_inited = 1;
}

/**
 * @description: 从池中取出一个容量足够的 buffer；没有足够容量时退而取任意空闲 buffer。
 * @param need_size 本次负载需要的最小 data 容量。
 * @return 可复用的 MediaBuffer；池为空时返回 NULL。
 * @note 调用方必须持有 g_media_buffer_pool_lock。
 */
static MediaBuffer *media_buffer_pool_take_locked(size_t need_size)
{
    MediaBuffer *prev = NULL;
    MediaBuffer *cur = g_media_buffer_pool;

    while (cur)
    {
        /* TODO:这里只要找到大于need_size的buffer就可以，有必要优化成最接近need_size的buffer吗 */
        if (cur->capacity >= need_size)
        {
            if (prev)
                prev->pool_next = cur->pool_next;
            else
                g_media_buffer_pool = cur->pool_next;
            cur->pool_next = NULL;
            g_media_buffer_pool_count--;
            return cur;
        }
        prev = cur;
        cur = cur->pool_next;
    }

    cur = g_media_buffer_pool;
    if (cur)
    {
        g_media_buffer_pool = cur->pool_next;
        cur->pool_next = NULL;
        g_media_buffer_pool_count--;
        return cur;
    }
    return NULL;
}

/**
 * @description: 获取一个 MediaBuffer，优先从池中复用，池为空时再分配结构体。
 * @param need_size 本次负载需要的最小 data 容量。
 * @return 可写入的 MediaBuffer；失败时返回 NULL。
 */
static MediaBuffer *media_buffer_acquire(size_t need_size)
{
    MediaBuffer *buffer;

    pthread_mutex_lock(&g_media_buffer_pool_lock);
    media_buffer_pool_prealloc_locked();
    buffer = media_buffer_pool_take_locked(need_size);
    pthread_mutex_unlock(&g_media_buffer_pool_lock);

    if (!buffer)
        buffer = (MediaBuffer *)calloc(1, sizeof(*buffer));
    return buffer;
}

/**
 * @description: 直接释放 MediaBuffer 及其 payload，不回收到池。
 * @param buffer 待释放对象。
 */
static void media_buffer_free_direct(MediaBuffer *buffer)
{
    if (!buffer)
        return;
    free(buffer->data);
    free(buffer);
}

/**
 * @description: 将引用归零的 MediaBuffer 归还到池，过大的 payload 直接释放。
 * @param buffer 引用计数已经归零的 buffer。
 */
static void media_buffer_return_to_pool(MediaBuffer *buffer)
{
    if (!buffer)
        return;

    buffer->size = 0;
    buffer->ref_count = 0;
    buffer->pool_next = NULL;

    if (buffer->capacity > MEDIA_BUFFER_POOL_MAX_CACHED_CAPACITY)
    {
        /* 大帧缓存长期留池会抬高常驻内存，超过阈值直接释放。 */
        media_buffer_free_direct(buffer);
        return;
    }

    pthread_mutex_lock(&g_media_buffer_pool_lock);
    media_buffer_pool_prealloc_locked();
    if (g_media_buffer_pool_count >= MEDIA_BUFFER_POOL_MAX_FREE)
    {
        /* 限制空闲池长度，避免短时峰值后长期占用过多内存。 */
        pthread_mutex_unlock(&g_media_buffer_pool_lock);
        media_buffer_free_direct(buffer);
        return;
    }
    buffer->pool_next = g_media_buffer_pool;
    g_media_buffer_pool = buffer;
    g_media_buffer_pool_count++;
    pthread_mutex_unlock(&g_media_buffer_pool_lock);
}

/**
 * @description: 创建媒体缓冲区并拷贝输入负载。
 * @param data 输入媒体数据地址。
 * @param size 输入媒体数据字节数。
 * @param out_buffer 输出创建成功的缓冲区，初始引用计数为 1。
 * @return 0 成功，-1 失败。
 */
int media_buffer_create_copy(const uint8_t *data, size_t size, MediaBuffer **out_buffer)
{
    MediaBuffer *buffer;
    uint8_t *new_data;

    if (!data || size == 0 || !out_buffer)
    {
        LOG_ERROR("media_buffer_create_copy failed: invalid args data=%p size=%zu out=%p",
                  (const void *)data,
                  size,
                  (void *)out_buffer);
        return -1;
    }

    /* 获取一个 MediaBuffer，优先从池中复用，池为空时再分配结构体 */
    buffer = media_buffer_acquire(size);
    if (!buffer)
    {
        LOG_ERROR("media_buffer_create_copy failed: acquire buffer size=%zu", size);
        return -1;
    }

    /* 有可能获取的buffer的size不足 */
    if (buffer->capacity < size)
    {
        /* 池中复用的 buffer 容量不足时扩容；容量足够时避免每帧 malloc/free。 */
        new_data = (uint8_t *)realloc(buffer->data, size);
        if (!new_data)
        {
            LOG_ERROR("media_buffer_create_copy failed: realloc payload old_capacity=%zu size=%zu",
                      buffer->capacity,
                      size);
            media_buffer_free_direct(buffer);
            return -1;
        }
        buffer->data = new_data;
        buffer->capacity = size;
    }

    memcpy(buffer->data, data, size);
    buffer->size = size;
    /*
     * 初始引用属于生产者栈上的 MediaPacket；每入一个输出队列会通过
     * media_packet_copy_ref() 再增加一次引用。
     */
    __atomic_store_n(&buffer->ref_count, 1, __ATOMIC_RELEASE);
    buffer->pool_next = NULL;
    *out_buffer = buffer;
    return 0;
}

/**
 * @description: 增加媒体缓冲区引用计数，用于多个 MediaPacket 共享同一份 payload。
 * @param buffer 需要增加引用的缓冲区，允许为 NULL。
 */
void media_buffer_retain(MediaBuffer *buffer)
{
    if (!buffer)
        return;
    __atomic_add_fetch(&buffer->ref_count, 1, __ATOMIC_ACQ_REL);
}

/**
 * @description: 减少媒体缓冲区引用计数，归零时回收到池或直接释放。
 * @param buffer 需要释放引用的缓冲区，允许为 NULL。
 */
void media_buffer_release(MediaBuffer *buffer)
{
    if (!buffer)
        return;
    /* 原子减引用避免每个 buffer 携带互斥锁；最后一个释放者负责回收。 */
    if (__atomic_sub_fetch(&buffer->ref_count, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    media_buffer_return_to_pool(buffer);
}

/**
 * @description: 初始化媒体包结构体，把所有字段清零。
 * @param packet 需要初始化的媒体包，允许为 NULL。
 */
void media_packet_init(MediaPacket *packet)
{
    if (!packet)
        return;
    memset(packet, 0, sizeof(*packet));
}

/**
 * @description: 复制媒体包元数据，并增加底层 MediaBuffer 引用计数。
 * @param dst 目标媒体包。
 * @param src 源媒体包。
 */
void media_packet_copy_ref(MediaPacket *dst, const MediaPacket *src)
{
    if (!dst || !src)
        return;

    *dst = *src;
    if (dst->buffer)
    {
        /* 只复制包头和共享 buffer 引用，不复制底层媒体数据。 */
        media_buffer_retain(dst->buffer);
    }
}

/**
 * @description: 重置媒体包，并释放其持有的 MediaBuffer 引用。
 * @param packet 需要重置的媒体包，允许为 NULL。
 */
void media_packet_reset(MediaPacket *packet)
{
    if (!packet)
        return;

    if (packet->buffer)
    {
        /* 每个持有该 buffer 的 packet 都必须 reset，最后一个引用触发回收。 */
        media_buffer_release(packet->buffer);
        packet->buffer = NULL;
    }
    memset(packet, 0, sizeof(*packet));
}
