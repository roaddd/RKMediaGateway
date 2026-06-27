/**
 * @file threadMessageQueue.c
 * @brief 通用线程消息队列实现。
 *
 * 队列槽位数量固定，单条消息数据按实际长度动态分配。队列支持多生产者、
 * 多消费者，但上层应根据业务拓扑约束消费者数量，避免命令被错误线程竞争。
 */

#include "threadMessageQueue.h"

#include "logger.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief 生成 pthread 条件变量等待使用的绝对超时时间。
 */
static void thread_message_make_abs_timeout(struct timespec *ts, int timeout_ms)
{
    long nsec = 0;

    clock_gettime(CLOCK_REALTIME, ts);
    if (timeout_ms < 0)
        timeout_ms = 0;
    ts->tv_sec += timeout_ms / 1000;
    nsec = ts->tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    ts->tv_sec += nsec / 1000000000L;
    ts->tv_nsec = nsec % 1000000000L;
}

/**
 * @brief 清理消息内部动态数据。
 */
void thread_message_release(ThreadMessage *message)
{
    if (!message)
    {
        LOG_ERROR("thread_message_release failed: message is NULL");
        return;
    }
    free(message->data);
    memset(message, 0, sizeof(*message));
}

/**
 * @brief 初始化固定容量环形消息队列。
 */
int thread_message_queue_init(ThreadMessageQueue *queue, size_t capacity)
{
    ThreadMessage *messages = NULL;
    int mutex_ret = 0;
    int cond_ret = 0;

    if (!queue || capacity == 0)
    {
        LOG_ERROR("thread_message_queue_init failed: invalid args queue=%p capacity=%zu",
                  (void *)queue,
                  capacity);
        return -1;
    }

    memset(queue, 0, sizeof(*queue));
    messages = (ThreadMessage *)calloc(capacity, sizeof(*messages));
    if (!messages)
    {
        LOG_ERROR("thread_message_queue_init failed: calloc messages capacity=%zu",
                  capacity);
        return -1;
    }
    mutex_ret = pthread_mutex_init(&queue->lock, NULL);
    if (mutex_ret != 0)
    {
        LOG_ERROR("thread_message_queue_init failed: pthread_mutex_init ret=%d(%s)",
                  mutex_ret,
                  strerror(mutex_ret));
        free(messages);
        return -1;
    }
    cond_ret = pthread_cond_init(&queue->cond, NULL);
    if (cond_ret != 0)
    {
        LOG_ERROR("thread_message_queue_init failed: pthread_cond_init ret=%d(%s)",
                  cond_ret,
                  strerror(cond_ret));
        pthread_mutex_destroy(&queue->lock);
        free(messages);
        return -1;
    }

    queue->messages = messages;
    queue->capacity = capacity;
    queue->running = 1;
    queue->initialized = 1;
    return 0;
}

/**
 * @brief 深拷贝消息后放入队尾，动态内存分配在加锁前完成以缩短临界区。
 */
int thread_message_queue_push_copy(ThreadMessageQueue *queue, const ThreadMessage *message)
{
    ThreadMessage copied = {0};
    size_t tail = 0;

    if (!queue || !message || !queue->initialized)
    {
        LOG_ERROR("thread_message_queue_push_copy failed: invalid args queue=%p message=%p initialized=%d",
                  (void *)queue,
                  (const void *)message,
                  queue ? queue->initialized : 0);
        return -1;
    }
    if (message->data_size > 0 && !message->data)
    {
        LOG_ERROR("thread_message_queue_push_copy failed: data is NULL type=%u request=%llu data_size=%zu",
                  message->type,
                  (unsigned long long)message->request_id,
                  message->data_size);
        return -1;
    }

    copied = *message;
    copied.data = NULL;
    if (message->data_size > 0)
    {
        copied.data = malloc(message->data_size);
        if (!copied.data)
        {
            LOG_ERROR("thread_message_queue_push_copy failed: malloc payload type=%u request=%llu data_size=%zu",
                      message->type,
                      (unsigned long long)message->request_id,
                      message->data_size);
            return -1;
        }
        memcpy(copied.data, message->data, message->data_size);
    }

    pthread_mutex_lock(&queue->lock);
    if (!queue->running)
    {
        LOG_ERROR("thread_message_queue_push_copy failed: queue stopped type=%u request=%llu",
                  message->type,
                  (unsigned long long)message->request_id);
        pthread_mutex_unlock(&queue->lock);
        thread_message_release(&copied);
        return -3;
    }
    if (queue->size >= queue->capacity)
    {
        LOG_ERROR("thread_message_queue_push_copy failed: queue full type=%u request=%llu size=%zu capacity=%zu",
                  message->type,
                  (unsigned long long)message->request_id,
                  queue->size,
                  queue->capacity);
        pthread_mutex_unlock(&queue->lock);
        thread_message_release(&copied);
        return -2;
    }

    tail = (queue->head + queue->size) % queue->capacity;
    queue->messages[tail] = copied;
    queue->size++;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
    return 0;
}

