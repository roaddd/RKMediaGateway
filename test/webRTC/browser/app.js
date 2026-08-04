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
    };

    let ws = null;
    let pc = null;
    let dc = null;
    let pendingLocalCandidates = [];

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

        els.wsConnectBtn.disabled = wsOpen;
        els.wsCloseBtn.disabled = !ws;
        els.startBtn.disabled = !wsOpen;
        els.pingBtn.disabled = !dcOpen;
        els.statusBtn.disabled = !dcOpen;
        els.ipcInput.disabled = !dcOpen;
        els.sendIpcBtn.disabled = !dcOpen;
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

        bindDataChannel(peer.createDataChannel("ipc"));
        log("创建本地 DataChannel：ipc");

        if (receiveVideo) {
            peer.addTransceiver("video", { direction: "recvonly" });
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
    updateButtons();
})();
