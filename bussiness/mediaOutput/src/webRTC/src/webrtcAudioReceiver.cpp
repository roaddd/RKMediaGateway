#include "../inc/webrtcAudioReceiver.h"

#include "logger.h"

namespace rkmedia {
namespace webrtc {

/* RTP 校验完成后的零拷贝视图，只在 handlePacket() 调用期间有效。 */
struct WebRtcAudioRtpPacketView {
    uint8_t payloadType = 0;
    uint16_t sequenceNumber = 0;
    uint32_t rtpTimestamp = 0;
    uint32_t ssrc = 0;
    bool marker = false;
    const uint8_t *payload = nullptr;
    size_t payloadSize = 0;
};

/**
 * @brief 按网络字节序（大端序）读取 RTP 头中的 16 位整数。
 *
 * RTP 把多字节整数的高位字节放在低地址，例如字节序列 0x12、0x34 表示
 * 数值 0x1234。逐字节移位解析不依赖当前 CPU 的字节序和内存对齐方式。
 */
static uint16_t read_be16(const uint8_t *data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                 static_cast<uint16_t>(data[1]));
}

/**
 * @brief 按网络字节序（大端序）读取 RTP 头中的 32 位整数。
 *
 * 第 0 个字节是数值的最高 8 位，因此依次左移 24、16、8、0 位后合并。
 */
static uint32_t read_be32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

/**
 * @brief 根据 RTP/RTCP 复用规则识别 RTCP 包。
 *
 * RTP 与 RTCP 的前两个字节布局如下，bit7 为每个字节的最高位：
 *
 * RTP：
 *   第 0 字节
 *   | bit7-bit6 | bit5 | bit4 | bit3-bit0 |
 *   |     V     |  P   |  X   |    CC     |
 *
 *   第 1 字节
 *   | bit7 | bit6-bit0 |
 *   |  M   |    PT     |
 *
 *   V：RTP 版本号，当前固定为 2。
 *   P：是否携带尾部 padding。
 *   X：是否携带 RTP 扩展头。
 *   CC：固定头后面的 CSRC 数量。
 *   M：Marker 标志。
 *   PT：7 位 RTP Payload Type。
 *
 * RTCP：
 *   第 0 字节
 *   | bit7-bit6 | bit5 | bit4-bit0 |
 *   |     V     |  P   |  RC/FMT   |
 *
 *   第 1 字节
 *   | bit7-bit0  |
 *   | Packet Type|
 *
 *   RC/FMT：不同 RTCP 类型中的报告数量或反馈消息格式。
 *   Packet Type：完整的 8 位 RTCP 包类型，不包含 RTP 的 Marker 位。
 *
 * RTP/RTCP 使用同一端口时，RFC 5761 将完整第二字节 192～223 保留给 RTCP
 * 分类使用。因此这里直接判断 data[1]，不能先执行“& 0x7f”；否则会把 RTCP
 * Packet Type 错误地当成 RTP 的 7 位 PT。当 RTP 的 M=1 且 PT=64～95 时，组合后
 * 的完整第二字节也会落入 192～223，因此 RTP/RTCP 复用会话禁止分配这段 RTP PT。
 */
static bool is_rtcp_packet(const uint8_t *data, size_t size)
{
    static const uint8_t RTP_VERSION = 2;
    static const uint8_t RTP_VERSION_SHIFT = 6;
    static const uint8_t RTCP_PACKET_TYPE_MIN = 192;
    static const uint8_t RTCP_PACKET_TYPE_MAX = 223;
    uint8_t packetType = 0;

    if (data == nullptr || size < 2 || (data[0] >> RTP_VERSION_SHIFT) != RTP_VERSION) {
        return false;
    }
    packetType = data[1];
    return packetType >= RTCP_PACKET_TYPE_MIN && packetType <= RTCP_PACKET_TYPE_MAX;
}

/**
 * @brief 校验 RTP 可变头部并生成编码负载视图。
 *
 * 解析顺序严格对应 RTP 布局：固定头、CSRC、扩展头、负载、padding。函数只解析
 * 字节布局，不读取接收器配置，也不修改会话统计，便于将协议校验集中在一个位置。
 *
 * 字节序约定：
 * - Sequence Number、Timestamp、SSRC 和扩展长度是 RTP 头中的多字节整数，必须
 *   通过 read_be16()/read_be32() 按网络字节序解析。
 * - V/P/X/CC/M/PT 是单字节内的位字段，只需要移位或掩码，不存在字节序转换。
 * - Opus payload 是按位定义的压缩码流，不是主机整数数组，必须保持接收到的字节
 *   顺序原样交给 Opus 解码器，不能按 16 位或 32 位整数进行大小端交换。
 */
static MediaResult parse_rtp_packet(const uint8_t *data, size_t size, WebRtcAudioRtpPacketView &view)
{
    size_t headerSize = 0;
    size_t payloadSize = 0;
    size_t extensionSize = 0;
    uint8_t csrcCount = 0;
    uint8_t paddingSize = 0;
    uint16_t extensionWords = 0;

    if (data == nullptr || size < 12) {
        LOG_ERROR("[WEBRTC][AUDIO_RX] malformed RTP fixed header: data=%p size=%zu",
                  static_cast<const void *>(data),
                  size);
        return MEDIA_ERR_INVALID_PARAM;
    }
    if ((data[0] >> 6) != 2) {
        LOG_ERROR("[WEBRTC][AUDIO_RX] malformed RTP version: expected=2 actual=%u size=%zu",
                  static_cast<unsigned int>(data[0] >> 6),
                  size);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* CC 表示紧跟固定头的 CSRC 数量，每项固定占 4 字节。 */
    csrcCount = data[0] & 0x0f;
    headerSize = 12 + static_cast<size_t>(csrcCount) * 4;
    if (headerSize > size) {
        LOG_ERROR("[WEBRTC][AUDIO_RX] malformed RTP CSRC list: size=%zu header=%zu cc=%u",
                  size,
                  headerSize,
                  static_cast<unsigned int>(csrcCount));
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* X=1 时，扩展头包含 16 位 profile 和以 32 位字为单位的 16 位长度。 */
    if ((data[0] & 0x10) != 0) {
        if (headerSize + 4 > size) {
            LOG_ERROR("[WEBRTC][AUDIO_RX] malformed RTP extension header: size=%zu offset=%zu",
                      size,
                      headerSize);
            return MEDIA_ERR_INVALID_PARAM;
        }
        extensionWords = read_be16(data + headerSize + 2);
        extensionSize = 4 + static_cast<size_t>(extensionWords) * 4;
        if (extensionSize > size - headerSize) {
            LOG_ERROR("[WEBRTC][AUDIO_RX] malformed RTP extension: size=%zu offset=%zu words=%u",
                      size,
                      headerSize,
                      static_cast<unsigned int>(extensionWords));
            return MEDIA_ERR_INVALID_PARAM;
        }
        headerSize += extensionSize;
    }

    payloadSize = size - headerSize;
    /* P=1 时，最后一个字节给出包含自身在内的 padding 总长度。 */
    if ((data[0] & 0x20) != 0) {
        paddingSize = data[size - 1];
        if (paddingSize == 0 || static_cast<size_t>(paddingSize) > payloadSize) {
            LOG_ERROR("[WEBRTC][AUDIO_RX] malformed RTP padding: payload=%zu padding=%u",
                      payloadSize,
                      static_cast<unsigned int>(paddingSize));
            return MEDIA_ERR_INVALID_PARAM;
        }
        payloadSize -= paddingSize;
    }
    if (payloadSize == 0) {
        LOG_ERROR("[WEBRTC][AUDIO_RX] RTP packet rejected: empty audio payload size=%zu",
                  size);
        return MEDIA_ERR_INVALID_PARAM;
    }

    view.payloadType = data[1] & 0x7f;
    view.sequenceNumber = read_be16(data + 2);
    view.rtpTimestamp = read_be32(data + 4);
    view.ssrc = read_be32(data + 8);
    view.marker = (data[1] & 0x80) != 0;

    /*
     * payload 指向已经剔除 RTP 头、扩展头和 padding 后的原始 Opus packet。
     * Opus 解码器按字节和位读取 TOC、帧数量及熵编码数据；交换相邻字节会改变这些
     * 位的位置并破坏码流，因此这里只记录地址和长度，不做任何大小端转换。
     */
    view.payload = data + headerSize;
    view.payloadSize = payloadSize;
    return MEDIA_OK;
}

/** 根据当前 SSRC 和期望序号，更新连续、缺口、重复及乱序统计。 */
static void update_sequence_statistics(const WebRtcAudioRtpPacketView &view,
                                       WebRtcAudioReceiverStats &stats,
                                       uint16_t &expectedSequenceNumber)
{
    int16_t sequenceDelta = 0;

    if (!stats.hasRtpPacket || stats.lastSsrc != view.ssrc) {
        if (stats.hasRtpPacket) {
            ++stats.ssrcChanges;
        }
        expectedSequenceNumber = static_cast<uint16_t>(view.sequenceNumber + 1);
        return;
    }

    /* 以有符号 16 位差值比较序号，可以自然覆盖 65535 到 0 的 RTP 回绕。 */
    sequenceDelta = static_cast<int16_t>(view.sequenceNumber - expectedSequenceNumber);
    if (sequenceDelta == 0) {
        expectedSequenceNumber = static_cast<uint16_t>(view.sequenceNumber + 1);
    } else if (view.sequenceNumber == stats.lastSequenceNumber) {
        ++stats.duplicatePackets;
    } else if (sequenceDelta > 0) {
        stats.sequenceGapPackets += static_cast<uint16_t>(sequenceDelta);
        expectedSequenceNumber = static_cast<uint16_t>(view.sequenceNumber + 1);
    } else {
        ++stats.outOfOrderPackets;
    }
}

/** 保存会话协商结果和上层回调；序号状态由成员缺省初始化为零。 */
WebRtcAudioReceiver::WebRtcAudioReceiver(
    const WebRtcAudioReceiverConfig &config,
    const WebRtcIncomingAudioPacketCallback &callback)
{
    config_ = config;
    callback_ = callback;
}

/** 重新绑定协商参数，并清空上一条音频发送源的全部累计状态。 */
MediaResult WebRtcAudioReceiver::reset(
    const WebRtcAudioReceiverConfig &config,
    const WebRtcIncomingAudioPacketCallback &callback)
{
    if (config.sessionId <= 0 || config.codec == WEBRTC_AUDIO_CODEC_NONE ||
        config.payloadType > 127) {
        LOG_ERROR("[WEBRTC][AUDIO_RX] reset failed: session=%d codec=%d pt=%u",
                  config.sessionId,
                  static_cast<int>(config.codec),
                  static_cast<unsigned int>(config.payloadType));
        return MEDIA_ERR_INVALID_CONFIG;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    callback_ = callback;
    stats_ = WebRtcAudioReceiverStats();
    expectedSequenceNumber_ = 0;
    return MEDIA_OK;
}

/** 记录一个无法解析为有效 RTP 负载的输入包。 */
void WebRtcAudioReceiver::recordMalformedPacket()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.malformedPackets;
}

/** 记录 audio Track 上的 RTCP 控制包，RTCP 不进入编码负载回调。 */
void WebRtcAudioReceiver::recordRtcpPacket()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.rtcpPackets;
}

/**
 * @brief 接受已经完成字节布局校验的 RTP 包。
 *
 * 主要逻辑：
 * 1. 在锁内确认接收器已配置，并校验 RTP PT 与当前 audio m-line 的协商 PT 一致；
 *    PT 不匹配的包只计入异常统计，不进入音频包回调。
 * 2. 根据 SSRC 和 RTP 序号更新缺包、重复、乱序等估算统计，再记录有效包的字节数
 *    与最近收到的 SSRC、序号、时间戳。这里仅做观测，不对包排序或丢弃乱序包。
 * 3. 将解析出的头字段写入 packet，并把已协商格式的音频编码负载按原始字节顺序复制到其
 *    payload；这样 packet 不再依赖 Track 回调中的临时接收缓冲区。
 * 4. 将 callback_ 复制到输出参数。实际回调由 handlePacket() 在释放 mutex_ 后执行，
 *    避免上层回调重入接收器时发生死锁。
 */
MediaResult WebRtcAudioReceiver::acceptRtpPacket(
    const WebRtcAudioRtpPacketView &view,
    size_t packetSize,
    uint64_t arrivalTimeUs,
    WebRtcIncomingAudioPacket &packet,
    WebRtcIncomingAudioPacketCallback &callback)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (config_.sessionId <= 0 || config_.codec == WEBRTC_AUDIO_CODEC_NONE) {
        LOG_ERROR("[WEBRTC][AUDIO_RX] packet rejected: receiver not configured");
        return MEDIA_ERR_NOT_READY;
    }
    if (view.payloadType != config_.payloadType) {
        ++stats_.unexpectedPayloadTypePackets;
        LOG_ERROR("[WEBRTC][AUDIO_RX] unexpected RTP PT: session=%d expected=%u actual=%u",
                  config_.sessionId,
                  static_cast<unsigned int>(config_.payloadType),
                  static_cast<unsigned int>(view.payloadType));
        return MEDIA_ERR_UNSUPPORTED;
    }

    /* 序号统计只描述到达情况；即使检测到重复或乱序，本阶段仍交付有效 RTP 包。 */
    update_sequence_statistics(view, stats_, expectedSequenceNumber_);
    ++stats_.rtpPackets;
    stats_.rtpBytes += packetSize;
    stats_.payloadBytes += view.payloadSize;
    stats_.hasRtpPacket = true;
    stats_.lastSsrc = view.ssrc;
    stats_.lastSequenceNumber = view.sequenceNumber;
    stats_.lastRtpTimestamp = view.rtpTimestamp;

    /* 把协议头字段及负载转为拥有独立存储的上层对象，供后续解码链异步使用。 */
    packet.sessionId = config_.sessionId;
    packet.codec = config_.codec;
    packet.payloadType = view.payloadType;
    packet.ssrc = view.ssrc;
    packet.sequenceNumber = view.sequenceNumber;
    packet.rtpTimestamp = view.rtpTimestamp;
    packet.marker = view.marker;
    packet.arrivalTimeUs = arrivalTimeUs;
    packet.payload.assign(view.payload, view.payload + view.payloadSize);
    /* 此处仅取回调快照，不在锁内调用外部代码。 */
    callback = callback_;
    return MEDIA_OK;
}

/**
 * @brief 解析并交付一个 audio Track 消息。
 *
 * 主流程只负责编排四个步骤：识别 RTCP、解析 RTP、提交统计与所有权复制、锁外回调。
 * RTP 变长头处理和序号状态机分别封装在实现文件内部，调用者仍只面对一个接口。
 */
MediaResult WebRtcAudioReceiver::handlePacket(const uint8_t *data,
                                               size_t size,
                                               uint64_t arrivalTimeUs)
{
    WebRtcAudioRtpPacketView view;
    WebRtcIncomingAudioPacket packet;
    WebRtcIncomingAudioPacketCallback callback;
    MediaResult result = MEDIA_ERR;

    if (data == nullptr || size < 2) {
        recordMalformedPacket();
        LOG_ERROR("[WEBRTC][AUDIO_RX] packet rejected: data=%p size=%zu",
                  static_cast<const void *>(data),
                  size);
        return MEDIA_ERR_INVALID_PARAM;
    }

    /* rtcp包 */
    if (is_rtcp_packet(data, size)) {
        recordRtcpPacket();
        return MEDIA_OK;
    }

    /* 解析固定头、CSRC、扩展头和 padding */
    result = parse_rtp_packet(data, size, view);
    if (result != MEDIA_OK) {
        recordMalformedPacket();
        return result;
    }

    /* 校验 PT、更新统计并构造上层音频包 */
    result = acceptRtpPacket(view, size, arrivalTimeUs, packet, callback);
    if (result != MEDIA_OK) {
        return result;
    }
    if (callback) {
        callback(packet);
    }
    return MEDIA_OK;
}

void WebRtcAudioReceiver::getStats(WebRtcAudioReceiverStats &stats) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats = stats_;
}

} // namespace webrtc
} // namespace rkmedia
