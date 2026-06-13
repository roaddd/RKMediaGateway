/**
 * @file ispController.h
 * @brief RKAIQ/ISP 生命周期控制模块接口。
 *
 * 该模块只负责 RKAIQ sysctl 的初始化、prepare、start、stop 和 deinit，
 * 不直接参与 V4L2 取帧、MPP 编码或输出分发。公共头文件使用 void*
 * 保存 RKAIQ 内部上下文，避免上层 mediaGateway 直接依赖 RKAIQ 头文件。
 */
#ifndef __ISP_CONTROLLER_H__
#define __ISP_CONTROLLER_H__

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 一批可在 RKAIQ started 后动态下发的图像控制参数。
 *
 * 该结构体用于统一承载画质、曝光、白平衡、抗频闪、降噪和去雾等运行时控制项。
 * 大部分整数字段使用 -1 作为“不修改”哨兵值，浮点增益/时间类字段使用 <=0 表示不修改。
 */
typedef struct {
    int enabled;                    /* 是否下发运行时图像控制参数；0 表示整批控制不生效。 */
    int brightness;                 /* 亮度等级 [0,255]；-1 表示不修改。 */
    int contrast;                   /* 对比度等级 [0,255]；-1 表示不修改。 */
    int saturation;                 /* 饱和度等级 [0,255]；-1 表示不修改。 */
    int hue;                        /* 色调等级 [0,255]；-1 表示不修改。 */
    int sharpness;                  /* 锐度等级 [0,100]；-1 表示不修改。 */
    int exposure_mode;              /* 曝光模式，取值遵循 RKAIQ opMode_t：0 自动，1 手动，-1 不修改。 */
    float exposure_time;            /* 固定曝光时间，>0 时下发；单位遵循 RKAIQ SDK。 */
    float exposure_gain;            /* 固定曝光增益，>0 时下发。 */
    int wb_mode;                    /* 白平衡模式，取值遵循 RKAIQ opMode_t：0 自动，1 手动，-1 不修改。 */
    int wb_ct;                      /* 手动白平衡色温，<=0 表示不修改。 */
    float wb_rgain;                 /* 手动白平衡 R gain，<=0 表示不修改。 */
    float wb_grgain;                /* 手动白平衡 GR gain，<=0 表示不修改。 */
    float wb_gbgain;                /* 手动白平衡 GB gain，<=0 表示不修改。 */
    float wb_bgain;                 /* 手动白平衡 B gain，<=0 表示不修改。 */
    int anti_flicker_enable;        /* 抗频闪开关：0/1 表示下发关闭/开启，-1 表示不修改。 */
    int power_line_freq;            /* 电源频率：0 关闭，1 50Hz，2 60Hz，-1 表示不修改。 */
    int nr_mode;                    /* 降噪模式，取值遵循 RKAIQ opMode_t；-1 表示不修改。 */
    int anr_strength;               /* 普通降噪强度 [0,100]；-1 表示不修改。 */
    int dehaze_mode;                /* 去雾模式，取值遵循 RKAIQ opMode_t；-1 表示不修改。 */
    int dehaze_strength;            /* 手动去雾强度 [0,10]；-1 表示不修改。 */
} IspControllerImageControls;

/**
 * @brief ISP/RKAIQ 健康诊断状态枚举。
 *
 * 该枚举只描述诊断结果，不直接触发重启或停流。上层可以根据状态决定是否告警、
 * 降级、或在后续自恢复阶段执行协同重启。
 */
typedef enum {
    ISP_CONTROLLER_HEALTH_DISABLED = 0,     /* 健康诊断未启用。 */
    ISP_CONTROLLER_HEALTH_OK = 1,           /* ISP/RKAIQ 当前状态正常。 */
    ISP_CONTROLLER_HEALTH_NOT_STARTED = 2,  /* 配置启用但 RKAIQ 未进入 started 状态。 */
    ISP_CONTROLLER_HEALTH_META_STALLED = 3, /* RKAIQ metas 回调超时，可能 3A 或 ISP pipeline 停滞。 */
    ISP_CONTROLLER_HEALTH_ERROR_LIMIT = 4   /* RKAIQ error 回调次数达到阈值。 */
} IspControllerHealthState;

