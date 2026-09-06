#include "audioEncoderManager.h"

#include "logger.h"

#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rkmedia {

/**
 * @description: Manager 私有实现，隐藏 STL 容器和编码器实例所有权。
 */
class AudioEncoderManager::Impl {
public:
    struct Group {
        AudioEncoderGroupId id;
        AudioEncoderKey key;
        AudioEncoderConfig config;
        std::unique_ptr<AudioEncoder> encoder;
        std::vector<int16_t> pcm_buffer;

        Group(AudioEncoderGroupId group_id,
              const AudioEncoderKey &group_key,
              const AudioEncoderConfig &group_config,
              std::unique_ptr<AudioEncoder> group_encoder)
            : id(group_id),
              key(group_key),
              config(group_config),
              encoder(std::move(group_encoder)),
              pcm_buffer()
        {
        }
    };

    typedef std::unordered_map<AudioEncoderKey,
                               AudioEncoderGroupId,
                               AudioEncoderKeyHash> KeyToGroupMap;

    KeyToGroupMap key_to_group;
    std::vector<std::unique_ptr<Group> > groups;
};

AudioEncoderManager::AudioEncoderManager()
    : impl_(new (std::nothrow) Impl())
{
    if (!impl_)
        LOG_ERROR("AudioEncoderManager construct failed: allocate implementation");
}

AudioEncoderManager::~AudioEncoderManager()
{
    clear();
}

/**
 * @description: 归一化配置并按 AudioEncoderKey 查重；仅首次出现的 Key 创建编码器。
 */
int AudioEncoderManager::registerGroup(const AudioEncoderConfig &config,
                                       AudioEncoderGroupRegistration *registration)
{
    AudioEncoderConfig normalized;
    AudioEncoderKey key;
    Impl::KeyToGroupMap::iterator existing = Impl::KeyToGroupMap::iterator();
    std::unique_ptr<AudioEncoder> encoder_instance;
    std::unique_ptr<Impl::Group> group;
    std::pair<Impl::KeyToGroupMap::iterator, bool> insert_result;
    AudioEncoderGroupId group_id = kInvalidAudioEncoderGroupId;
    size_t old_group_count = 0;
    bool group_pushed = false;

    if (!registration) {
        LOG_ERROR("AudioEncoderManager registerGroup failed: registration is NULL codec=%d", config.codec);
        return -1;
    }
    *registration = AudioEncoderGroupRegistration();
    if (!impl_) {
        LOG_ERROR("AudioEncoderManager registerGroup failed: implementation is NULL codec=%d", config.codec);
        return -1;
    }
    if (normalizeAudioEncoderConfig(config, &normalized) != 0) {
        LOG_ERROR("AudioEncoderManager registerGroup failed: normalize codec=%d", config.codec);
        return -1;
    }
    if (makeAudioEncoderKey(normalized, &key) != 0) {
        LOG_ERROR("AudioEncoderManager registerGroup failed: make key codec=%d", normalized.codec);
        return -1;
    }

    existing = impl_->key_to_group.find(key);
    if (existing != impl_->key_to_group.end()) {
        registration->group_id = existing->second;
        registration->reused = true;
        LOG_INFO("audio encoder group reused: group_id=%llu codec=%d rate=%d channels=%d",
                 static_cast<unsigned long long>(registration->group_id),
                 normalized.codec,
                 normalized.sample_rate,
                 normalized.encoder_channels);
        return 0;
    }

    encoder_instance = createAudioEncoder(normalized);
    if (!encoder_instance) {
        LOG_ERROR("AudioEncoderManager registerGroup failed: create encoder codec=%d rate=%d channels=%d",
                  normalized.codec,
                  normalized.sample_rate,
                  normalized.encoder_channels);
        return -1;
    }

    group_id = static_cast<AudioEncoderGroupId>(impl_->groups.size()) + 1;
    group.reset(new (std::nothrow) Impl::Group(group_id,
                                               key,
                                               normalized,
                                               std::move(encoder_instance)));
    if (!group) {
        LOG_ERROR("AudioEncoderManager registerGroup failed: allocate group id=%llu codec=%d",
                  static_cast<unsigned long long>(group_id),
                  normalized.codec);
        return -1;
    }

    /* 两个容器必须作为一个逻辑事务更新，任一步异常都回退新加入的编码组。 */
    old_group_count = impl_->groups.size();
    try {
        impl_->groups.push_back(std::move(group));
        group_pushed = true;
        insert_result = impl_->key_to_group.insert(std::make_pair(key, group_id));
    } catch (const std::bad_alloc &) {
        if (group_pushed && impl_->groups.size() > old_group_count)
            impl_->groups.pop_back();
        LOG_ERROR("AudioEncoderManager registerGroup failed: container allocation group_id=%llu codec=%d",
                  static_cast<unsigned long long>(group_id),
                  normalized.codec);
        return -1;
    }

    if (!insert_result.second) {
        impl_->groups.pop_back();
        registration->group_id = insert_result.first->second;
        registration->reused = true;
        LOG_INFO("audio encoder group reused after insert: group_id=%llu codec=%d",
                 static_cast<unsigned long long>(registration->group_id),
                 normalized.codec);
        return 0;
    }

    registration->group_id = group_id;
    registration->reused = false;
    LOG_INFO("audio encoder group created: group_id=%llu codec=%d rate=%d channels=%d unique_groups=%zu",
             static_cast<unsigned long long>(group_id),
             normalized.codec,
             normalized.sample_rate,
             normalized.encoder_channels,
             impl_->groups.size());
    return 0;
}

