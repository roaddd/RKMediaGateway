/**
 * @file webrtcRtpFrameDiagnostics.h
 * @brief 定义 H.264 编码帧与 packetizer 生成 RTP 包之间的关联诊断器。
 *
 * 本模块位于 H264RtpPacketizer 之后、RTCP/NACK 处理器之前。它只读取 RTP
 * 元数据并原样放行消息，不改变 packetizer、发送节奏或网络传输行为。
 */
#ifndef __WEBRTC_RTP_FRAME_DIAGNOSTICS_H__
#define __WEBRTC_RTP_FRAME_DIAGNOSTICS_H__

#include "webrtcStats.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <rtc/mediahandler.hpp>
#include <string>

namespace rkmedia {
namespace webrtc {

/**
 * @brief 观察 H.264 packetizer 输出并生成帧级 RTP 关联统计。
 *
 * beginFrame()、packetizer 触发的 outgoing() 和 finishFrame() 共同构成一次
 * 帧级事务。统计中的“生成”只表示 RTP 包已经由 packetizer 产出，不表示包已
 * 经 ICE/DTLS/SRTP 成功发送或被浏览器接收。
 */
class WebRtcRtpFrameDiagnostics final : public rtc::MediaHandler {
public:
    /**
     * @brief 创建一个会话级 RTP 帧诊断器。
     * @param[in] sessionId 云端会话 ID，用于诊断日志定位。
     * @param[in] payloadType 当前 H.264 Track 协商得到的 RTP PT。
     */
    WebRtcRtpFrameDiagnostics(const std::string &sessionId, uint8_t payloadType);

    /**
     * @brief 开始记录一帧即将进入 packetizer 的 H.264 数据。
     * @param[in] frameId gateway 视频帧序号。
     * @param[in] ptsUs 编码帧 PTS，单位微秒。
     * @param[in] encodedBytes 编码帧字节数。
     * @param[in] keyFrame 是否为关键帧。
     * @note 必须在 Track::sendFrame() 之前调用。
     */
    void beginFrame(uint64_t frameId,
                    uint64_t ptsUs,
                    size_t encodedBytes,
                    bool keyFrame);

    /**
     * @brief 结束当前帧事务并提交累计统计。
     * @param[in] sendAccepted Track::sendFrame() 是否正常返回。
     * @param[in] sendCallUs Track::sendFrame() 调用耗时，单位微秒。
     * @param[in] bufferedAmount sendFrame 返回后的 Track 待发送字节数。
     * @note sendAccepted 只代表 Track 接受调用，不代表 RTP 已成功发送到网络。
     */
    void finishFrame(bool sendAccepted, uint64_t sendCallUs, uint64_t bufferedAmount);

    /**
     * @brief 观察 packetizer 输出的一批 RTP 消息。
     * @param[in,out] messages 待传给后续 RTCP/NACK/传输处理器的消息集合。
     * @param[in] send libdatachannel 的反向发送回调，本处理器不调用该回调。
     * @details 只解析 RTP 固定头、CSRC 和扩展头长度，绝不修改或删除消息。
     */
    void outgoing(rtc::message_vector &messages, const rtc::message_callback &send) override;

    /**
     * @brief 获取当前累计统计的线程安全快照。
     * @param[out] stats 接收累计计数、最近帧和最大 RTP 分片帧。
     */
    void getStats(WebRtcRtpFrameDiagnosticsStats &stats) const;

private:
    /**
     * @brief 提交当前活动帧，并按需输出一条帧级关联日志。
     * @pre 调用者已经持有 mutex_。
     */
    void commitCurrentFrameLocked();

private:
    /* 不可变会话信息：构造完成后只读。 */
    std::string sessionId_; /* 日志中使用的云端会话 ID。 */
    uint8_t payloadType_; /* 当前 H.264 Track 协商得到的 RTP PT。 */

    /* 并发保护：覆盖 sendFrame 调用线程与调试快照读取线程。 */
    mutable std::mutex mutex_;

    /* 当前帧事务：从 beginFrame 持续到 finishFrame。 */
    bool frameActive_; /* 当前是否存在等待完成的帧事务。 */
    WebRtcRtpFrameStats currentFrame_; /* 当前帧的输入元数据和 RTP 汇总。 */

    /* 已提交统计：仅通过 getStats 对外提供只读快照。 */
    WebRtcRtpFrameDiagnosticsStats stats_; /* 会话级累计诊断统计。 */
};

} // namespace webrtc
} // namespace rkmedia

#endif
