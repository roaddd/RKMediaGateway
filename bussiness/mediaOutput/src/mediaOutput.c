#include "mediaOutput.h"

#include "gb28181/inc/gb28181Output.h"
#include "logger.h"
#include "rtmp/inc/rtmpOutput.h"
#include "rtsp/inc/rtspOutput.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_OUTPUT_QUEUE_CAPACITY 32
#define DEFAULT_RECONNECT_INTERVAL_MS 1000

/**
 * @description: 鍒濆鍖栦竴涓崟绫诲獟浣撶幆褰㈤槦鍒椼€? * @param queue 寰呭垵濮嬪寲闃熷垪銆? * @param capacity 闃熷垪瀹归噺锛?=0 鏃朵娇鐢ㄩ粯璁ゅ閲忋€? * @return 0 鎴愬姛锛?1 澶辫触銆? */
static int output_queue_init(MediaOutputPacketQueue *queue, int capacity)
{
    if (!queue)
    {
        LOG_ERROR("output_queue_init failed: queue is NULL");
        return -1;
    }
    memset(queue, 0, sizeof(*queue));
    queue->capacity = (capacity > 0) ? capacity : DEFAULT_OUTPUT_QUEUE_CAPACITY;
    queue->items = (MediaPacket *)calloc((size_t)queue->capacity, sizeof(MediaPacket));
    if (!queue->items)
    {
        LOG_ERROR("output_queue_init failed: alloc capacity=%d", queue->capacity);
        return -1;
    }
    return 0;
}

/**
 * @description: 閲婃斁鍗曠被濯掍綋鐜舰闃熷垪鍙婂叾涓皻鏈彂閫佺殑鍖呭紩鐢ㄣ€? * @param queue 寰呴噴鏀鹃槦鍒椼€? */
