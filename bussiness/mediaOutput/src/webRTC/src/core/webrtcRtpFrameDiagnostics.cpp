/**
 * @file webrtcRtpFrameDiagnostics.cpp
 * @brief 实现 H.264 编码帧到 packetizer RTP 输出的帧级关联统计。
 */
#include "webrtcRtpFrameDiagnostics.h"

#include <limits>
#include <rtc/rtp.hpp>

#include "logger.h"

namespace rkmedia {
namespace webrtc {

static const size_t WEBRTC_RTP_FIXED_HEADER_BYTES = 12;
static const size_t WEBRTC_RTP_EXTENSION_HEADER_BYTES = 4;
static const size_t WEBRTC_RTP_LARGE_FRAME_LOG_BYTES = 64 * 1024;

/**
 * @brief 安全计算 RTP 头总长度。
 * @param[in] data RTP 包首地址。
 * @param[in] size RTP 包总字节数。
 * @param[out] headerBytes RTP 固定头、CSRC 与扩展头的总字节数。
 * @retval true RTP 头长度合法且未越过输入边界。
 * @retval false 输入过短或扩展头声明长度越界。
 */
static bool parse_rtp_header_size(const uint8_t *data, size_t size, size_t *headerBytes)
{
    size_t calculatedBytes;
    size_t extensionWords;
    size_t extensionBytes;
    uint8_t csrcCount;
    bool hasExtension;

    if (!data || !headerBytes || size < WEBRTC_RTP_FIXED_HEADER_BYTES) {
        return false;
    }

    csrcCount = static_cast<uint8_t>(data[0] & 0x0f);
    hasExtension = (data[0] & 0x10) != 0;
    calculatedBytes = WEBRTC_RTP_FIXED_HEADER_BYTES + static_cast<size_t>(csrcCount) * 4;
    if (calculatedBytes > size) {
        return false;
    }
    if (!hasExtension) {
        *headerBytes = calculatedBytes;
        return true;
    }
    if (calculatedBytes + WEBRTC_RTP_EXTENSION_HEADER_BYTES > size) {
        return false;
    }

    /* RTP 扩展头的 length 字段使用网络字节序，单位为 32 bit word。 */
    extensionWords = (static_cast<size_t>(data[calculatedBytes + 2]) << 8) |
                     static_cast<size_t>(data[calculatedBytes + 3]);
    if (extensionWords >
        (std::numeric_limits<size_t>::max() - WEBRTC_RTP_EXTENSION_HEADER_BYTES) / 4) {
        return false;
    }
    extensionBytes = WEBRTC_RTP_EXTENSION_HEADER_BYTES + extensionWords * 4;
    if (extensionBytes > size - calculatedBytes) {
        return false;
    }

    *headerBytes = calculatedBytes + extensionBytes;
    return true;
}

WebRtcRtpFrameDiagnostics::WebRtcRtpFrameDiagnostics(const std::string &sessionId,
                                                     uint8_t payloadType)
    : sessionId_(sessionId),
      payloadType_(payloadType),
      frameActive_(false)
{
}

/** 开始一个编码帧到 RTP 分片的关联事务。 */
void WebRtcRtpFrameDiagnostics::beginFrame(uint64_t frameId,
                                           uint64_t ptsUs,
                                           size_t encodedBytes,
                                           bool keyFrame)
{
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

    lock.lock();
    if (frameActive_) {
        /* 理论上 sendFrame 同步串行；出现重入时先保留上一帧，避免静默覆盖诊断证据。 */
        ++stats_.overlappingFrames;
        currentFrame_.sendAccepted = false;
        commitCurrentFrameLocked();
    }

    currentFrame_ = WebRtcRtpFrameStats();
    currentFrame_.valid = true;
    currentFrame_.frameId = frameId;
    currentFrame_.ptsUs = ptsUs;
    currentFrame_.encodedBytes = static_cast<uint64_t>(encodedBytes);
    currentFrame_.keyFrame = keyFrame;
    frameActive_ = true;
}

/** 完成当前帧事务，并合并 Track 调用结果。 */
void WebRtcRtpFrameDiagnostics::finishFrame(bool sendAccepted,
                                            uint64_t sendCallUs,
                                            uint64_t bufferedAmount)
{
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

    lock.lock();
    if (!frameActive_) {
        LOG_WARN("[WEBRTC_RTP_FRAME] session=%s finish without active frame",
                 sessionId_.c_str());
        return;
    }

    currentFrame_.sendAccepted = sendAccepted;
    currentFrame_.sendCallUs = sendCallUs;
    currentFrame_.bufferedAmount = bufferedAmount;
    commitCurrentFrameLocked();
}

/** 解析 packetizer 输出，但不修改消息集合和后续发送回调。 */
void WebRtcRtpFrameDiagnostics::outgoing(rtc::message_vector &messages,
                                         const rtc::message_callback &send)
{
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
    rtc::message_ptr message;
    const rtc::RtpHeader *header;
    const uint8_t *data;
    size_t index;
    size_t headerBytes;
    uint16_t sequence;
    uint16_t expectedSequence;
    uint32_t timestamp;
    bool firstValidPacket;

    (void)send;
    lock.lock();
    if (!frameActive_) {
        ++stats_.orphanBatches;
        return;
    }

    firstValidPacket = currentFrame_.rtpPackets == 0;
    for (index = 0; index < messages.size(); ++index) {
        message = messages[index];
        header = NULL;
        data = NULL;
        headerBytes = 0;
        sequence = 0;
        expectedSequence = 0;
        timestamp = 0;

        if (!message || message->type != rtc::Message::Binary ||
            message->size() < WEBRTC_RTP_FIXED_HEADER_BYTES) {
            ++currentFrame_.parseFailures;
            continue;
        }
        data = reinterpret_cast<const uint8_t *>(message->data());
        header = reinterpret_cast<const rtc::RtpHeader *>(data);
        if (header->version() != 2 || header->payloadType() != payloadType_ ||
            !parse_rtp_header_size(data, message->size(), &headerBytes)) {
            ++currentFrame_.parseFailures;
            continue;
        }

        sequence = header->seqNumber();
        timestamp = header->timestamp();
        if (firstValidPacket) {
            currentFrame_.firstSequence = sequence;
            currentFrame_.rtpTimestamp = timestamp;
            firstValidPacket = false;
        } else {
            expectedSequence = static_cast<uint16_t>(currentFrame_.lastSequence + 1);
            if (sequence != expectedSequence) {
                ++currentFrame_.sequenceDiscontinuities;
            }
            if (timestamp != currentFrame_.rtpTimestamp) {
                ++currentFrame_.timestampMismatches;
            }
        }

        currentFrame_.lastSequence = sequence;
        currentFrame_.markerSeen = currentFrame_.markerSeen || header->marker() != 0;
        ++currentFrame_.rtpPackets;
        currentFrame_.rtpBytes += static_cast<uint64_t>(message->size());
        currentFrame_.rtpPayloadBytes +=
            static_cast<uint64_t>(message->size() - headerBytes);
    }
}

/** 返回已提交帧的累计统计快照。 */
void WebRtcRtpFrameDiagnostics::getStats(WebRtcRtpFrameDiagnosticsStats &stats) const
{
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);