/**
 * @brief 低照度场景自动优化策略配置。
 *
 * 该结构体描述夜间/低照度检测阈值，以及进入低照度后要联动的曝光、降噪、
 * 锐度和编码码率策略。enter/exit 阈值成对使用，用于形成回滞避免频繁切换。
 */
typedef struct {
    int enabled;                    /* 是否启用低照度自动优化策略。 */
    float enter_lux;                /* 环境照度低于该值时进入低照度模式；<=0 表示不使用 lux 判断。 */
    float exit_lux;                 /* 环境照度高于该值时退出低照度模式；应大于 enter_lux。 */
    float enter_mean_luma;          /* 平均亮度低于该值时进入低照度模式；<=0 表示不使用亮度判断。 */
    float exit_mean_luma;           /* 平均亮度高于该值时退出低照度模式；应大于 enter_mean_luma。 */
    float exposure_gain;            /* 低照度时锁定曝光增益；<=0 表示不修改。 */
    float exposure_time;            /* 低照度时锁定曝光时间；<=0 表示不修改。 */
    int nr_strength;                /* 低照度时降噪强度 [0,100]；-1 表示不修改。 */
    int sharpness;                  /* 低照度时锐度 [0,100]；-1 表示不修改。 */
    int normal_nr_strength;         /* 退出低照度时恢复的降噪强度 [0,100]；-1 表示恢复自动模式。 */
    int normal_sharpness;           /* 退出低照度时恢复的锐度 [0,100]；-1 表示不修改。 */
    int bitrate_boost_percent;      /* 低照度时编码码率提升百分比；0 表示不联动码率。 */
    int qp_delta;                   /* FIXQP 模式下低照度 QP 调整量；负数降低 QP 提升画质，0 表示不联动 QP。 */
    int min_switch_interval_ms;     /* 低照度模式最小切换间隔，避免临界抖动。 */
} IspControllerLowLightConfig;

/**
 * @brief ISP/RKAIQ 生命周期和异常降级策略配置。
 *
 * 该结构体只描述控制链路是否启用、停止时如何处理外部硬件状态，以及初始化失败时是否允许
 * 上层继续走普通 V4L2 采集路径。
 */
typedef struct {
    int enabled;                    /* 是否启用 RKAIQ/ISP 控制链路。 */
    int keep_external_hw_state;     /* stop 时是否保留补光灯、IRCut 等外部硬件状态。 */
    int fallback_on_error;          /* RKAIQ 初始化失败时是否允许上层降级继续运行。 */
} IspControllerLifecycleConfig;

/**
 * @brief ISP/RKAIQ 绑定的 sensor、IQ 文件和 prepare 参数配置。
 *
 * 该结构体集中保存与具体 camera pipeline 绑定相关的参数，便于后续扩展多 sensor、
 * HDR 工作模式或强制 IQ 文件选择。
 */
typedef struct {
    const char *sensor_name;        /* sensor media entity 名称；为空时尝试通过 video_device 反查。 */
    const char *video_device;       /* 与 sensor 绑定的 video node，例如 /dev/video0。 */
    const char *iq_dir;             /* RKAIQ IQ 文件搜索目录。 */
    const char *force_iq_file;      /* 可选：强制使用指定 IQ 文件；为空时由 RKAIQ 自动匹配。 */
    int width;                      /* RKAIQ prepare 使用的 sensor 输出宽度。 */
    int height;                     /* RKAIQ prepare 使用的 sensor 输出高度。 */
    int working_mode;               /* RKAIQ 工作模式，通常 0=normal，16=HDR2，32=HDR3。 */
} IspControllerSensorConfig;

/**
 * @brief ISP/RKAIQ 健康诊断配置。
 *
 * 该结构体集中保存 metas 回调停滞、error 回调累计阈值和后续自恢复策略开关。
 */
typedef struct {
    int check_enable;               /* 是否启用 ISP/RKAIQ 健康诊断。 */
    int meta_timeout_ms;            /* started 后 metas 回调允许停滞的最长时间；<=0 表示不检测。 */
    int max_error_count;            /* RKAIQ error 回调累计阈值；<=0 表示不检测。 */
    int restart_on_fault;           /* 预留：诊断异常后是否允许上层执行 ISP 协同重启。 */
} IspControllerHealthConfig;