size_t AudioEncoderManager::groupCount() const
{
    if (!impl_) {
        LOG_ERROR("AudioEncoderManager groupCount failed: implementation is NULL");
        return 0;
    }
    return impl_->groups.size();
}

AudioEncoder *AudioEncoderManager::encoder(AudioEncoderGroupId group_id)
{
    size_t index = 0;

    if (!impl_) {
        LOG_ERROR("AudioEncoderManager encoder failed: implementation is NULL group_id=%llu",
                  static_cast<unsigned long long>(group_id));
        return NULL;
    }
    if (group_id == kInvalidAudioEncoderGroupId || group_id > impl_->groups.size()) {
        LOG_ERROR("AudioEncoderManager encoder failed: invalid group_id=%llu group_count=%zu",
                  static_cast<unsigned long long>(group_id),
                  impl_->groups.size());
        return NULL;
    }
    index = static_cast<size_t>(group_id - 1);
    if (!impl_->groups[index] || !impl_->groups[index]->encoder) {
        LOG_ERROR("AudioEncoderManager encoder failed: group unavailable group_id=%llu",
                  static_cast<unsigned long long>(group_id));
        return NULL;
    }
    return impl_->groups[index]->encoder.get();
}

const AudioEncoderConfig *AudioEncoderManager::config(AudioEncoderGroupId group_id) const
{
    size_t index = 0;

    if (!impl_) {
        LOG_ERROR("AudioEncoderManager config failed: implementation is NULL group_id=%llu",
                  static_cast<unsigned long long>(group_id));
        return NULL;
    }
    if (group_id == kInvalidAudioEncoderGroupId || group_id > impl_->groups.size()) {
        LOG_ERROR("AudioEncoderManager config failed: invalid group_id=%llu group_count=%zu",
                  static_cast<unsigned long long>(group_id),
                  impl_->groups.size());
        return NULL;
    }
    index = static_cast<size_t>(group_id - 1);
    if (!impl_->groups[index]) {
        LOG_ERROR("AudioEncoderManager config failed: group unavailable group_id=%llu",
                  static_cast<unsigned long long>(group_id));
        return NULL;
    }
    return &impl_->groups[index]->config;
}

