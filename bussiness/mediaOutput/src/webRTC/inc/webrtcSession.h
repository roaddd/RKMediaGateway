#ifndef __WEBRTC_SESSION_H__
#define __WEBRTC_SESSION_H__

#include "websocketServer.h"
#include "webrtcTypes.h"

#include <memory>
#include <mutex>
#include <functional>
#include <rtc/datachannel.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/track.hpp>
#include <string>
#include <vector>

namespace rkmedia {
namespace webrtc {

typedef std::function<void(int)> WebRtcSessionClosedCallback;

/*
 * 单个浏览器 WebRTC 会话。
 * 一个 WebSocket 连接对应一个 WebRtcSession，内部管理 PeerConnection、DataChannel 和视频 Track。
 */
class WebRtcSession : public std::enable_shared_from_this<WebRtcSession> {
public:
    WebRtcSession(int id,
                  const WebRtcServerConfig &config,
                  const std::shared_ptr<communication::WebSocketConnection> &connection,
                  const WebRtcSessionClosedCallback &closedCallback);
    ~WebRtcSession();

    void start();
    void close();
    int id() const;
    bool isVideoReady() const;
    bool sendVideoFrame(const WebRtcVideoFrame &frame);
    bool isAudioReady() const;
    bool sendAudioFrame(const WebRtcAudioFrame &frame);

private:
    void handleWsText(const std::string &message);
    void handleOffer(const std::string &message);
    void handleCandidate(const std::string &message);
    void createPeerConnection();
    void bindDataChannel(const std::shared_ptr<rtc::DataChannel> &dc);
    void addH264VideoTrack(const std::string &mid, uint8_t payloadType);
    void addG711AudioTrack(const std::string &mid, uint8_t payloadType, WebRtcAudioCodec codec);
    void sendDescription(const rtc::Description &description);
    void sendCandidate(const rtc::Candidate &candidate);
    void handleIpcMessage(const std::string &message);
    bool convertAnnexBToLengthPrefixed(const uint8_t *data, size_t size, rtc::binary &frame);
    bool copyLengthPrefixedFrame(const uint8_t *data, size_t size, rtc::binary &frame);

private:
    int id_;
    WebRtcServerConfig config_;
    std::shared_ptr<communication::WebSocketConnection> connection_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::DataChannel> dc_;
    std::shared_ptr<rtc::Track> videoTrack_;
    std::shared_ptr<rtc::Track> audioTrack_;
    mutable std::mutex mutex_;
    WebRtcSessionClosedCallback closedCallback_;
    bool closed_;
};

} // namespace webrtc
} // namespace rkmedia

#endif
