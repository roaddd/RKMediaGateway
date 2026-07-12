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
    MEDIA_CONTROL_MESSAGE_SET_FPS = 1,           /* 修改采集帧率。 */
    MEDIA_CONTROL_MESSAGE_SET_VIDEO_ENCODE_PARAMS = 2  /* 修改编码器完整运行参数。 */
} MediaControlMessageType;

typedef enum {
    MEDIA_CONTROL_ENDPOINT_CAPTURE = 1, /* 视频采集线程。 */
    MEDIA_CONTROL_ENDPOINT_ENCODER = 2  /* 视频编码线程。 */
} MediaControlEndpointType;

typedef struct {
    int target_fps; /* 请求应用的目标帧率。 */
} MediaSetFpsRequest;

/*
 * 视频编码运行参数。
 * 动态帧率切换时编码器使用该结构一次性更新 fps、码率、GOP、RC 和 QP。
 */
typedef struct {
    int fps;         /* 编码帧率。 */
    int bitrate;     /* 目标码率，单位 bit/s。 */
    int gop;         /* GOP 长度。 */
    int rc_mode;     /* 码率控制模式，取值来自 MPP_ENC_RC_MODE_*。 */
    int qp_init;     /* 初始 QP；<=0 表示沿用当前或默认值。 */
    int qp_min;      /* P/B 帧最小 QP；<=0 表示沿用当前或默认值。 */
    int qp_max;      /* P/B 帧最大 QP；<=0 表示沿用当前或默认值。 */
    int qp_min_i;    /* I 帧最小 QP；<=0 表示沿用当前或默认值。 */
    int qp_max_i;    /* I 帧最大 QP；<=0 表示沿用当前或默认值。 */
    int qp_max_step; /* 相邻帧最大 QP 变化步长；<=0 表示沿用当前或默认值。 */
} MediaVideoEncodeParams;

/* 请求编码线程应用完整视频编码运行参数。 */
typedef struct {
    MediaVideoEncodeParams params; /* 请求应用的完整编码运行参数。 */
} MediaSetVideoEncodeParamsRequest;

typedef struct {
    int requested_fps; /* 请求帧率。 */
    int applied_fps;   /* 执行成功后实际采用的帧率。 */
} MediaSetFpsResult;

/* 编码线程应用完整编码运行参数后的执行结果。 */
typedef struct {
    MediaVideoEncodeParams requested_params; /* 请求参数。 */
    MediaVideoEncodeParams applied_params;   /* 执行成功后实际采用的参数。 */
} MediaSetVideoEncodeParamsResult;

#ifdef __cplusplus
}
#endif

#endif