/**
 * @brief ISP 控制器初始化和运行策略总配置。
 *
 * 该结构体由 mediaGateway 配置转换而来，按功能域组合生命周期、sensor/IQ、
 * 图像控制、健康诊断和低照度优化策略，避免所有配置字段平铺在同一层。
 */
typedef struct {
    IspControllerLifecycleConfig lifecycle; /* ISP/RKAIQ 启停和异常降级策略。 */
    IspControllerSensorConfig sensor;       /* sensor、IQ 文件和 RKAIQ prepare 参数。 */
    IspControllerImageControls controls;    /* RKAIQ start 后自动下发的运行时图像控制参数。 */
    IspControllerHealthConfig health;       /* ISP/RKAIQ 健康诊断配置。 */
    IspControllerLowLightConfig low_light;  /* 低照度自动优化策略配置。 */
} IspControllerConfig;

/**
 * @brief RKAIQ 生命周期和 sensor 基础状态。
 *
 * 该结构体用于描述 ISP 控制链路是否启用、是否完成 init/prepare/start，
 * 以及当前绑定的 sensor 名称和运行时长。
 */
typedef struct {
    int enabled;                    /* 配置层是否请求启用 ISP。 */
    int started;                    /* RKAIQ 是否已进入 started 状态。 */
    int initialized;                /* RKAIQ 是否已完成 init。 */
    int prepared;                   /* RKAIQ 是否已完成 prepare。 */
    char sensor_name[128];          /* 当前生效的 sensor media entity 名称。 */
    uint64_t start_ts_us;           /* RKAIQ start 成功时的单调时钟时间戳。 */
    uint64_t uptime_us;             /* RKAIQ started 后的运行时长。 */
} IspControllerLifecycleStatus;

/**
 * @brief RKAIQ 回调统计状态。
 *
 * 该结构体记录 metas 和 error 回调的累计次数、最近帧号、最近错误码和回调时间戳，
 * 供健康诊断判断 3A/ISP pipeline 是否停滞。
 */
typedef struct {
    uint64_t meta_frame_id;         /* 最近一次 metas 回调携带的 frame_id。 */
    uint64_t meta_callback_count;   /* metas 回调累计次数。 */
    uint64_t error_count;           /* RKAIQ error 回调累计次数。 */
    int last_error_code;            /* 最近一次 RKAIQ error code。 */
    uint64_t last_meta_ts_us;       /* 最近一次 metas 回调时间戳。 */
    uint64_t last_error_ts_us;      /* 最近一次 error 回调时间戳。 */
} IspControllerCallbackStatus;

/**
 * @brief 运行时图像控制下发状态。
 *
 * 该结构体记录图像控制功能是否启用、最近一次控制批次是否成功下发，以及成功下发的
 * 参数快照，方便日志和调试工具确认当前控制意图。
 */
typedef struct {
    int enabled;                    /* 配置层是否启用运行时图像控制参数。 */
    int applied;                    /* 最近一次运行时图像控制参数是否已成功下发。 */
    IspControllerImageControls controls; /* 最近一次成功下发的运行时图像控制参数快照。 */
} IspControllerControlStatus;

/**
 * @brief ISP/RKAIQ 健康诊断结果。
 *
 * 该结构体记录健康诊断是否启用、当前诊断状态、状态原因和 metas 停滞时间。
 * 它是对 lifecycle/callbacks 的二次计算结果。
 */
typedef struct {
    int enabled;                    /* 是否启用 ISP/RKAIQ 健康诊断。 */
    IspControllerHealthState state; /* 当前健康诊断状态。 */
    char reason[128];               /* 当前健康状态原因描述。 */
    uint64_t meta_stall_us;         /* 距离最近一次 metas 回调的时间；无回调时按 start 计算。 */
} IspControllerHealthStatus;

/**
 * @brief 低照度自动优化运行状态。
 *
 * 该结构体记录低照度策略是否启用、当前是否已经进入低照度模式、建议码率提升比例
 * 和最近一次状态切换原因。
 */
typedef struct {
    int enabled;                    /* 是否启用低照度自动优化策略。 */
    int active;                     /* 当前是否处于低照度优化模式。 */
    int bitrate_boost_percent;      /* 当前建议的码率提升百分比。 */
    int qp_delta;                   /* 当前建议的 FIXQP QP 调整量。 */
    char reason[128];               /* 最近一次低照度状态切换原因。 */
} IspControllerLowLightStatus;