int AudioEncoderManager::groupAt(size_t index,
                                 AudioEncoderGroupId *group_id,
                                 const AudioEncoderConfig **group_config) const
{
    if (!group_id || !group_config) {
        LOG_ERROR("AudioEncoderManager groupAt failed: invalid output index=%zu group_id=%p config=%p",
                  index,
                  static_cast<void *>(group_id),
                  static_cast<const void *>(group_config));
        return -1;
    }
    *group_id = kInvalidAudioEncoderGroupId;
    *group_config = NULL;
    if (!impl_) {
        LOG_ERROR("AudioEncoderManager groupAt failed: implementation is NULL index=%zu", index);
        return -1;
    }
    if (index >= impl_->groups.size() || !impl_->groups[index]) {
        LOG_ERROR("AudioEncoderManager groupAt failed: invalid index=%zu group_count=%zu",
                  index,
                  impl_->groups.size());
        return -1;
    }
    *group_id = impl_->groups[index]->id;
    *group_config = &impl_->groups[index]->config;
    return 0;
}

/**
 * @description: 在管理器内部完成采集 PCM 到目标编码 PCM 的同步适配和编码。
 */
int AudioEncoderManager::encode(AudioEncoderGroupId group_id,
                                const AudioEncoderPcmInput &input,
                                AudioEncoderOutput *output)
{
    Impl::Group *group = NULL;
    const int16_t *encoder_pcm = NULL;
    size_t group_index = 0;
    size_t output_sample_count = 0;
    int64_t sample_sum = 0;
    int rate_ratio = 0;
    int encoder_samples = 0;
    int source_channel = 0;
    int output_channel = 0;
    int sample_index = 0;
    int ratio_index = 0;

    /* 第一步：清空输出并校验原始 PCM 的地址、采样数、采样率及声道数。 */
    if (output)
        *output = AudioEncoderOutput();
    if (!impl_ || !input.data || input.samples_per_channel <= 0 || input.sample_rate <= 0 ||
        input.channels <= 0 || !output) {
        LOG_ERROR("AudioEncoderManager encode failed: impl=%p pcm=%p samples=%d rate=%d channels=%d output=%p",
                  static_cast<void *>(impl_.get()),
                  static_cast<const void *>(input.data),
                  input.samples_per_channel,
                  input.sample_rate,
                  input.channels,
                  static_cast<void *>(output));
        return -1;
    }

    /* 第二步：根据稳定的运行时 ID 找到去重后的编码组及其目标编码参数。 */
    if (group_id == kInvalidAudioEncoderGroupId || group_id > impl_->groups.size()) {
        LOG_ERROR("AudioEncoderManager encode failed: invalid group_id=%llu group_count=%zu",
                  static_cast<unsigned long long>(group_id),
                  impl_->groups.size());
        return -1;
    }
    group_index = static_cast<size_t>(group_id - 1);
    group = impl_->groups[group_index].get();
    if (!group || !group->encoder) {
        LOG_ERROR("AudioEncoderManager encode failed: group unavailable group_id=%llu",
                  static_cast<unsigned long long>(group_id));
        return -1;
    }

    /*
     * 第三步：计算采集采样率到编码采样率的整数降采样比例。
     * 当前不支持升采样和非整数倍变采样，单次输入采样数也必须能被比例整除。
     */
    if (input.sample_rate < group->config.sample_rate ||
        input.sample_rate % group->config.sample_rate != 0) {
        LOG_ERROR("AudioEncoderManager encode failed: unsupported rate conversion group_id=%llu input_rate=%d encoder_rate=%d",
                  static_cast<unsigned long long>(group_id),
                  input.sample_rate,
                  group->config.sample_rate);
        return -1;
    }
    rate_ratio = input.sample_rate / group->config.sample_rate;
    if (rate_ratio <= 0 || input.samples_per_channel % rate_ratio != 0) {
        LOG_ERROR("AudioEncoderManager encode failed: unaligned samples group_id=%llu samples=%d ratio=%d",
                  static_cast<unsigned long long>(group_id),
                  input.samples_per_channel,
                  rate_ratio);
        return -1;
    }

    /*
     * 第四步：确定声道适配方式。目标为单声道时选择 capture_channel_index；
     * 目标为多声道时要求采集声道数完全一致，不在这里执行混音或声道扩展。
     */
    if (group->config.encoder_channels == 1) {
        if (group->config.capture_channel_index < 0 ||
            group->config.capture_channel_index >= input.channels) {
            LOG_ERROR("AudioEncoderManager encode failed: invalid input channel group_id=%llu selected=%d capture_channels=%d",
                      static_cast<unsigned long long>(group_id),
                      group->config.capture_channel_index,
                      input.channels);
            return -1;
        }
    } else if (group->config.encoder_channels != input.channels) {
        LOG_ERROR("AudioEncoderManager encode failed: unsupported channel conversion group_id=%llu capture=%d encoder=%d",
                  static_cast<unsigned long long>(group_id),
                  input.channels,
                  group->config.encoder_channels);
        return -1;
    }

    encoder_samples = input.samples_per_channel / rate_ratio;

    /* 第五步：采样率和声道均一致时直接引用输入 PCM，避免分配和复制。 */
    if (rate_ratio == 1 && group->config.encoder_channels == input.channels) {
        encoder_pcm = input.data;
    } else {
        /*
         * 需要声道选择或降采样时，复用编码组私有缓冲区保存转换结果。
         * 每个运行时编码组只由当前音频 worker 串行调用，因此该缓冲区无需加锁。
         */
        output_sample_count = static_cast<size_t>(encoder_samples) *
                              static_cast<size_t>(group->config.encoder_channels);
        try {
            group->pcm_buffer.resize(output_sample_count);
        } catch (const std::bad_alloc &) {
            LOG_ERROR("AudioEncoderManager encode failed: resize PCM group_id=%llu samples=%zu",
                      static_cast<unsigned long long>(group_id),
                      output_sample_count);
            return -1;
        }

        /*
         * 整数倍降采样时，对每个目标样本覆盖的输入样本做平均。
         * 这同时完成基础低通处理，避免直接抽取每第 N 个样本产生明显混叠。
        */
        for (sample_index = 0; sample_index < encoder_samples; ++sample_index) {
            for (output_channel = 0;
                 output_channel < group->config.encoder_channels;
                 ++output_channel) {
                if (group->config.encoder_channels == 1) {
                    /* 单声道编码：读取配置指定的一个采集声道。 */
                    source_channel = group->config.capture_channel_index;
                } else {
                    /* 多声道编码：输入和输出声道数相同，按声道下标原样映射。 */
                    source_channel = output_channel;
                }
                sample_sum = 0;
                for (ratio_index = 0; ratio_index < rate_ratio; ++ratio_index) {
                    sample_sum += input.data[((sample_index * rate_ratio) + ratio_index) * input.channels + source_channel];
                }
                group->pcm_buffer[static_cast<size_t>(sample_index) * static_cast<size_t>(group->config.encoder_channels) + static_cast<size_t>(output_channel)] =
                    static_cast<int16_t>(sample_sum / rate_ratio);
            }
        }
        encoder_pcm = group->pcm_buffer.data();
    }

    /* 第六步：将适配后的 PCM 同步送入具体编码器，输出数据由编码器实例持有。 */
    if (group->encoder->encode(encoder_pcm, encoder_samples, input.pts_us, output) != 0) {
        LOG_ERROR("AudioEncoderManager encode failed: codec group_id=%llu samples=%d rate=%d channels=%d",
                  static_cast<unsigned long long>(group_id),
                  encoder_samples,
                  group->config.sample_rate,
                  group->config.encoder_channels);
        return -1;
    }
    return 0;
}

