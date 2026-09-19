#include "webrtcAudioReceiver.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using rkmedia::webrtc::WEBRTC_AUDIO_CODEC_OPUS;
using rkmedia::webrtc::WebRtcAudioReceiver;
using rkmedia::webrtc::WebRtcAudioReceiverConfig;
using rkmedia::webrtc::WebRtcAudioReceiverStats;
using rkmedia::webrtc::WebRtcIncomingAudioPacket;

static int g_failures = 0;

#define CHECK_TRUE(condition)                                                        \
    do {                                                                             \
        if (!(condition)) {                                                           \
            std::fprintf(stderr, "check failed: %s (%s:%d)\n",                    \
                         #condition, __FILE__, __LINE__);                             \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

/* 构造不带 CSRC、扩展和 padding 的最小 RTP 包，供各测试场景复用。 */
static std::vector<uint8_t> make_rtp_packet(uint8_t payload_type,
                                            uint16_t sequence,
                                            uint32_t timestamp,
                                            uint32_t ssrc,
                                            const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> packet(12 + payload.size(), 0);

    packet[0] = 0x80;
    packet[1] = payload_type;
    packet[2] = static_cast<uint8_t>(sequence >> 8);
    packet[3] = static_cast<uint8_t>(sequence);
    packet[4] = static_cast<uint8_t>(timestamp >> 24);
    packet[5] = static_cast<uint8_t>(timestamp >> 16);
    packet[6] = static_cast<uint8_t>(timestamp >> 8);
    packet[7] = static_cast<uint8_t>(timestamp);
    packet[8] = static_cast<uint8_t>(ssrc >> 24);
    packet[9] = static_cast<uint8_t>(ssrc >> 16);
    packet[10] = static_cast<uint8_t>(ssrc >> 8);
    packet[11] = static_cast<uint8_t>(ssrc);
    std::copy(payload.begin(), payload.end(), packet.begin() + 12);
    return packet;
}

/* 验证 RTP 固定头字段和 Opus payload 能完整映射到类型安全的回调参数。 */
static void test_parse_standard_packet()
{
    WebRtcAudioReceiverConfig config;
    WebRtcIncomingAudioPacket received;
    std::vector<uint8_t> payload = {0xf8, 0xff, 0xfe};
    std::vector<uint8_t> packet;
    int callback_count = 0;
    MediaResult result = MEDIA_ERR;

    config.sessionId = 7;
    config.codec = WEBRTC_AUDIO_CODEC_OPUS;
    config.payloadType = 111;
    WebRtcAudioReceiver receiver(config, [&](const WebRtcIncomingAudioPacket &input) {
        received = input;
        ++callback_count;
    });

    packet = make_rtp_packet(111, 321, 96000, 0x12345678, payload);
    result = receiver.handlePacket(packet.data(), packet.size(), 1234567);

    CHECK_TRUE(result == MEDIA_OK);
    CHECK_TRUE(callback_count == 1);
    CHECK_TRUE(received.sessionId == 7);
    CHECK_TRUE(received.codec == WEBRTC_AUDIO_CODEC_OPUS);
    CHECK_TRUE(received.payloadType == 111);
    CHECK_TRUE(received.sequenceNumber == 321);
    CHECK_TRUE(received.rtpTimestamp == 96000);
    CHECK_TRUE(received.ssrc == 0x12345678);
    CHECK_TRUE(received.arrivalTimeUs == 1234567);
    CHECK_TRUE(received.payload == payload);
}

/* 验证接收器会跳过 RTP 扩展头，并从有效负载末尾剔除 padding。 */
static void test_extension_and_padding()
{
    WebRtcAudioReceiverConfig config;
    WebRtcIncomingAudioPacket received;
    std::vector<uint8_t> packet = {
        0xb0, 111, 0x00, 0x01, 0x00, 0x00, 0x03, 0xc0,
        0x00, 0x00, 0x00, 0x01,
        0xbe, 0xde, 0x00, 0x01, 0x11, 0x22, 0x33, 0x44,
        0xaa, 0xbb, 0x00, 0x02,
    };
    MediaResult result = MEDIA_ERR;

    config.sessionId = 1;
    config.codec = WEBRTC_AUDIO_CODEC_OPUS;
    config.payloadType = 111;
    WebRtcAudioReceiver receiver(config, [&](const WebRtcIncomingAudioPacket &input) {
        received = input;
    });

    result = receiver.handlePacket(packet.data(), packet.size(), 99);
    CHECK_TRUE(result == MEDIA_OK);
    CHECK_TRUE(received.payload == std::vector<uint8_t>({0xaa, 0xbb}));
}

/* 验证截断包、错误 PT 和 RTCP 分别进入对应统计，且不会触发音频回调。 */
static void test_rejects_invalid_packets()
{
    WebRtcAudioReceiverConfig config;
    std::vector<uint8_t> short_packet(8, 0);
    std::vector<uint8_t> wrong_pt;
    std::vector<uint8_t> rtcp = {0x80, 200, 0x00, 0x01, 0, 0, 0, 0};
    int callback_count = 0;
    WebRtcAudioReceiverStats stats;

    config.sessionId = 2;
    config.codec = WEBRTC_AUDIO_CODEC_OPUS;
    config.payloadType = 111;
    WebRtcAudioReceiver receiver(config, [&](const WebRtcIncomingAudioPacket &) {
        ++callback_count;
    });

    wrong_pt = make_rtp_packet(112, 1, 960, 1, {0x01});
    CHECK_TRUE(receiver.handlePacket(short_packet.data(), short_packet.size(), 1) ==
               MEDIA_ERR_INVALID_PARAM);
    CHECK_TRUE(receiver.handlePacket(wrong_pt.data(), wrong_pt.size(), 2) ==
               MEDIA_ERR_UNSUPPORTED);
    CHECK_TRUE(receiver.handlePacket(rtcp.data(), rtcp.size(), 3) == MEDIA_OK);
    receiver.getStats(stats);
    CHECK_TRUE(callback_count == 0);
    CHECK_TRUE(stats.malformedPackets == 1);
    CHECK_TRUE(stats.unexpectedPayloadTypePackets == 1);
    CHECK_TRUE(stats.rtcpPackets == 1);
}

/* 验证序号回绕、缺包、重复、乱序以及 SSRC 切换的统计规则。 */
static void test_sequence_statistics()
{
    WebRtcAudioReceiverConfig config;
    WebRtcAudioReceiverStats stats;
    std::vector<uint8_t> packet;
    WebRtcAudioReceiver receiver(config, nullptr);

    config.sessionId = 3;
    config.codec = WEBRTC_AUDIO_CODEC_OPUS;
    config.payloadType = 111;
    CHECK_TRUE(receiver.reset(config, nullptr) == MEDIA_OK);

    packet = make_rtp_packet(111, 65535, 100, 9, {0x01});
    CHECK_TRUE(receiver.handlePacket(packet.data(), packet.size(), 1) == MEDIA_OK);
    packet = make_rtp_packet(111, 0, 1060, 9, {0x02});
    CHECK_TRUE(receiver.handlePacket(packet.data(), packet.size(), 2) == MEDIA_OK);
    packet = make_rtp_packet(111, 2, 2980, 9, {0x03});
    CHECK_TRUE(receiver.handlePacket(packet.data(), packet.size(), 3) == MEDIA_OK);
    packet = make_rtp_packet(111, 2, 2980, 9, {0x03});
    CHECK_TRUE(receiver.handlePacket(packet.data(), packet.size(), 4) == MEDIA_OK);
    packet = make_rtp_packet(111, 1, 2020, 9, {0x04});
    CHECK_TRUE(receiver.handlePacket(packet.data(), packet.size(), 5) == MEDIA_OK);
    packet = make_rtp_packet(111, 50, 4000, 10, {0x05});
    CHECK_TRUE(receiver.handlePacket(packet.data(), packet.size(), 6) == MEDIA_OK);

    receiver.getStats(stats);
    CHECK_TRUE(stats.rtpPackets == 6);
    CHECK_TRUE(stats.sequenceGapPackets == 1);
    CHECK_TRUE(stats.duplicatePackets == 1);
    CHECK_TRUE(stats.outOfOrderPackets == 1);
    CHECK_TRUE(stats.ssrcChanges == 1);
    CHECK_TRUE(stats.lastSsrc == 10);
    CHECK_TRUE(stats.lastSequenceNumber == 50);
}

/* 顺序执行全部无设备依赖的 RTP 接收行为测试。 */
int main()
{
    test_parse_standard_packet();
    test_extension_and_padding();
    test_rejects_invalid_packets();
    test_sequence_statistics();

    if (g_failures != 0) {
        std::fprintf(stderr, "webrtc audio receiver tests failed: %d\n", g_failures);
        return 1;
    }
    std::printf("webrtc audio receiver tests passed\n");
    return 0;
}