/**
 * @brief AE 曝光查询状态。
 *
 * 该结构体承载 RKAIQ AE 查询结果，包括收敛状态、平均亮度、环境照度、曝光时间、
 * 各级增益和 ISO，是低照度检测和曝光诊断的主要数据来源。
 */
typedef struct {
    int valid;                      /* AE 查询结果是否有效。 */
    int converged;                  /* AE 是否收敛。 */
    int exp_max;                    /* AE 是否已达到曝光上限。 */
    float mean_luma;                /* 当前平均亮度。 */
    float luma_deviation;           /* 当前亮度偏差。 */
    float env_lux;                  /* 当前环境照度估算。 */
    float integration_time;         /* 当前线性曝光时间，单位由 RKAIQ SDK 定义。 */
    float analog_gain;              /* 当前模拟增益。 */
    float digital_gain;             /* 当前数字增益。 */
    float isp_dgain;                /* 当前 ISP 数字增益。 */
    int iso;                        /* 当前 ISO 估算值。 */
} IspControllerAeStatus;

/**
 * @brief AWB 白平衡查询状态。
 *
 * 该结构体承载 RKAIQ AWB 查询结果，包括色温、白平衡四通道 gain 和 LVValue，
 * 用于观察自动白平衡是否收敛以及当前色彩状态。
 */
typedef struct {
    int valid;                      /* AWB 查询结果是否有效。 */
    int converged;                  /* AWB 是否收敛。 */
    float cct;                      /* 当前色温估算。 */
    float ccri;                     /* 当前色温置信/辅助指标，含义由 RKAIQ SDK 定义。 */
    float wb_rgain;                 /* 当前白平衡 R gain。 */
    float wb_grgain;                /* 当前白平衡 GR gain。 */
    float wb_gbgain;                /* 当前白平衡 GB gain。 */
    float wb_bgain;                 /* 当前白平衡 B gain。 */
    uint32_t lv_value;              /* AWB 查询到的 LVValue。 */
} IspControllerAwbStatus;

/**
 * @brief ISP 控制器对外输出的聚合状态。
 *
 * 该结构体按功能域组合生命周期、回调、控制、健康、低照度、AE 和 AWB 状态，
 * 避免状态字段平铺过多，也便于上层按模块读取关注的数据。
 */
typedef struct {
    IspControllerLifecycleStatus lifecycle; /* RKAIQ 生命周期和 sensor 基础状态。 */
    IspControllerCallbackStatus callbacks;  /* RKAIQ error/metas 回调统计。 */
    IspControllerControlStatus control;     /* 运行时图像控制下发状态。 */
    IspControllerHealthStatus health;       /* ISP/RKAIQ 健康诊断状态。 */
    IspControllerLowLightStatus low_light;  /* 低照度自动优化状态。 */
    IspControllerAeStatus ae;               /* AE 曝光、亮度、增益查询状态。 */
    IspControllerAwbStatus awb;             /* AWB 色温、白平衡 gain 查询状态。 */
} IspControllerStatus;

/**
 * @brief ISP 控制器运行上下文。
 *
 * 该结构体由调用方持有，内部保存归一化配置、RKAIQ sys_ctx、生命周期标志、
 * 回调统计缓存和低照度策略状态。上层不应直接修改其中字段。
 */
typedef struct {
    IspControllerConfig config;     /* 生效后的 ISP 配置副本。 */
    void *sys_ctx;                  /* RKAIQ sysctl 上下文，实际类型为 rk_aiq_sys_ctx_t*。 */
    int initialized;                /* 是否已完成 rk_aiq_uapi2_sysctl_init。 */
    int prepared;                   /* 是否已完成 rk_aiq_uapi2_sysctl_prepare。 */
    int started;                    /* 是否已完成 rk_aiq_uapi2_sysctl_start。 */
    char resolved_sensor_name[128]; /* 最终使用的 sensor media entity 名称。 */
    pthread_mutex_t status_lock;    /* 保护回调线程和主线程之间共享的状态数据。 */
    int status_lock_ready;          /* status_lock 是否已初始化。 */
    uint64_t start_ts_us;           /* RKAIQ start 成功时的单调时钟时间戳。 */
    uint64_t meta_frame_id;         /* 最近一次 metas 回调携带的 frame_id。 */
    uint64_t meta_callback_count;   /* metas 回调累计次数。 */
    uint64_t error_count;           /* RKAIQ error 回调累计次数。 */
    int last_error_code;            /* 最近一次 RKAIQ error code。 */
    uint64_t last_meta_ts_us;       /* 最近一次 metas 回调时间戳。 */
    uint64_t last_error_ts_us;      /* 最近一次 error 回调时间戳。 */
    IspControllerImageControls last_controls; /* 最近一次成功下发的运行时图像控制参数快照。 */
    int controls_applied;           /* last_controls 是否包含有效的下发记录。 */
    int low_light_active;           /* 当前是否处于低照度优化模式。 */
    uint64_t low_light_last_switch_ts_us; /* 最近一次低照度模式切换时间。 */
    char low_light_reason[128];     /* 最近一次低照度状态切换原因。 */
} IspControllerCtx;

