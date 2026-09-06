#ifndef __AUDIO_ENCODER_MANAGER_H__
#define __AUDIO_ENCODER_MANAGER_H__

#include "audioEncoderInternal.h"

namespace rkmedia {

typedef uint64_t AudioEncoderGroupId;

const AudioEncoderGroupId kInvalidAudioEncoderGroupId = 0;

/**
 * @description: 注册编码配置后的去重结果。
 */
struct AudioEncoderGroupRegistration {
    AudioEncoderGroupId group_id; /* 实际复用或新建的运行期编码组 ID。 */
    bool reused;                  /* true 表示复用了参数完全相同的现有编码组。 */

    AudioEncoderGroupRegistration()
        : group_id(kInvalidAudioEncoderGroupId), reused(false)
    {
    }
};

/**
 * @description: 统一音频编码组管理器。
 *
 * 管理器在启动配置阶段注册编码组：每份配置先归一化并生成 AudioEncoderKey，
 * 相同 Key 只创建一个 AudioEncoder 实例。当前类不负责协议输出绑定和线程调度，
 * 配置阶段完成注册后由单个音频 worker 访问，不支持并发修改编码组。
 */
class AudioEncoderManager {
public:
    AudioEncoderManager();
    ~AudioEncoderManager();

    /**
     * @description: 注册一个编码配置，相同有效参数会复用已有编码组。
     * @param config 原始编码配置。
     * @param registration 返回编码组 ID 和是否发生复用。
     * @return 0 成功，-1 表示参数、内存分配或编码器初始化失败。
     */
    int registerGroup(const AudioEncoderConfig &config,
                      AudioEncoderGroupRegistration *registration);

    /** @description: 返回去重后的实际编码组数量。 */
    size_t groupCount() const;

    /**
     * @description: 按运行期 ID 查询编码器实例，实例所有权仍属于管理器。
     * @return 找到时返回编码器指针，否则返回 NULL 并打印错误日志。
     */
    AudioEncoder *encoder(AudioEncoderGroupId group_id);

    /**
     * @description: 按运行期 ID 查询归一化后的编码配置。
     * @return 找到时返回配置指针，否则返回 NULL 并打印错误日志。
     */
    const AudioEncoderConfig *config(AudioEncoderGroupId group_id) const;

    /** @description: 按稳定顺序枚举去重后的实际编码组及其归一化参数。 */
    int groupAt(size_t index,
                AudioEncoderGroupId *group_id,
                const AudioEncoderConfig **config) const;

    /**
     * @description: 根据编码组参数完成 PCM 声道选择、整数倍降采样并同步编码。
     * @return 0 成功，-1 表示输入、转换比例或底层编码失败。
     */
    int encode(AudioEncoderGroupId group_id,
               const AudioEncoderPcmInput &input,
               AudioEncoderOutput *output);

    /** @description: 销毁全部去重编码组和底层编码器实例。 */
    void clear();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    AudioEncoderManager(const AudioEncoderManager &);
    AudioEncoderManager &operator=(const AudioEncoderManager &);
};

} // namespace rkmedia

#endif