    lock.lock();
    stats = stats_;
}

/** 提交当前帧，并为关键帧、大帧或异常帧输出关联日志。 */
void WebRtcRtpFrameDiagnostics::commitCurrentFrameLocked()
{
    bool replaceLargest;
    bool logFrame;

    if (!frameActive_) {
        return;
    }

    ++stats_.frames;
    stats_.generatedPackets += currentFrame_.rtpPackets;
    stats_.generatedBytes += currentFrame_.rtpBytes;
    stats_.generatedPayloadBytes += currentFrame_.rtpPayloadBytes;
    stats_.parseFailures += currentFrame_.parseFailures;
    stats_.sequenceDiscontinuities += currentFrame_.sequenceDiscontinuities;
    stats_.timestampMismatches += currentFrame_.timestampMismatches;
    if (currentFrame_.rtpPackets == 0) {
        ++stats_.framesWithoutRtp;
    }

    stats_.latestFrame = currentFrame_;
    replaceLargest = !stats_.largestFrame.valid ||
                     currentFrame_.rtpPackets > stats_.largestFrame.rtpPackets ||
                     (currentFrame_.rtpPackets == stats_.largestFrame.rtpPackets &&
                      currentFrame_.rtpBytes > stats_.largestFrame.rtpBytes);
    if (replaceLargest) {
        stats_.largestFrame = currentFrame_;
    }

    /* 只记录关键帧、大帧和异常帧，避免逐帧日志反过来干扰实时发送。 */
    logFrame = currentFrame_.keyFrame ||
               currentFrame_.encodedBytes >= WEBRTC_RTP_LARGE_FRAME_LOG_BYTES ||
               currentFrame_.rtpPackets == 0 || currentFrame_.parseFailures > 0 ||
               currentFrame_.sequenceDiscontinuities > 0 ||
               currentFrame_.timestampMismatches > 0 || !currentFrame_.sendAccepted;
    if (logFrame) {
        LOG_INFO("[WEBRTC_RTP_FRAME] session=%s frame_id=%llu pts_us=%llu key=%d "
                 "encoded_bytes=%llu rtp_ts=%u packets=%llu rtp_bytes=%llu "
                 "payload_bytes=%llu seq_first=%u seq_last=%u marker=%d "
                 "seq_discontinuities=%llu timestamp_mismatches=%llu parse_failures=%llu "
                 "send_accepted=%d send_call_us=%llu buffered=%llu",
                 sessionId_.c_str(),
                 static_cast<unsigned long long>(currentFrame_.frameId),
                 static_cast<unsigned long long>(currentFrame_.ptsUs),
                 currentFrame_.keyFrame ? 1 : 0,
                 static_cast<unsigned long long>(currentFrame_.encodedBytes),
                 static_cast<unsigned int>(currentFrame_.rtpTimestamp),
                 static_cast<unsigned long long>(currentFrame_.rtpPackets),
                 static_cast<unsigned long long>(currentFrame_.rtpBytes),
                 static_cast<unsigned long long>(currentFrame_.rtpPayloadBytes),
                 static_cast<unsigned int>(currentFrame_.firstSequence),
                 static_cast<unsigned int>(currentFrame_.lastSequence),
                 currentFrame_.markerSeen ? 1 : 0,
                 static_cast<unsigned long long>(currentFrame_.sequenceDiscontinuities),
                 static_cast<unsigned long long>(currentFrame_.timestampMismatches),
                 static_cast<unsigned long long>(currentFrame_.parseFailures),
                 currentFrame_.sendAccepted ? 1 : 0,
                 static_cast<unsigned long long>(currentFrame_.sendCallUs),
                 static_cast<unsigned long long>(currentFrame_.bufferedAmount));
    }

    currentFrame_ = WebRtcRtpFrameStats();
    frameActive_ = false;
}

} // namespace webrtc
} // namespace rkmedia
