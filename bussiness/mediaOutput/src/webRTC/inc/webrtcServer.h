#ifndef __WEBRTC_SERVER_H__
#define __WEBRTC_SERVER_H__

#include "websocketServer.h"
#include "webrtcSession.h"
#include "webrtcTypes.h"

#include <map>
#include <memory>
#include <mutex>

namespace rkmedia {
namespace webrtc {

/*
 * WebRTC 输出服务。
 * 负责启动 WebSocket 信令服务、创建浏览器会话，并把编码帧广播到所有已就绪的视频 Track。
 */
class WebRtcServer {
public:
    WebRtcServer();
    ~WebRtcServer();

    bool start(const WebRtcServerConfig &config);
    void stop();
    int sendVideoFrame(const WebRtcVideoFrame &frame);
    int sendAudioFrame(const WebRtcAudioFrame &frame);
    int sessionCount() const;

private:
    void handleClient(const std::shared_ptr<communication::WebSocketConnection> &connection);
    void removeSession(int id);

private:
    WebRtcServerConfig config_;
    std::shared_ptr<communication::WebSocketServer> wsServer_;
    std::map<int, std::shared_ptr<WebRtcSession>> sessions_;
    mutable std::mutex mutex_;
    int nextSessionId_;
    bool running_;
};

} // namespace webrtc
} // namespace rkmedia

#endif