void AudioEncoderManager::clear()
{
    if (!impl_)
        return;
    impl_->key_to_group.clear();
    impl_->groups.clear();
}

} // namespace rkmedia

struct AudioEncoderManagerHandle {
    rkmedia::AudioEncoderManager manager;
};

static rkmedia::AudioEncoderConfig make_cpp_config(const AudioEncoderParams *params)
{
    rkmedia::AudioEncoderConfig config;

    if (!params)
    {
        LOG_ERROR("make_cpp_config failed: params is NULL");
        return config;
    }

    config.codec = params->codec;
    config.sample_rate = params->sample_rate;
    config.encoder_channels = params->channels;
    config.max_samples_per_frame = params->max_samples_per_frame;
    config.capture_channel_index = params->input_channel;
    /* codec 是联合体的判别字段，只读取当前编码格式对应的专用参数。 */
    if (params->codec == MEDIA_CODEC_AAC) {
        config.aac.bitrate = params->codec_params.aac.bitrate;
        config.aac.profile = params->codec_params.aac.profile;
    } else if (params->codec == MEDIA_CODEC_OPUS) {
        config.opus.bitrate = params->codec_params.opus.bitrate;
        config.opus.complexity = params->codec_params.opus.complexity;
        config.opus.enable_vbr = params->codec_params.opus.enable_vbr;
        config.opus.enable_fec = params->codec_params.opus.enable_fec;
        config.opus.enable_dtx = params->codec_params.opus.enable_dtx;
        config.opus.packet_loss_percent = params->codec_params.opus.packet_loss_percent;
        config.opus.application = params->codec_params.opus.application;
        config.opus.max_packet_bytes = params->codec_params.opus.max_packet_bytes;
    }
    return config;
}

