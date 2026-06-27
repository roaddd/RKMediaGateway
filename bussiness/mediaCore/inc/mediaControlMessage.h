/**
 * @file mediaControlMessage.h
 * @brief 媒体工作线程之间使用的控制消息定义。
 *
 * 本文件只定义媒体业务语义；消息存储、深拷贝和线程同步由
 * ThreadMessageQueue 通用模块负责。
 */

#ifndef __MEDIA_CONTROL_MESSAGE_H__
#define __MEDIA_CONTROL_MESSAGE_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_CONTROL_MESSAGE_SET_FPS = 1 /* 修改采集或编码器帧率。 */
} MediaControlMessageType;

typedef enum {
    MEDIA_CONTROL_ENDPOINT_CAPTURE = 1, /* 视频采集线程。 */
    MEDIA_CONTROL_ENDPOINT_ENCODER = 2  /* 视频编码线程。 */
} MediaControlEndpointType;

typedef struct {
    int target_fps; /* 请求应用的目标帧率。 */
} MediaSetFpsRequest;

typedef struct {
    int requested_fps; /* 请求帧率。 */
    int applied_fps;   /* 执行成功后实际采用的帧率。 */
} MediaSetFpsResult;

#ifdef __cplusplus
}
#endif

#endif
