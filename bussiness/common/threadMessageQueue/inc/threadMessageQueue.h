/**
 * @file threadMessageQueue.h
 * @brief 通用线程消息队列接口。
 *
 * 队列对消息数据执行深拷贝，发送方在 push 返回后即可释放原始数据。
 * pop 成功后消息所有权转移给接收方，接收方必须调用
 * thread_message_release 释放消息内部数据。
 */

#ifndef __THREAD_MESSAGE_QUEUE_H__
#define __THREAD_MESSAGE_QUEUE_H__

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t type;          /* 消息类型，由上层业务定义。 */
    uint64_t request_id;    /* 请求标识，用于关联命令与执行结果。 */
    uint32_t endpoint_type; /* 目标或来源端点类型，由上层业务定义。 */
    int32_t endpoint_index; /* 目标或来源端点下标。 */
    int32_t status;         /* 执行结果；请求消息通常填写 0。 */
    void *data;             /* 动态分配的消息数据。 */
    size_t data_size;       /* 消息数据长度。 */
} ThreadMessage;

typedef struct {
    ThreadMessage *messages; /* 固定容量的环形消息槽。 */
    size_t capacity;         /* 队列最大消息数量。 */
    size_t head;             /* 当前队头下标。 */
    size_t size;             /* 当前消息数量。 */
    pthread_mutex_t lock;    /* 保护环形队列与运行状态。 */
    pthread_cond_t cond;     /* 新消息到达或队列停止时唤醒消费者。 */
    int running;             /* 是否允许继续收发消息。 */
    int initialized;         /* 同步资源是否初始化完成。 */
} ThreadMessageQueue;

/**
 * @brief 初始化线程消息队列。
 * @param queue 待初始化队列。
 * @param capacity 队列最大消息数量，必须大于 0。
 * @return 0 成功，-1 失败。
 */
int thread_message_queue_init(ThreadMessageQueue *queue, size_t capacity);

/**
 * @brief 深拷贝一条消息并放入队列。
 * @param queue 目标队列。
 * @param message 待复制消息。
 * @return 0 成功，-1 参数或内存错误，-2 队列已满，-3 队列已停止。
 */
int thread_message_queue_push_copy(ThreadMessageQueue *queue, const ThreadMessage *message);

/**
 * @brief 非阻塞取出一条消息。
 * @param queue 来源队列。
 * @param message 接收消息；成功后由调用方负责 release。
 * @return 1 成功取出，0 暂无消息，-1 参数错误，-3 队列停止且为空。
 */
int thread_message_queue_try_pop(ThreadMessageQueue *queue, ThreadMessage *message);

/**
 * @brief 等待并取出一条消息。
 * @param queue 来源队列。
 * @param message 接收消息；成功后由调用方负责 release。
 * @param timeout_ms 最大等待毫秒数；0 表示非阻塞。
 * @return 1 成功取出，0 超时，-1 参数错误，-3 队列停止且为空。
 */
int thread_message_queue_pop(ThreadMessageQueue *queue,
                             ThreadMessage *message,
                             int timeout_ms);

/**
 * @brief 查询队列当前是否存在待处理消息。
 * @return 1 存在消息，0 不存在消息或队列无效。
 */
int thread_message_queue_has_messages(ThreadMessageQueue *queue);

/**
 * @brief 释放 pop 后取得的消息数据并清零消息。
 */
void thread_message_release(ThreadMessage *message);

/**
 * @brief 停止队列并唤醒所有等待线程。
 */
void thread_message_queue_stop(ThreadMessageQueue *queue);

/**
 * @brief 释放队列内未消费消息及同步资源。
 */
void thread_message_queue_deinit(ThreadMessageQueue *queue);

#ifdef __cplusplus
}
#endif

#endif