/**
 * @brief 初始化并启动 RKAIQ/ISP 控制链路。
 *
 * 当 config 为空或 config->lifecycle.enabled 为 0 时，该函数只清空 ctx 并返回成功。
 * 当 ENABLE_RKAIQ 未编译进工程时，会根据 fallback_on_error 决定返回成功
 * 还是失败。真实 RKAIQ 路径会依次执行 sensor 解析、可选 preInit、
 * sysctl init、prepare 和 start。
 *
 * @param ctx ISP 控制上下文，由调用者持有。
 * @param config ISP 初始化配置。
 * @return 0 表示成功或允许降级；-1 表示失败且不允许降级。
 */
int isp_controller_init(IspControllerCtx *ctx, const IspControllerConfig *config);

/**
 * @brief 停止并释放 RKAIQ/ISP 控制链路。
 *
 * 该函数允许重复调用。若 RKAIQ 已启动，则先 stop；若已初始化，则 deinit；
 * 最后清空上下文。
 *
 * @param ctx ISP 控制上下文。
 */
void isp_controller_deinit(IspControllerCtx *ctx);

/**
 * @brief 查询 RKAIQ/ISP 控制链路是否处于 started 状态。
 *
 * @param ctx ISP 控制上下文。
 * @return 1 表示已启动，0 表示未启动或 ctx 无效。
 */
int isp_controller_is_started(const IspControllerCtx *ctx);

/**
 * @brief 向已启动的 RKAIQ 上下文下发运行时图像控制参数。
 *
 * 整数类控制项使用 -1 表示不修改，曝光时间、曝光增益和手动白平衡 gain 使用 <=0 表示不修改。
 * 当 controls->enabled 为 0 时，该函数直接返回成功，不会触发任何 RKAIQ API 调用。
 *
 * @param ctx ISP 控制上下文。
 * @param controls 运行时图像控制参数批次。
 * @return 0 表示下发成功或无需处理；-1 表示参数无效或 RKAIQ API 调用失败。
 */
int isp_controller_apply_image_controls(IspControllerCtx *ctx, const IspControllerImageControls *controls);

/**
 * @brief 根据 AE 状态更新低照度优化策略。
 *
 * 该接口会根据 env_lux/mean_luma 和回滞阈值判断是否进入或退出低照度模式。
 * 进入低照度时可联动曝光、降噪和锐度；退出时恢复到自动曝光/自动降噪，锐度保持 IQ 默认。
 *
 * @param ctx ISP 控制上下文。
 * @param status 当前 ISP 状态，可传入 isp_controller_query_status 的结果；为 NULL 时函数内部查询。
 * @return 0 表示无需切换或切换成功；-1 表示状态查询失败或 RKAIQ 控制下发失败。
 */
int isp_controller_update_low_light(IspControllerCtx *ctx, IspControllerStatus *status);

/**
 * @brief 查询当前 ISP/RKAIQ 运行状态。
 *
 * 该接口会返回模块生命周期状态和回调计数。启用 RKAIQ 编译时，还会尝试
 * 通过 AE/AWB 查询接口获取曝光、增益、亮度、色温和白平衡信息。
 *
 * @param ctx ISP 控制上下文。
 * @param status 输出状态。
 * @return 0 表示成功，-1 表示参数无效。
 */
int isp_controller_query_status(IspControllerCtx *ctx, IspControllerStatus *status);

#ifdef __cplusplus
}
#endif

#endif