static void fill_c_params(const rkmedia::AudioEncoderConfig &config,
                          AudioEncoderParams *params)
{
    if (!params)
        return;
    params->codec = config.codec;
    params->sample_rate = config.sample_rate;
    params->channels = config.encoder_channels;
    params->max_samples_per_frame = config.max_samples_per_frame;
    params->input_channel = config.capture_channel_index;
    /* 只回填当前编码格式真正生效的私有参数，避免调用方误读无关默认值。 */
    if (config.codec == MEDIA_CODEC_AAC) {
        params->codec_params.aac.bitrate = config.aac.bitrate;
        params->codec_params.aac.profile = config.aac.profile;
    } else if (config.codec == MEDIA_CODEC_OPUS) {
        params->codec_params.opus.bitrate = config.opus.bitrate;
        params->codec_params.opus.complexity = config.opus.complexity;
        params->codec_params.opus.enable_vbr = config.opus.enable_vbr;
        params->codec_params.opus.enable_fec = config.opus.enable_fec;
        params->codec_params.opus.enable_dtx = config.opus.enable_dtx;
        params->codec_params.opus.packet_loss_percent = config.opus.packet_loss_percent;
        params->codec_params.opus.application = config.opus.application;
        params->codec_params.opus.max_packet_bytes = config.opus.max_packet_bytes;
    }
}

extern "C" int audio_encoder_manager_create(AudioEncoderManagerHandle **handle)
{
    AudioEncoderManagerHandle *created = NULL;

    if (!handle) {
        LOG_ERROR("audio_encoder_manager_create failed: handle is NULL");
        return -1;
    }
    *handle = NULL;
    created = new (std::nothrow) AudioEncoderManagerHandle();
    if (!created) {
        LOG_ERROR("audio_encoder_manager_create failed: allocate manager handle");
        return -1;
    }
    *handle = created;
    return 0;
}

extern "C" void audio_encoder_manager_destroy(AudioEncoderManagerHandle *handle)
{
    delete handle;
}