static void output_queue_deinit(MediaOutputPacketQueue *queue)
{
    int i;
    if (!queue)
        return;
    for (i = 0; i < queue->capacity; ++i)
        media_packet_reset(&queue->items[i]);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

/**
 * @description: 杩斿洖褰撳墠闊抽闃熷垪鍜岃棰戦槦鍒楁繁搴︽€诲拰銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @return 闃熷垪鎬绘繁搴︺€? */
static int output_queue_total_depth(const MediaOutput *output)
{
    return output->video_queue.size + output->audio_queue.size;
}

/**
 * @description: 鏇存柊 stats.queue_depth锛涜皟鐢ㄦ柟蹇呴』鎸佹湁 output->lock銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? */
static void output_update_queue_depth_locked(MediaOutput *output)
{
    output->stats.queue_depth = output_queue_total_depth(output);
}

/**
 * @description: 鏌ョ湅闃熷ご鍖呬絾涓嶅嚭闃熴€? * @param queue 鍗曠被濯掍綋闃熷垪銆? * @return 闃熷ご鍖呮寚閽堬紱闃熷垪涓虹┖鏃惰繑鍥?NULL銆? */
static MediaPacket *output_queue_peek(MediaOutputPacketQueue *queue)
{
    if (!queue || queue->size <= 0)
        return NULL;
    return &queue->items[queue->head];
}

/**
 * @description: 浠庢寚瀹氶槦鍒楀脊鍑洪槦澶村寘寮曠敤锛涜皟鐢ㄦ柟蹇呴』鎸佹湁 output->lock銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @param queue 鍗曠被濯掍綋闃熷垪銆? * @param packet 杈撳嚭寮瑰嚭鐨勫獟浣撳寘寮曠敤銆? * @return 0 鎴愬姛锛?1 闃熷垪涓虹┖銆? */
static int output_queue_pop_locked(MediaOutput *output, MediaOutputPacketQueue *queue, MediaPacket *packet)
{
    if (!queue || queue->size <= 0)
        return -1;

    *packet = queue->items[queue->head];
    media_packet_init(&queue->items[queue->head]);
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;
    output_update_queue_depth_locked(output);
    return 0;
}

/**
 * @description: 涓㈠純鎸囧畾闃熷垪鏈€鏃х殑濯掍綋鍖咃紱璋冪敤鏂瑰繀椤绘寔鏈?output->lock銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @param queue 鍗曠被濯掍綋闃熷垪銆? */
static void output_queue_drop_oldest_locked(MediaOutput *output, MediaOutputPacketQueue *queue)
{
    MediaPacket packet;

    media_packet_init(&packet);
    if (output_queue_pop_locked(output, queue, &packet) == 0)
    {
        output->stats.dropped_frames++;
        media_packet_reset(&packet);
    }
}

/**
 * @description: 鏍规嵁 MediaPacket 绫诲瀷閫夋嫨闊抽闃熷垪鎴栬棰戦槦鍒椼€? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @param packet 寰呭叆闃熷獟浣撳寘銆? * @return 瀵瑰簲鍗曠被濯掍綋闃熷垪銆? */
static MediaOutputPacketQueue *output_queue_for_packet(MediaOutput *output, const MediaPacket *packet)
{
    if (packet->frame_type == MEDIA_FRAME_TYPE_AUDIO)
        return &output->audio_queue;
    return &output->video_queue;
}

/**
 * @description: 灏嗗獟浣撳寘寮曠敤鍘嬪叆鎸囧畾闃熷垪锛涜皟鐢ㄦ柟蹇呴』鎸佹湁 output->lock銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @param queue 鍗曠被濯掍綋闃熷垪銆? * @param packet 寰呭叆闃熷獟浣撳寘銆? * @return 0 鎴愬姛锛?1 闃熷垪涓嶅彲鐢ㄦ垨宸叉弧銆? */
static int output_queue_push_locked(MediaOutput *output, MediaOutputPacketQueue *queue, const MediaPacket *packet)
{
    int tail;
    if (!queue || !queue->items || queue->capacity <= 0 || queue->size >= queue->capacity)
    {
        LOG_ERROR("output_queue_push_locked failed: invalid/full queue=%p items=%p capacity=%d size=%d",
                  (void *)queue,
                  queue ? (void *)queue->items : NULL,
                  queue ? queue->capacity : -1,
                  queue ? queue->size : -1);
        return -1;
    }

    tail = (queue->head + queue->size) % queue->capacity;
    media_packet_copy_ref(&queue->items[tail], packet);
    queue->size++;
    output_update_queue_depth_locked(output);
    return 0;
}

/**
 * @description: 浠庨煶棰?瑙嗛鍙岄槦鍒椾腑閫夋嫨涓嬩竴鍖呭彂閫侊紱璋冪敤鏂瑰繀椤绘寔鏈?output->lock銆? *
 * 绛栫暐锛? * 1. 閲嶈繛鍚庣瓑寰呭叧閿抚鏃讹紝浼樺厛鍙栬棰戦槦鍒楋紝閬垮厤闊抽鍖呭湪鍏抽敭甯у墠琚彂閫併€? * 2. 姝ｅ父鐘舵€佷笅鎸夐槦澶?PTS 杈冩棭鑰呭彂閫侊紝淇濇寔闊宠棰戝熀鏈椂搴忋€? * 3. 鍗曚晶涓虹┖鏃剁洿鎺ュ彂閫佸彟涓€渚с€? *
 * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @param packet 杈撳嚭寮瑰嚭鐨勫獟浣撳寘寮曠敤銆? * @return 0 鎴愬姛锛?1 涓や釜闃熷垪鍧囦负绌恒€? */
static int output_pop_next_locked(MediaOutput *output, MediaPacket *packet)
{
    MediaPacket *video = output_queue_peek(&output->video_queue);
    MediaPacket *audio = output_queue_peek(&output->audio_queue);

    if (!video && !audio)
        return -1;

    if (output->waiting_for_keyframe && video)
        return output_queue_pop_locked(output, &output->video_queue, packet);
    if (!video)
        return output_queue_pop_locked(output, &output->audio_queue, packet);
    if (!audio)
        return output_queue_pop_locked(output, &output->video_queue, packet);
    if (audio->pts_us < video->pts_us)
        return output_queue_pop_locked(output, &output->audio_queue, packet);
    return output_queue_pop_locked(output, &output->video_queue, packet);
}

/**
 * @description: 杈撳嚭閫氶亾鍚庡彴鍙戦€佺嚎绋嬨€? *
 * 绾跨▼璐熻矗绛夊緟闃熷垪鏁版嵁銆佹噿杩炴帴涓嬫父銆佹墽琛岄噸杩炲悗鐨勫叧閿抚淇濇姢銆? * 璋冪敤鍗忚 send_packet 鍥炶皟锛屽苟缁存姢鍙戦€?涓㈠寘/澶辫触缁熻銆? *
 * @param arg MediaOutput 鎸囬拡銆? * @return NULL銆? */
static void *media_output_thread(void *arg)
{
    MediaOutput *output = (MediaOutput *)arg;
    MediaPacket packet;

    media_packet_init(&packet);
    while (1)
    {
        pthread_mutex_lock(&output->lock);
        while (!output->stop_requested && output_queue_total_depth(output) == 0)
            pthread_cond_wait(&output->cond, &output->lock);
        if (output->stop_requested && output_queue_total_depth(output) == 0)
        {
            pthread_mutex_unlock(&output->lock);
            break;
        }
        if (output_pop_next_locked(output, &packet) != 0)
        {
            pthread_mutex_unlock(&output->lock);
            continue;
        }
        pthread_mutex_unlock(&output->lock);

        if (!output->connected)
        {
            if (output->vtable->connect(output) == 0)
            {
                pthread_mutex_lock(&output->lock);
                output->connected = 1;
                output->stats.connected = 1;
                output->waiting_for_keyframe = output->config.drop_until_keyframe_after_reconnect ? 1 : 0;
                output->stats.waiting_for_keyframe = output->waiting_for_keyframe;
                output->stats.reconnect_count++;
                pthread_mutex_unlock(&output->lock);
                printf("[OUTPUT] name=%s event=connected reconnects=%" PRIu64 "\n",
                       output->config.name ? output->config.name : "unknown",
                       output->stats.reconnect_count);
            }
            else
            {
                pthread_mutex_lock(&output->lock);
                output->stats.connected = 0;
                pthread_mutex_unlock(&output->lock);
                LOG_WARN("[OUTPUT] name=%s event=connect_failed retry_ms=%d",
                         output->config.name ? output->config.name : "unknown",
                         output->config.reconnect_interval_ms);
                media_packet_reset(&packet);
                usleep((useconds_t)output->config.reconnect_interval_ms * 1000U);
                continue;
            }
        }

        if (output->waiting_for_keyframe && !packet.is_key_frame)
        {
            pthread_mutex_lock(&output->lock);
            output->stats.dropped_frames++;
            pthread_mutex_unlock(&output->lock);
            media_packet_reset(&packet);
            continue;
        }

        if (output->waiting_for_keyframe && packet.is_key_frame)
        {
            pthread_mutex_lock(&output->lock);
            output->waiting_for_keyframe = 0;
            output->stats.waiting_for_keyframe = 0;
            pthread_mutex_unlock(&output->lock);
        }

        if (output->vtable->send_packet(output, &packet) != 0)
        {
            pthread_mutex_lock(&output->lock);
            output->connected = 0;
            output->stats.connected = 0;
            output->waiting_for_keyframe = output->config.drop_until_keyframe_after_reconnect ? 1 : 0;
            output->stats.waiting_for_keyframe = output->waiting_for_keyframe;
            output->stats.send_failures++;
            pthread_mutex_unlock(&output->lock);
            LOG_ERROR("[OUTPUT] name=%s event=send_failed frame=%" PRIu64 " failures=%" PRIu64,
                      output->config.name ? output->config.name : "unknown",
                      packet.frame_id,
                      output->stats.send_failures);
            if (output->vtable->disconnect)
                output->vtable->disconnect(output);
        }
        else
        {
            pthread_mutex_lock(&output->lock);
            output->stats.sent_frames++;
            output->stats.sent_bytes += packet.buffer ? packet.buffer->size : 0;
            pthread_mutex_unlock(&output->lock);
        }

        media_packet_reset(&packet);
    }

    if (output->vtable->disconnect)
        output->vtable->disconnect(output);
    return NULL;
}

/**
 * @description: 鎸夊崗璁被鍨嬪垱寤哄苟鍒濆鍖栬緭鍑洪€氶亾銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @param config 杈撳嚭鍗忚閰嶇疆銆? * @return 0 鎴愬姛锛?1 澶辫触銆? */
int media_output_setup(MediaOutput *output, const MediaOutputConfig *config)
{
    if (!output || !config)
    {
        LOG_ERROR("media_output_setup failed: invalid arguments output=%p config=%p",
                  (void *)output,
                  (const void *)config);
        return -1;
    }

    switch (config->type)
    {
    case MEDIA_OUTPUT_TYPE_RTSP:
        return media_output_setup_rtsp(output, &config->protocol.rtsp);
    case MEDIA_OUTPUT_TYPE_RTMP:
        return media_output_setup_rtmp(output, &config->protocol.rtmp);
    case MEDIA_OUTPUT_TYPE_GB28181:
        return media_output_setup_gb28181(output, &config->protocol.gb28181);
    default:
        LOG_ERROR("media_output_setup failed: unknown type=%d", config->type);
        return -1;
    }
}

/**
 * @description: 鍒濆鍖栭€氱敤杈撳嚭閫氶亾鐘舵€併€佸弻闃熷垪鍜屽悓姝ュ璞°€? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @param config 閫氱敤杈撳嚭閫氶亾閰嶇疆銆? * @param vtable 鍗忚鍥炶皟琛ㄣ€? * @param impl 鍗忚绉佹湁涓婁笅鏂囥€? * @return 0 鎴愬姛锛?1 澶辫触銆? */
int media_output_init(MediaOutput *output,
                      const MediaOutputChannelConfig *config,
                      const MediaOutputVTable *vtable,
                      void *impl)
{
    int queue_capacity;

    if (!output || !config || !vtable || !vtable->connect || !vtable->send_packet)
    {
        LOG_ERROR("media_output_init failed: invalid arguments output=%p config=%p vtable=%p connect=%p send=%p",
                  (void *)output,
                  (const void *)config,
                  (const void *)vtable,
                  vtable ? (void *)vtable->connect : NULL,
                  vtable ? (void *)vtable->send_packet : NULL);
        return -1;
    }

    memset(output, 0, sizeof(*output));
    output->config = *config;
    output->vtable = vtable;
    output->impl = impl;
    output->config.reconnect_interval_ms = (config->reconnect_interval_ms > 0)
                                               ? config->reconnect_interval_ms
                                               : DEFAULT_RECONNECT_INTERVAL_MS;

    queue_capacity = (config->queue_capacity > 0) ? config->queue_capacity : DEFAULT_OUTPUT_QUEUE_CAPACITY;
    if (output_queue_init(&output->video_queue, queue_capacity) != 0 ||
        output_queue_init(&output->audio_queue, queue_capacity) != 0)
    {
        LOG_ERROR("media_output_init failed: queue alloc name=%s capacity=%d",
                  config->name ? config->name : "unknown",
                  queue_capacity);
        output_queue_deinit(&output->video_queue);
        output_queue_deinit(&output->audio_queue);
        return -1;
    }

    pthread_mutex_init(&output->lock, NULL);
    pthread_cond_init(&output->cond, NULL);
    output->waiting_for_keyframe = output->config.drop_until_keyframe_after_reconnect ? 1 : 0;
    output->stats.waiting_for_keyframe = output->waiting_for_keyframe;
    return 0;
}

/**
 * @description: 鍚姩鍗忚绉佹湁璧勬簮鍜岄€氱敤鍙戦€佺嚎绋嬨€? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @return 0 鎴愬姛锛?1 澶辫触銆? */
int media_output_start(MediaOutput *output)
{
    if (!output)
    {
        LOG_ERROR("media_output_start failed: output is NULL");
        return -1;
    }

    if (output->vtable->start && output->vtable->start(output) != 0)
    {
        LOG_ERROR("media_output_start failed: vtable start name=%s",
                  output->config.name ? output->config.name : "unknown");
        return -1;
    }

    output->running = 1;
    if (pthread_create(&output->thread, NULL, media_output_thread, output) != 0)
    {
        output->running = 0;
        LOG_ERROR("media_output_start failed: pthread_create name=%s",
                  output->config.name ? output->config.name : "unknown");
        if (output->vtable->stop)
            output->vtable->stop(output);
        return -1;
    }
    return 0;
}

/**
 * @description: 灏嗗獟浣撳寘鍘嬪叆闊抽鎴栬棰戦槦鍒椼€? *
 * 婊￠槦鍒楃瓥鐣ワ細
 * - 闊抽婊★細涓㈠純鏃ч煶棰戯紝淇濈暀鏈€鏂伴煶棰戜互闄嶄綆瀹炴椂寤惰繜銆? * - 瑙嗛闈炲叧閿抚婊★細涓㈠純鏂板寘锛岄伩鍏嶅欢杩熺户缁爢绉€? * - 瑙嗛鍏抽敭甯ф弧锛氫涪寮冩棫瑙嗛锛屼负鍏抽敭甯ц吘鍑烘仮澶嶇偣銆? *
 * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @param packet 寰呭彂閫佸獟浣撳寘銆? * @return 0 鎴愬姛鎴栨寜绛栫暐涓㈠純锛?1 鍙傛暟闈炴硶鎴栧叆闃熷け璐ャ€? */
int media_output_enqueue(MediaOutput *output, const MediaPacket *packet)
{
    MediaOutputPacketQueue *queue;

    if (!output || !packet || !packet->buffer)
    {
        LOG_ERROR("media_output_enqueue failed: invalid args output=%p packet=%p buffer=%p",
                  (void *)output,
                  (const void *)packet,
                  packet ? (void *)packet->buffer : NULL);
        return -1;
    }

    pthread_mutex_lock(&output->lock);
    queue = output_queue_for_packet(output, packet);

    /* 澶勭悊杈撳嚭閫氶亾闃熷垪婊＄殑鎯呭喌 */
    if (queue->size >= queue->capacity)
    {
        if (packet->frame_type == MEDIA_FRAME_TYPE_AUDIO)
        {
            output_queue_drop_oldest_locked(output, queue);
        }
        else if (!packet->is_key_frame)
        {
            output->stats.dropped_frames++;
            pthread_mutex_unlock(&output->lock);
            return 0;
        }
        else
        {
            while (queue->size >= queue->capacity)
                output_queue_drop_oldest_locked(output, queue);
        }
    }

    if (output_queue_push_locked(output, queue, packet) != 0)
    {
        LOG_ERROR("media_output_enqueue failed: push packet frame=%" PRIu64 " type=%d codec=%d",
                  packet->frame_id,
                  packet->frame_type,
                  packet->codec);
        pthread_mutex_unlock(&output->lock);
        return -1;
    }
    pthread_cond_signal(&output->cond);
    pthread_mutex_unlock(&output->lock);
    return 0;
}

/**
 * @description: 娑堣垂鍗忚渚цЕ鍙戠殑澶栭儴 IDR 璇锋眰銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @return 1 闇€瑕佽姹傜紪鐮佸櫒绔嬪嵆浜х敓 IDR锛? 鏃犺姹傘€? */
int media_output_consume_external_idr_request(MediaOutput *output)
{
    if (!output)
        return 0;
    if (output->type == MEDIA_OUTPUT_TYPE_RTSP)
        return media_output_rtsp_consume_external_idr_request(output);
    if (output->type == MEDIA_OUTPUT_TYPE_GB28181)
        return media_output_gb28181_consume_external_idr_request(output);
    return 0;
}

/**
 * @description: 璇锋眰杈撳嚭绾跨▼閫€鍑哄苟鍋滄鍗忚绉佹湁璧勬簮銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? */
void media_output_stop(MediaOutput *output)
{
    if (!output || !output->running)
        return;

    pthread_mutex_lock(&output->lock);
    output->stop_requested = 1;
    pthread_cond_broadcast(&output->cond);
    pthread_mutex_unlock(&output->lock);
    pthread_join(output->thread, NULL);
    output->running = 0;
    if (output->vtable->stop)
        output->vtable->stop(output);
}

/**
 * @description: 閲婃斁杈撳嚭閫氶亾鎸佹湁鐨勫叏閮ㄩ€氱敤璧勬簮銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? */
void media_output_deinit(MediaOutput *output)
{
    if (!output)
        return;

    media_output_stop(output);
    if (!output->running && output->vtable && output->vtable->stop)
        output->vtable->stop(output);
    output_queue_deinit(&output->video_queue);
    output_queue_deinit(&output->audio_queue);
    pthread_cond_destroy(&output->cond);
    pthread_mutex_destroy(&output->lock);
    memset(output, 0, sizeof(*output));
}

/**
 * @description: 璇诲彇杈撳嚭閫氶亾缁熻蹇収銆? * @param output 杈撳嚭閫氶亾瀵硅薄銆? * @param stats 杈撳嚭缁熻蹇収銆? */
void media_output_get_stats(MediaOutput *output, MediaOutputStats *stats)
{
    if (!output || !stats)
        return;

    pthread_mutex_lock(&output->lock);
    *stats = output->stats;
    pthread_mutex_unlock(&output->lock);
}
