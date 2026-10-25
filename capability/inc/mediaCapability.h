/**
 * @file mediaCapability.h
 * @brief 整机媒体处理能力查询和候选配置准入校验接口。
 *
 * 本模块描述当前硬件平台可供所有业务模块共享的能力，不依赖mediaGateway、
 * RTSP或WebRTC等具体业务配置。启动配置、运行时用户配置和自动控制策略应
 * 先把目标参数转换为本接口的需求结构，再执行统一校验。
 */

#ifndef MEDIA_CAPABILITY_H
#define MEDIA_CAPABILITY_H

#include <stddef.h>
#include <stdint.h>

#include "commonDef.h"
#include "mediaPacket.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_CAPABILITY_MAX_VIDEO_STREAMS 4

/** 能力数据的可信程度。 */
typedef enum {
    MEDIA_CAPABILITY_CONFIDENCE_PROVISIONAL = 0, /* 来自芯片资料或保守工程估算，尚未完成整机实测。 */
    MEDIA_CAPABILITY_CONFIDENCE_VERIFIED = 1     /* 已在当前硬件、驱动和软件组合上完成稳定性验证。 */
} MediaCapabilityConfidence;

/** 视频编码候选配置被拒绝的具体原因。 */
typedef enum {
    MEDIA_VIDEO_CAPABILITY_OK = 0,                    /* 所有启用码流均满足当前能力约束。 */
    MEDIA_VIDEO_CAPABILITY_INVALID_ARGUMENT,          /* 接口参数为空或码流数量越界。 */
    MEDIA_VIDEO_CAPABILITY_INVALID_STREAM_CONFIG,     /* 启用码流的宽、高或帧率不为正数。 */
    MEDIA_VIDEO_CAPABILITY_UNSUPPORTED_CODEC,         /* 当前平台能力表不支持所请求的编码格式。 */
    MEDIA_VIDEO_CAPABILITY_TOO_MANY_INSTANCES,        /* 同时启用的编码实例数超过产品上限。 */
    MEDIA_VIDEO_CAPABILITY_WIDTH_EXCEEDED,            /* 某一路图像宽度超过单实例上限。 */
    MEDIA_VIDEO_CAPABILITY_HEIGHT_EXCEEDED,           /* 某一路图像高度超过单实例上限。 */
    MEDIA_VIDEO_CAPABILITY_FPS_EXCEEDED,              /* 某一路帧率超过单实例上限。 */
    MEDIA_VIDEO_CAPABILITY_AGGREGATE_LOAD_EXCEEDED    /* 所有码流的总像素吞吐量超过上限。 */
} MediaVideoCapabilityCode;

/** 一路候选视频编码需求。 */
typedef struct {
    int enabled;          /* 是否把该路计入编码实例和总负载。 */
    MediaCodecType codec; /* 视频编码格式；当前RK3568能力表只接受H.264。 */
    int width;            /* 编码图像有效宽度，单位像素。 */
    int height;           /* 编码图像有效高度，单位像素。 */
    int fps;              /* 目标编码帧率，单位帧/秒。 */
} MediaVideoEncodeRequirement;

/** 当前平台的视频编码能力描述。 */
typedef struct {
    MediaCodecType codec;                    /* 本能力条目对应的编码格式。 */
    int max_encoder_instances;               /* 产品允许同时启用的最大编码实例数。 */
    int max_width;                           /* 单个编码实例支持的最大有效宽度。 */
    int max_height;                          /* 单个编码实例支持的最大有效高度。 */
    int max_single_stream_fps;               /* 单个编码实例允许的最大帧率。 */
    uint64_t max_pixels_per_second;          /* 所有实例总像素吞吐量上限。 */
    size_t verified_combination_count;       /* 已完成实测的精确码流组合数量。 */
    MediaCapabilityConfidence confidence;    /* 当前上限是暂定估算还是实测结论。 */
} MediaVideoEncodeCapability;

/** 一次候选配置校验的详细结果。 */
typedef struct {
    MediaVideoCapabilityCode code;            /* 准入结果及具体拒绝原因。 */
    MediaCapabilityConfidence confidence;     /* 本次判断所依据能力数据的可信程度。 */
    int rejected_stream_index;                /* 失败码流下标；组合级失败或成功时为-1。 */
    int active_encoder_instances;             /* 候选配置实际启用的编码实例数量。 */
    uint64_t requested_pixels_per_second;     /* 候选配置计算得到的总像素吞吐量。 */
    uint64_t limit_pixels_per_second;         /* 当前能力表允许的总像素吞吐量。 */
    int matched_verified_combination;         /* 是否命中经过实测的精确组合。 */
} MediaVideoCapabilityValidation;

/**
 * @brief 查询当前平台指定格式的视频编码能力。
 * @param codec 需要查询的编码格式。
 * @param capability 输出能力描述。
 * @return MEDIA_OK表示查询成功；参数为空或格式不支持时返回对应MediaResult。
 */
MediaResult media_capability_get_video_encode(
    MediaCodecType codec,
    MediaVideoEncodeCapability *capability);

/**
 * @brief 校验一组候选视频编码需求是否能够同时运行。
 *
 * 校验顺序为单路参数、编码格式、实例数量、单路宽高/帧率、实测组合白名单，
 * 最后检查暂定总像素预算。禁用码流不会计入实例数和总负载。即使返回失败，
 * validation仍会尽可能给出失败位置、实际负载和当前上限。
 *
 * @param streams 候选码流数组；stream_count 为 0 时允许传入 NULL。
 * @param stream_count 数组元素数量，可为 0，也可包含禁用码流。
 * @param validation 输出详细校验结果。
 * @return MEDIA_OK表示允许；配置不合法返回MEDIA_ERR_INVALID_CONFIG，能力超限返回
 * MEDIA_ERR_UNSUPPORTED，接口参数错误返回MEDIA_ERR_INVALID_PARAM。
 */
MediaResult media_capability_validate_video_encode(
    const MediaVideoEncodeRequirement *streams,
    size_t stream_count,
    MediaVideoCapabilityValidation *validation);

/**
 * @brief 将能力数据置信级别转换为稳定的调试字符串。
 * @param confidence 能力数据的置信级别。
 * @return 静态字符串地址，调用方不得释放。
 */
const char *media_capability_confidence_string(MediaCapabilityConfidence confidence);

/**
 * @brief 将视频能力校验码转换为稳定的调试字符串。
 * @param code 视频能力校验码。
 * @return 静态字符串地址，调用方不得释放。
 */
const char *media_video_capability_code_string(MediaVideoCapabilityCode code);

#ifdef __cplusplus
}
#endif

#endif
