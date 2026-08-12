(function () {
    "use strict";

    const els = {
        wsUrl: document.getElementById("wsUrl"),
        remoteId: document.getElementById("remoteId"),
        wsConnectBtn: document.getElementById("wsConnectBtn"),
        wsCloseBtn: document.getElementById("wsCloseBtn"),
        receiveVideo: document.getElementById("receiveVideo"),
        startBtn: document.getElementById("startBtn"),
        pingBtn: document.getElementById("pingBtn"),
        statusBtn: document.getElementById("statusBtn"),
        resetBtn: document.getElementById("resetBtn"),
        ipcInput: document.getElementById("ipcInput"),
        sendIpcBtn: document.getElementById("sendIpcBtn"),
        clearIpcBtn: document.getElementById("clearIpcBtn"),
        clearLogBtn: document.getElementById("clearLogBtn"),
        wsState: document.getElementById("wsState"),
        pcState: document.getElementById("pcState"),
        iceState: document.getElementById("iceState"),
        gatheringState: document.getElementById("gatheringState"),
        dcState: document.getElementById("dcState"),
        localSdpType: document.getElementById("localSdpType"),
        remoteSdpType: document.getElementById("remoteSdpType"),
        localSdp: document.getElementById("localSdp"),
        remoteSdp: document.getElementById("remoteSdp"),
        eventLog: document.getElementById("eventLog"),
        ipcLog: document.getElementById("ipcLog"),
        remoteVideo: document.getElementById("remoteVideo"),
        videoHint: document.getElementById("videoHint"),
        requestKeyFrameBtn: document.getElementById("requestKeyFrameBtn"),
        keyFrameRequestState: document.getElementById("keyFrameRequestState"),
        keyFrameArrivalLatency: document.getElementById("keyFrameArrivalLatency"),
        keyFrameRenderLatency: document.getElementById("keyFrameRenderLatency"),
    };

    let ws = null;
    let pc = null;
    let dc = null;
    let pendingLocalCandidates = [];
    let keyFrameWorker = null;
    let keyFramePort = null;
    let videoReceiver = null;
    let renderedFrameCallbackId = 0;
    let keyFrameRequestSequence = 0;
    let pendingKeyFrameRequest = null;
    let keyFrameRequestTimeoutId = 0;
    const renderedVideoFrames = new Map();

    function now() {
        return new Date().toLocaleTimeString("zh-CN", { hour12: false });
    }

    function log(message) {
        els.eventLog.textContent += `[${now()}] ${message}\n`;
        els.eventLog.scrollTop = els.eventLog.scrollHeight;
    }

    function logIpc(direction, message) {
        els.ipcLog.textContent += `[${now()}] ${direction} ${message}\n`;
        els.ipcLog.scrollTop = els.ipcLog.scrollHeight;
    }

    function setText(el, text, stateClass) {
        el.textContent = text;
        el.classList.remove("state-ok", "state-bad");
        if (stateClass) {
            el.classList.add(stateClass);
        }
    }

    function updateButtons() {
        const wsOpen = ws && ws.readyState === WebSocket.OPEN;
        const dcOpen = dc && dc.readyState === "open";
        const keyFrameReady = keyFramePort && videoReceiver && videoReceiver.track.readyState === "live";

        els.wsConnectBtn.disabled = wsOpen;
        els.wsCloseBtn.disabled = !ws;
        els.startBtn.disabled = !wsOpen;
        els.pingBtn.disabled = !dcOpen;
        els.statusBtn.disabled = !dcOpen;
        els.ipcInput.disabled = !dcOpen;
        els.sendIpcBtn.disabled = !dcOpen;
        els.requestKeyFrameBtn.disabled = !keyFrameReady || !!pendingKeyFrameRequest;
    }

    /* 判断当前浏览器是否支持接收端 WebRTC Encoded Transform。 */
    function supportsKeyFrameRequest() {
        return typeof Worker === "function" &&
            typeof MessageChannel === "function" &&
            typeof RTCRtpScriptTransform === "function";
    }

    /* 停止关键帧测试相关资源，并恢复页面初始状态。 */
    function closeKeyFrameTest() {
        if (keyFrameRequestTimeoutId) {
            window.clearTimeout(keyFrameRequestTimeoutId);
            keyFrameRequestTimeoutId = 0;
        }
        if (renderedFrameCallbackId && typeof els.remoteVideo.cancelVideoFrameCallback === "function") {
            els.remoteVideo.cancelVideoFrameCallback(renderedFrameCallbackId);
            renderedFrameCallbackId = 0;
        }
        if (keyFramePort) {
            keyFramePort.close();
            keyFramePort = null;
        }
        if (keyFrameWorker) {
            keyFrameWorker.terminate();
            keyFrameWorker = null;
        }

        videoReceiver = null;
        pendingKeyFrameRequest = null;
        renderedVideoFrames.clear();
        els.keyFrameArrivalLatency.textContent = "-";
        els.keyFrameRenderLatency.textContent = "-";
        setText(els.keyFrameRequestState,
            supportsKeyFrameRequest() ? "等待视频" : "浏览器不支持",
            supportsKeyFrameRequest() ? "" : "state-bad");
        updateButtons();
    }

    /* 使用关键帧 RTP 时间戳完成出图统计，确保统计对象确实是请求返回的关键帧。 */
    function completeKeyFrameRender(request, renderedAt) {
        let renderLatency;

        if (!pendingKeyFrameRequest || pendingKeyFrameRequest.id !== request.id) {
            return;
        }

        renderLatency = renderedAt - request.requestedAt;
        els.keyFrameRenderLatency.textContent = `${renderLatency} ms`;
        setText(els.keyFrameRequestState, "画面已恢复", "state-ok");
        log(`关键帧恢复完成：到达=${request.arrivalLatency}ms，出图=${renderLatency}ms`);
        window.clearTimeout(keyFrameRequestTimeoutId);
        keyFrameRequestTimeoutId = 0;
        pendingKeyFrameRequest = null;
        updateButtons();
    }

    /* 持续记录 video 元素真正呈现的 WebRTC RTP 时间戳。 */
    function startRenderedFrameMonitor() {
        let monitor;

        if (typeof els.remoteVideo.requestVideoFrameCallback !== "function") {
            return;
        }

        monitor = () => {
            renderedFrameCallbackId = els.remoteVideo.requestVideoFrameCallback((unusedNow, metadata) => {
                const request = pendingKeyFrameRequest;
                const rtpTimestamp = metadata.rtpTimestamp;
                const renderedAt = Date.now();
                let oldestTimestamp;

                if (rtpTimestamp !== undefined) {
                    renderedVideoFrames.set(rtpTimestamp, renderedAt);
                    if (renderedVideoFrames.size > 30) {
                        oldestTimestamp = renderedVideoFrames.keys().next().value;
                        renderedVideoFrames.delete(oldestTimestamp);
                    }
                }
                if (request && request.keyFrameReceived &&
                    (request.rtpTimestamp === undefined || request.rtpTimestamp === rtpTimestamp)) {
                    completeKeyFrameRender(request, renderedAt);
                }
                monitor();
            });
        };
        monitor();
    }

    /* 处理 Worker 返回的请求状态和编码关键帧事件。 */
    function handleKeyFrameWorkerMessage(event) {
        const message = event.data || {};
        const request = pendingKeyFrameRequest;
        let arrivalLatency;
        let renderedAt;

        if (message.type === "request-error" || message.type === "transform-error") {
            setText(els.keyFrameRequestState, "请求失败", "state-bad");
            log(`关键帧请求失败：${message.error || "unknown error"}`);
            if (keyFrameRequestTimeoutId) {
                window.clearTimeout(keyFrameRequestTimeoutId);
                keyFrameRequestTimeoutId = 0;
            }
            pendingKeyFrameRequest = null;
            updateButtons();
            return;
        }
        if (message.type === "request-complete" && request && message.requestId === request.id) {
            setText(els.keyFrameRequestState, "等待关键帧");
            log("浏览器已处理关键帧请求，等待设备返回关键帧");
            return;
        }
        if (message.type !== "keyframe" || !request || message.requestId !== request.id) {
            return;
        }

        arrivalLatency = Date.now() - request.requestedAt;
        request.keyFrameReceived = true;
        request.arrivalLatency = arrivalLatency;
        request.rtpTimestamp = message.rtpTimestamp;
        els.keyFrameArrivalLatency.textContent = `${arrivalLatency} ms`;
        setText(els.keyFrameRequestState, "关键帧已到达", "state-ok");
        log(`收到请求后的首个编码关键帧：${arrivalLatency}ms`);

        renderedAt = renderedVideoFrames.get(message.rtpTimestamp);
        if (renderedAt !== undefined) {
            completeKeyFrameRender(request, renderedAt);
            return;
        }

        if (typeof els.remoteVideo.requestVideoFrameCallback !== "function") {
            els.keyFrameRenderLatency.textContent = "浏览器不支持统计";
            window.clearTimeout(keyFrameRequestTimeoutId);
            keyFrameRequestTimeoutId = 0;
            pendingKeyFrameRequest = null;
            updateButtons();
        }
    }

    /* 为远端视频 Receiver 安装透传转换器，以便发送请求并观察关键帧。 */
    function attachKeyFrameTest(receiver) {
        let channel;

        closeKeyFrameTest();
        if (!supportsKeyFrameRequest()) {
            log("当前浏览器不支持 RTCRtpScriptTransform，无法主动请求关键帧");
            return;
        }

        try {
            channel = new MessageChannel();
            keyFrameWorker = new Worker("keyframe-worker.js");
            keyFramePort = channel.port1;
            keyFramePort.onmessage = handleKeyFrameWorkerMessage;
            keyFramePort.start();
            receiver.transform = new RTCRtpScriptTransform(
                keyFrameWorker,
                { name: "keyframe-test", port: channel.port2 },
                [channel.port2]);
            videoReceiver = receiver;
            setText(els.keyFrameRequestState, "就绪", "state-ok");
            startRenderedFrameMonitor();
            log("关键帧请求测试接口已就绪");
        } catch (error) {
            log(`初始化关键帧请求测试失败：${error.name || "Error"}: ${error.message}`);
            closeKeyFrameTest();
            setText(els.keyFrameRequestState, "初始化失败", "state-bad");
        }
        updateButtons();
    }

    /* 请求浏览器向设备发送 RTCP 关键帧反馈，并启动到达、出图计时。 */
    function requestRemoteKeyFrame() {
        let requestId;

        if (!keyFramePort || !videoReceiver || pendingKeyFrameRequest) {
            log("关键帧请求失败：视频 Receiver 尚未就绪或已有请求正在进行");
            return;
        }

        requestId = ++keyFrameRequestSequence;
        pendingKeyFrameRequest = {
            id: requestId,
            requestedAt: Date.now(),
            keyFrameReceived: false,
            arrivalLatency: 0,
        };
        els.keyFrameArrivalLatency.textContent = "-";
        els.keyFrameRenderLatency.textContent = "-";
        setText(els.keyFrameRequestState, "正在请求");
        keyFramePort.postMessage({ type: "request-keyframe", requestId });
        log(`发送关键帧请求：request_id=${requestId}`);

        keyFrameRequestTimeoutId = window.setTimeout(() => {
            setText(els.keyFrameRequestState, "5 秒未恢复", "state-bad");
            log("关键帧请求超时：5 秒内未观察到新的编码关键帧和恢复画面");
            pendingKeyFrameRequest = null;
            keyFrameRequestTimeoutId = 0;
            updateButtons();
        }, 5000);
        updateButtons();
    }

    function signalingMessage(type, payload) {
        const message = Object.assign({ type }, payload || {});
        const remoteId = els.remoteId.value.trim();
        if (remoteId) {
            message.id = remoteId;
        }
        return message;
    }

    function sendSignaling(type, payload) {
        if (!ws || ws.readyState !== WebSocket.OPEN) {
            log(`信令发送失败：WebSocket 未连接，type=${type}`);
            return;
        }

        const message = signalingMessage(type, payload);
        ws.send(JSON.stringify(message));
        log(`信令发送：${JSON.stringify(message)}`);
    }

    function normalizeDescription(message) {
        return {
            type: message.type,
            sdp: message.sdp || message.description || "",
        };
    }

    function normalizeCandidate(message) {
        return {
            candidate: message.candidate,
            sdpMid: message.mid || message.sdpMid || "0",
            sdpMLineIndex: message.sdpMLineIndex,
        };
    }

    function createPeerConnection() {
        closePeerConnection();

        pc = new RTCPeerConnection({
            bundlePolicy: "max-bundle",
        });

        pendingLocalCandidates = [];
        setText(els.pcState, pc.connectionState || "new");
        setText(els.iceState, pc.iceConnectionState || "new");
        setText(els.gatheringState, pc.iceGatheringState || "new");

        pc.onconnectionstatechange = () => {
            const state = pc.connectionState || "unknown";
            setText(els.pcState, state, state === "connected" ? "state-ok" : "");
            log(`PeerConnection 状态：${state}`);
        };

        pc.oniceconnectionstatechange = () => {
            const state = pc.iceConnectionState || "unknown";
            const stateClass = state === "connected" || state === "completed" ? "state-ok" :
                state === "failed" || state === "disconnected" ? "state-bad" : "";
            setText(els.iceState, state, stateClass);
            log(`ICE 状态：${state}`);
        };

        pc.onicegatheringstatechange = () => {
            const state = pc.iceGatheringState || "unknown";
            setText(els.gatheringState, state, state === "complete" ? "state-ok" : "");
            log(`ICE gathering 状态：${state}`);
        };

        pc.onicecandidate = (event) => {
            if (!event.candidate) {
                log("本地 ICE candidate 收集结束");
                return;
            }

            const payload = {
                candidate: event.candidate.candidate,
                mid: event.candidate.sdpMid,
                sdpMLineIndex: event.candidate.sdpMLineIndex,
            };

            pendingLocalCandidates.push(payload);
            sendSignaling("candidate", payload);
        };

        pc.ontrack = (event) => {
            if (event.streams && event.streams[0]) {
                els.remoteVideo.srcObject = event.streams[0];
                els.videoHint.textContent = "已收到远端 Track";
                log("收到远端媒体 Track");
            }
            /*
             * 主动创建 Offer 时已经在 addTransceiver() 后安装 Transform。
             * 这里只为收到远端 Offer 等被动协商场景补装，避免同一 Receiver 重复赋值。
             */
            if (event.track.kind === "video" && videoReceiver !== event.receiver) {
                attachKeyFrameTest(event.receiver);
            }
        };

        pc.ondatachannel = (event) => {
            log(`收到远端 DataChannel：${event.channel.label}`);
            bindDataChannel(event.channel);
        };

        return pc;
    }

    function bindDataChannel(channel) {
        dc = channel;
        setText(els.dcState, dc.readyState);
        updateButtons();

        dc.onopen = () => {
            setText(els.dcState, "open", "state-ok");
            log(`DataChannel 已打开：${dc.label}`);
            updateButtons();
        };

        dc.onclose = () => {
            setText(els.dcState, "closed", "state-bad");
            log("DataChannel 已关闭");
            updateButtons();
        };

        dc.onerror = () => {
            setText(els.dcState, "error", "state-bad");
            log("DataChannel 错误");
            updateButtons();
        };

        dc.onmessage = (event) => {
            if (typeof event.data === "string") {
                logIpc("<", event.data);
            } else {
                logIpc("<", `[binary ${event.data.byteLength || 0} bytes]`);
            }
        };
    }

    function closePeerConnection() {
        closeKeyFrameTest();
        if (dc) {
            dc.close();
            dc = null;
        }
        if (pc) {
            pc.getSenders().forEach((sender) => {
                if (sender.track) {
                    sender.track.stop();
                }
            });
            pc.close();
            pc = null;
        }

        pendingLocalCandidates = [];
        els.remoteVideo.srcObject = null;
        els.videoHint.textContent = "等待远端 Track";
        els.localSdp.textContent = "";
        els.remoteSdp.textContent = "";
        els.localSdpType.textContent = "-";
        els.remoteSdpType.textContent = "-";
        setText(els.pcState, "closed");
        setText(els.iceState, "closed");
        setText(els.gatheringState, "new");
        setText(els.dcState, "closed");
        updateButtons();
    }

    async function startOffer() {
        const peer = createPeerConnection();
        const receiveVideo = els.receiveVideo.checked;
        let offer;
        let videoTransceiver;

        bindDataChannel(peer.createDataChannel("ipc"));
        log("创建本地 DataChannel：ipc");

        if (receiveVideo) {
            /*
             * Transform 必须在媒体接收开始前安装。先取得 video Receiver 并挂载 Worker，
             * 再创建 Offer，避免 ontrack 阶段赋值时接收通路已经进入运行状态。
             */
            videoTransceiver = peer.addTransceiver("video", { direction: "recvonly" });
            attachKeyFrameTest(videoTransceiver.receiver);
            peer.addTransceiver("audio", { direction: "recvonly" });
            log("已启用音视频接收，Offer 将包含 video/audio recvonly m-line");
        } else {
            log("当前为纯 DataChannel 模式，Offer 不包含视频 m-line");
        }

        offer = await peer.createOffer();
        await peer.setLocalDescription(offer);

        els.localSdpType.textContent = "offer";
        els.localSdp.textContent = peer.localDescription.sdp;
        sendSignaling("offer", { sdp: peer.localDescription.sdp });
    }

    async function handleOffer(message) {
        const peer = createPeerConnection();
        const offer = normalizeDescription(message);

        await peer.setRemoteDescription(offer);
        els.remoteSdpType.textContent = "offer";
        els.remoteSdp.textContent = offer.sdp;

        const answer = await peer.createAnswer();
        await peer.setLocalDescription(answer);

        els.localSdpType.textContent = "answer";
        els.localSdp.textContent = peer.localDescription.sdp;
        sendSignaling("answer", { sdp: peer.localDescription.sdp });
    }

    async function handleAnswer(message) {
        if (!pc) {
            log("忽略 answer：PeerConnection 不存在");
            return;
        }

        const answer = normalizeDescription(message);
        await pc.setRemoteDescription(answer);
        els.remoteSdpType.textContent = "answer";
        els.remoteSdp.textContent = answer.sdp;
        log("已设置远端 Answer");
    }

    async function handleCandidate(message) {
        if (!pc || !message.candidate) {
            return;
        }

        await pc.addIceCandidate(normalizeCandidate(message));
        log(`已添加远端 ICE candidate：${message.candidate}`);
    }

    function sendIpcObject(obj) {
        sendIpc(JSON.stringify(obj));
    }

    function sendIpc(text) {
        if (!dc || dc.readyState !== "open") {
            logIpc("!", "DataChannel 未打开");
            return;
        }
        dc.send(text);
        logIpc(">", text);
    }

    function connectWebSocket() {
        const url = els.wsUrl.value.trim();
        if (!url) {
            log("WebSocket 地址为空");
            return;
        }

        if (ws) {
            ws.close();
        }

        ws = new WebSocket(url);
        setText(els.wsState, "connecting");
        log(`连接 WebSocket：${url}`);
        updateButtons();

        ws.onopen = () => {
            setText(els.wsState, "open", "state-ok");
            log("WebSocket 已连接");
            updateButtons();
        };

        ws.onclose = () => {
            setText(els.wsState, "closed", "state-bad");
            log("WebSocket 已关闭");
            ws = null;
            updateButtons();
        };

        ws.onerror = () => {
            setText(els.wsState, "error", "state-bad");
            log("WebSocket 错误");
            updateButtons();
        };

        ws.onmessage = async (event) => {
            if (typeof event.data !== "string") {
                log("忽略非文本信令消息");
                return;
            }

            log(`信令接收：${event.data}`);
            const message = JSON.parse(event.data);

            if (message.id && !els.remoteId.value.trim()) {
                els.remoteId.value = message.id;
            }

            if (message.type === "offer") {
                await handleOffer(message);
            } else if (message.type === "answer") {
                await handleAnswer(message);
            } else if (message.type === "candidate") {
                await handleCandidate(message);
            } else if (message.type === "request") {
                sendSignaling("ready", {});
            } else {
                log(`未知信令类型：${message.type}`);
            }
        };
    }

    els.wsConnectBtn.onclick = connectWebSocket;
    els.wsCloseBtn.onclick = () => {
        if (ws) {
            ws.close();
        }
    };
    els.startBtn.onclick = () => {
        startOffer().catch((err) => log(`创建 Offer 失败：${err.message}`));
    };
    els.resetBtn.onclick = closePeerConnection;
    els.pingBtn.onclick = () => sendIpcObject({ cmd: "ping", ts: Date.now() });
    els.statusBtn.onclick = () => sendIpcObject({ cmd: "get_status", ts: Date.now() });
    els.requestKeyFrameBtn.onclick = requestRemoteKeyFrame;
    els.sendIpcBtn.onclick = () => sendIpc(els.ipcInput.value);
    els.clearIpcBtn.onclick = () => {
        els.ipcLog.textContent = "";
    };
    els.clearLogBtn.onclick = () => {
        els.eventLog.textContent = "";
    };

    setText(els.wsState, "closed");
    setText(els.pcState, "closed");
    setText(els.iceState, "closed");
    setText(els.gatheringState, "new");
    setText(els.dcState, "closed");
    setText(els.keyFrameRequestState,
        supportsKeyFrameRequest() ? "等待视频" : "浏览器不支持",
        supportsKeyFrameRequest() ? "" : "state-bad");
    updateButtons();
})();