extern "C" int audio_encoder_manager_register(AudioEncoderManagerHandle *handle,
                                                const AudioEncoderParams *params,
                                                AudioEncoderRuntimeGroupId *group_id,
                                                int *reused)
{
    rkmedia::AudioEncoderConfig config;
    rkmedia::AudioEncoderGroupRegistration registration;

    if (!handle || !params || !group_id || !reused) {
        LOG_ERROR("audio_encoder_manager_register failed: handle=%p params=%p group_id=%p reused=%p",
                  static_cast<void *>(handle),
                  static_cast<const void *>(params),
                  static_cast<void *>(group_id),
                  static_cast<void *>(reused));
        return -1;
    }
    *group_id = AUDIO_ENCODER_INVALID_GROUP_ID;
    *reused = 0;
    config = make_cpp_config(params);
    if (handle->manager.registerGroup(config, &registration) != 0) {
        LOG_ERROR("audio_encoder_manager_register failed: codec=%d rate=%d channels=%d",
                  params->codec,
                  params->sample_rate,
                  params->channels);
        return -1;
    }
    *group_id = registration.group_id;
    *reused = registration.reused ? 1 : 0;
    return 0;
}

extern "C" size_t audio_encoder_manager_group_count(const AudioEncoderManagerHandle *handle)
{
    if (!handle) {
        LOG_ERROR("audio_encoder_manager_group_count failed: handle is NULL");
        return 0;
    }
    return handle->manager.groupCount();
}

extern "C" int audio_encoder_manager_group_at(const AudioEncoderManagerHandle *handle,
                                                size_t index,
                                                AudioEncoderRuntimeGroupId *group_id,
                                                AudioEncoderParams *params)
{
    const rkmedia::AudioEncoderConfig *config = NULL;
    rkmedia::AudioEncoderGroupId cpp_group_id = rkmedia::kInvalidAudioEncoderGroupId;

    if (!handle || !group_id || !params) {
        LOG_ERROR("audio_encoder_manager_group_at failed: handle=%p index=%zu group_id=%p params=%p",
                  static_cast<const void *>(handle),
                  index,
                  static_cast<void *>(group_id),
                  static_cast<void *>(params));
        return -1;
    }
    *group_id = AUDIO_ENCODER_INVALID_GROUP_ID;
    *params = AudioEncoderParams();
    if (handle->manager.groupAt(index, &cpp_group_id, &config) != 0 || !config) {
        LOG_ERROR("audio_encoder_manager_group_at failed: lookup index=%zu", index);
        return -1;
    }
    *group_id = cpp_group_id;
    fill_c_params(*config, params);
    return 0;
}

extern "C" int audio_encoder_manager_encode(AudioEncoderManagerHandle *handle,
                                              AudioEncoderRuntimeGroupId group_id,
                                              const AudioEncoderPcmInput *input,
                                              AudioEncoderOutput *output)
{
    if (!handle || !input || !input->data || input->samples_per_channel <= 0 ||
        input->sample_rate <= 0 || input->channels <= 0 || !output) {
        LOG_ERROR("audio_encoder_manager_encode failed: handle=%p group_id=%llu input=%p pcm=%p samples=%d rate=%d channels=%d output=%p",
                  static_cast<void *>(handle),
                  static_cast<unsigned long long>(group_id),
                  static_cast<const void *>(input),
                  input ? static_cast<const void *>(input->data) : NULL,
                  input ? input->samples_per_channel : 0,
                  input ? input->sample_rate : 0,
                  input ? input->channels : 0,
                  static_cast<void *>(output));
        return -1;
    }
    *output = AudioEncoderOutput();
    if (handle->manager.encode(group_id, *input, output) != 0) {
        LOG_ERROR("audio_encoder_manager_encode failed: group_id=%llu samples=%d pts=%llu",
                  static_cast<unsigned long long>(group_id),
                  input->samples_per_channel,
                  static_cast<unsigned long long>(input->pts_us));
        return -1;
    }
    return 0;
}
