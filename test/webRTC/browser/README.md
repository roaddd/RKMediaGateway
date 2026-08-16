# RKMediaGateway WebRTC 浏览器测试页

这是第一阶段浏览器连通性测试页面，用于验证：

- WebSocket 信令连接。
- 浏览器端 `RTCPeerConnection` 创建。
- 浏览器端 Offer 生成。
- Answer / ICE candidate 处理。
- DataChannel IPC 消息收发。
- 视频、音频 Track 接收。
- 主动发送 RTCP PLI/FIR，并统计关键帧到达和出图耗时。

启动静态页面：

```bash
cd RKMediaGateway/test/webRTC/browser
python3 -m http.server --bind 0.0.0.0 8081
```

浏览器访问：

```text
http://设备IP:8081
```

默认信令地址：

```text
ws://192.168.1.3:8000/browser
```

如果使用 libdatachannel 示例里的按 ID 转发信令服务器，浏览器端 `Remote ID` 填 `server`，设备端 WebSocket 客户端使用 `server` 作为本地 ID。

第一阶段预期结果：

```text
WebSocket=open
PeerConnection=connected
ICE=connected 或 completed
DataChannel=open
IPC 日志里能看到 ping/pong
```

默认不要勾选“接收视频”。默认模式只生成 DataChannel Offer，适合验证第一阶段 IPC 通路。

后续接 H264 Track 时再勾选“接收视频”，浏览器 Offer 才会包含 `m=video` 和 `recvonly`。

设备端测试服务器：

```bash
cd RKMediaGateway
./build/build.sh webrtc_ws_server_test Debug
./build/output/webrtc_ws_server_test 8000 0.0.0.0
```

浏览器页面中的 WebSocket 地址填写：

```text
ws://设备IP:8000/browser
```

当前设备端测试服务器直接处理浏览器发来的 `offer` 和 `candidate`，不需要额外的 Python/Node 信令转发服务器。

H264 单文件视频测试：

```bash
./build/output/webrtc_ws_server_test 8000 0.0.0.0 /path/to/test_720p_30fps_baseline.h264 30
```

H264 文件需要是 Annex-B 裸流格式，也就是 ffmpeg `-f h264` 生成的 `00 00 00 01` 起始码格式。
启动后在浏览器页面勾选“接收视频”，再点击开始连接。浏览器 Offer 会携带 `m=video`，设备端会把单个 H264 文件解析成帧，Answer 添加 H264 sendonly Track，并通过 WebRTC RTP 通道循环发送给浏览器。

## PLI/FIR 关键帧恢复测试

视频正常播放且“请求状态”显示“就绪”后，点击“发送 PLI/FIR”。页面通过接收端 `RTCRtpScriptTransformer.sendKeyFrameRequest()` 请求设备输出关键帧，并显示：

- `关键帧到达`：从点击按钮到浏览器收到新编码关键帧的时间。
- `画面恢复`：从点击按钮到该关键帧被 `<video>` 实际呈现的时间。

浏览器允许根据当前解码状态决定不发送请求，因此 Promise 成功不等于设备一定收到 RTCP。最终需要同时执行设备端 `getWebRTC`，确认 `video pli rx` 和目标 session 的 `pli_rx` 增加。设备端 `PliHandler` 会将 PLI 和 FIR 统一交给关键帧请求逻辑。

该接口需要较新的浏览器。如果页面显示“浏览器不支持”，请升级浏览器；页面必须通过 HTTP/HTTPS 服务打开，不能直接双击 HTML 文件，因为测试依赖独立 Worker。