/**
 * @brief 在已持锁状态下取出队头消息并转移其所有权。
 */
static int thread_message_queue_pop_locked(ThreadMessageQueue *queue, ThreadMessage *message)
{
    if (queue->size == 0)
        return queue->running ? 0 : -3;

    *message = queue->messages[queue->head];
    memset(&queue->messages[queue->head], 0, sizeof(queue->messages[queue->head]));
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;
    return 1;
}

/**
 * @brief 非阻塞取出队头消息。
 */
int thread_message_queue_try_pop(ThreadMessageQueue *queue, ThreadMessage *message)
{
    int ret = 0;

    if (!queue || !message || !queue->initialized)
    {
        LOG_ERROR("thread_message_queue_try_pop failed: invalid args queue=%p message=%p initialized=%d",
                  (void *)queue,
                  (void *)message,
                  queue ? queue->initialized : 0);
        return -1;
    }

    memset(message, 0, sizeof(*message));
    pthread_mutex_lock(&queue->lock);
    ret = thread_message_queue_pop_locked(queue, message);
    pthread_mutex_unlock(&queue->lock);
    return ret;
}

/**
 * @brief 等待消息到达后取出队头消息。
 */
int thread_message_queue_pop(ThreadMessageQueue *queue,
                             ThreadMessage *message,
                             int timeout_ms)
{
    struct timespec ts = {0};
    int wait_ret = 0;
    int ret = 0;

    if (!queue || !message || !queue->initialized)
    {
        LOG_ERROR("thread_message_queue_pop failed: invalid args queue=%p message=%p initialized=%d timeout_ms=%d",
                  (void *)queue,
                  (void *)message,
                  queue ? queue->initialized : 0,
                  timeout_ms);
        return -1;
    }
    if (timeout_ms <= 0)
        return thread_message_queue_try_pop(queue, message);

    memset(message, 0, sizeof(*message));
    thread_message_make_abs_timeout(&ts, timeout_ms);
    pthread_mutex_lock(&queue->lock);
    while (queue->running && queue->size == 0)
    {
        wait_ret = pthread_cond_timedwait(&queue->cond, &queue->lock, &ts);
        if (wait_ret == ETIMEDOUT)
        {
            pthread_mutex_unlock(&queue->lock);
            return 0;
        }
    }
    ret = thread_message_queue_pop_locked(queue, message);
    pthread_mutex_unlock(&queue->lock);
    return ret;
}

/**
 * @brief 查询队列是否存在待消费消息。
 */
int thread_message_queue_has_messages(ThreadMessageQueue *queue)
{
    int has_messages = 0;

    if (!queue || !queue->initialized)
    {
        LOG_ERROR("thread_message_queue_has_messages failed: invalid queue=%p initialized=%d",
                  (void *)queue,
                  queue ? queue->initialized : 0);
        return 0;
    }
    pthread_mutex_lock(&queue->lock);
    has_messages = queue->size > 0;
    pthread_mutex_unlock(&queue->lock);
    return has_messages;
}

/**
 * @brief 停止队列并唤醒等待者。
 */
void thread_message_queue_stop(ThreadMessageQueue *queue)
{
    if (!queue || !queue->initialized)
    {
        LOG_ERROR("thread_message_queue_stop failed: invalid queue=%p initialized=%d",
                  (void *)queue,
                  queue ? queue->initialized : 0);
        return;
    }
    pthread_mutex_lock(&queue->lock);
    queue->running = 0;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
}

/**
 * @brief 释放队列中尚未消费的消息和队列自身资源。
 */
void thread_message_queue_deinit(ThreadMessageQueue *queue)
{
    size_t i = 0;

    if (!queue || !queue->initialized)
    {
        LOG_ERROR("thread_message_queue_deinit failed: invalid queue=%p initialized=%d",
                  (void *)queue,
                  queue ? queue->initialized : 0);
        return;
    }

    thread_message_queue_stop(queue);
    for (i = 0; i < queue->capacity; ++i)
        thread_message_release(&queue->messages[i]);
    free(queue->messages);
    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->lock);
    memset(queue, 0, sizeof(*queue));
}
