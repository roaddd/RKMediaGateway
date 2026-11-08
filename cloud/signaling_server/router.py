"""云端信令路由：只维护连接身份、浏览会话及消息去向，不处理网络收发。"""

from __future__ import annotations

import hmac
import secrets
from dataclasses import dataclass, field
from typing import Any

from .protocol import (
    MAX_CANDIDATE_LENGTH,
    MAX_SDP_LENGTH,
    ProtocolError,
    VERSION,
    error_message,
    parse_message,
    required_streams,
    required_string,
)


@dataclass
class Action:
    """路由结果：向 peer 发送 message，或以 close_code 关闭该连接。"""

    peer: object  # server.py 持有的连接对象；路由器只用它作为不透明标识。
    message: dict[str, Any] | None = None  # 待发送的协议消息；无消息时为 None。
    close_code: int | None = None  # WebSocket 关闭码；与 message 同时存在时先发消息再关闭。


@dataclass
class Peer:
    """一条 WebSocket 的身份及关联会话；设备连接可承载多个浏览会话。"""

    role: str  # 由连接路径确定，取值为 device 或 browser。
    authenticated: bool = False  # register/auth 成功后才允许处理会话消息。
    device_id: str = ""  # 仅设备连接使用，且必须与凭据绑定。
    streams: frozenset[str] = frozenset()  # 设备登记时声明的可观看 stream_id。
    allowed_devices: frozenset[str] = frozenset()  # 浏览器凭据允许访问的 device_id。
    sessions: set[str] = field(default_factory=set)  # 此连接参与的全部 session_id。


@dataclass
class Session:
    """一个浏览器到设备的路由；v1 只允许一轮 Offer/Answer。"""

    session_id: str  # 云端随机生成，不复用，不能用 RTP SSRC 替代。
    browser: object  # 此会话唯一的浏览器连接。
    device: object  # 此会话对应的设备长连接，可与其他会话共享。
    device_id: str  # 设备稳定标识。
    stream_id: str  # 设备内的媒体输出标识，如 main/sub。
    state: str = "opened"  # opened -> offered -> answered；关闭时删除会话。


class SignalingRouter:
    """校验身份与会话归属，返回发送动作；不依赖具体 WebSocket 库。"""

    def __init__(
        self,
        device_tokens: dict[str, str],
        browser_tokens: dict[str, frozenset[str]],
        *,
        max_sessions_per_device: int = 32,
        max_sessions_per_browser: int = 4,
    ) -> None:
        # 凭据由配置模块加载；设备凭据与浏览器访问范围分别保存。
        self._device_tokens = device_tokens
        self._browser_tokens = browser_tokens
        self._max_sessions_per_device = max_sessions_per_device
        self._max_sessions_per_browser = max_sessions_per_browser
        # 三张索引分别回答“连接是谁”“设备在哪条连接上”“会话对应哪两端”。
        # 只在同一个 asyncio 事件循环内同步操作；这里不跨 await，因此无需另加锁。
        self._peers: dict[object, Peer] = {}
        self._devices: dict[str, object] = {}
        self._sessions: dict[str, Session] = {}

    def connect(self, peer: object, role: str) -> None:
        """记录一条尚未认证的连接；role 只能由服务端根据路径指定。"""
        if role not in ("device", "browser") or peer in self._peers:
            raise ValueError("invalid connection role or duplicate connection")
        self._peers[peer] = Peer(role)

    def process(self, peer: object, raw: str) -> list[Action]:
        """解析一条客户端消息，按身份和协议状态返回有序的发送/关闭动作。"""
        state = self._peers.get(peer)
        if state is None:
            return []
        message: dict[str, Any] = {}
        try:
            message = parse_message(raw)
            kind = message["type"]
            # 未认证连接仅允许各自的首条 register/auth，不能提前创建会话。
            if not state.authenticated:
                if state.role == "device" and kind == "register":
                    return self._register(peer, state, message)
                if state.role == "browser" and kind == "auth":
                    return self._authenticate(peer, state, message)
                raise ProtocolError("unauthorized", "authentication required")
            if kind == "open" and state.role == "browser":
                return self._open(peer, state, message)
            if kind in ("offer", "answer", "candidate", "close"):
                return self._session_message(peer, state, kind, message)
            raise ProtocolError("invalid_message", "message type is not allowed in this state")
        except ProtocolError as error:
            # 只回显受限长度的关联 ID；凭据和原始消息绝不进入错误响应。
            request_id = message.get("request_id", "")
            session_id = message.get("session_id", "")
            if not isinstance(request_id, str) or len(request_id) > 128:
                request_id = ""
            if not isinstance(session_id, str) or len(session_id) > 128:
                session_id = ""
            actions = [Action(peer, error_message(error.code, error.message,
                                                  request_id=request_id, session_id=session_id))]
            # 首条消息不合法时先回错误，再关闭未认证连接，避免持续占用资源。
            if not state.authenticated:
                actions.append(Action(peer, close_code=1008))
            return actions

    def disconnect(self, peer: object, *, reason: str = "client_closed") -> list[Action]:
        """移除连接及其会话，并通知每个会话的另一端；重复调用无副作用。"""
        state = self._peers.pop(peer, None)
        if state is None:
            return []
        if state.role == "device" and self._devices.get(state.device_id) is peer:
            del self._devices[state.device_id]
        actions: list[Action] = []
        # 先从总表移除，再从对端反向索引移除；旧 session_id 从此不可路由。
        for session_id in tuple(state.sessions):
            session = self._sessions.pop(session_id, None)
            if session is None:
                continue
            other = session.browser if session.device is peer else session.device
            other_state = self._peers.get(other)
            if other_state is not None:
                other_state.sessions.discard(session_id)
                actions.append(Action(other, self._closed(session_id, reason)))
        return actions

    def _register(self, peer: object, state: Peer, message: dict[str, Any]) -> list[Action]:
        """验证设备凭据和码流列表；同 ID 重连时让新连接接管。"""
        device_id = required_string(message, "device_id", 128)
        streams = required_streams(message)
        token = required_string(message, "token", 4096)
        expected = self._device_tokens.get(device_id)
        if expected is None or not hmac.compare_digest(token, expected):
            raise ProtocolError("unauthorized", "invalid device credentials")

        actions: list[Action] = []
        old_peer = self._devices.get(device_id)
        if old_peer is not None and old_peer is not peer:
            # 旧 TCP/WebSocket 可能尚未检测到断线；先撤销旧会话并通知浏览器，
            # 再关闭旧连接，防止两个连接同时代表同一设备。
            actions.extend(self.disconnect(old_peer, reason="device_offline"))
            actions.append(Action(old_peer, close_code=1008))
        state.authenticated = True
        state.device_id = device_id
        state.streams = streams
        self._devices[device_id] = peer
        actions.append(Action(peer, {"version": VERSION, "type": "registered", "device_id": device_id}))
        return actions

    def _authenticate(self, peer: object, state: Peer, message: dict[str, Any]) -> list[Action]:
        """验证浏览器凭据，并保存其被授权访问的设备集合。"""
        token = required_string(message, "token", 4096)
        allowed: frozenset[str] | None = None
        # 比较凭据时不拼接日志，也不把 token 复制到协议错误消息中。
        for known_token, devices in self._browser_tokens.items():
            if hmac.compare_digest(token, known_token):
                allowed = devices
        if allowed is None:
            raise ProtocolError("unauthorized", "invalid browser credentials")
        state.authenticated = True
        state.allowed_devices = allowed
        return [Action(peer, {"version": VERSION, "type": "authenticated"})]

    def _open(self, peer: object, state: Peer, message: dict[str, Any]) -> list[Action]:
        """按 ACL、在线设备和码流能力建立浏览会话，返回两端 opened。"""
        request_id = required_string(message, "request_id", 128)
        device_id = required_string(message, "device_id", 128)
        stream_id = required_string(message, "stream_id", 64)
        if device_id not in state.allowed_devices:
            raise ProtocolError("unauthorized", "device access denied")
        device = self._devices.get(device_id)
        if device is None:
            raise ProtocolError("device_offline", "device is offline")
        device_state = self._peers[device]
        if stream_id not in device_state.streams:
            raise ProtocolError("stream_not_found", "stream is not available")
        if (len(state.sessions) >= self._max_sessions_per_browser or
                len(device_state.sessions) >= self._max_sessions_per_device):
            raise ProtocolError("resource_limit", "session limit reached")

        # session_id 与连接、request_id 和 SSRC 都不同；随机且不可复用。
        session_id = secrets.token_urlsafe(18)
        while session_id in self._sessions:
            session_id = secrets.token_urlsafe(18)
        session = Session(session_id, peer, device, device_id, stream_id)
        self._sessions[session_id] = session
        state.sessions.add(session_id)
        device_state.sessions.add(session_id)
        opened = {"version": VERSION, "type": "opened", "session_id": session_id,
                  "device_id": device_id, "stream_id": stream_id}
        # 先通知设备创建会话，再通知浏览器；浏览器收到 opened 后可能立即发送 Offer。
        return [Action(device, opened), Action(peer, {**opened, "request_id": request_id})]

    def _session_message(
        self, peer: object, state: Peer, kind: str, message: dict[str, Any]
    ) -> list[Action]:
        """验证会话所有权和单轮协商状态，只向该会话的对端转发。"""
        session_id = required_string(message, "session_id", 128)
        session = self._sessions.get(session_id)
        if session is None:
            # 重复 close 幂等；迟到的 SDP/ICE 必须拒绝，不能误投递给新会话。
            if kind == "close":
                return []
            raise ProtocolError("session_not_found", "session does not exist")
        if peer not in (session.browser, session.device) or session_id not in state.sessions:
            raise ProtocolError("unauthorized", "session access denied")

        other = session.device if peer is session.browser else session.browser
        if kind == "close":
            reason = message.get("reason", "client_closed")
            if not isinstance(reason, str) or not reason or len(reason) > 64:
                raise ProtocolError("invalid_message", "invalid reason")
            del self._sessions[session_id]
            # 只释放本浏览会话；共享的设备长连接与其他浏览会话保持不变。
            self._peers[session.browser].sessions.discard(session_id)
            self._peers[session.device].sessions.discard(session_id)
            return [Action(other, self._closed(session_id, reason))]

        if kind == "offer":
            # v1 规定浏览器发起协商，且一条会话只能提交一次 Offer。
            if peer is not session.browser or session.state != "opened":
                raise ProtocolError("invalid_message", "offer is not expected")
            sdp = required_string(message, "sdp", MAX_SDP_LENGTH)
            session.state = "offered"
            payload = {"version": VERSION, "type": kind, "session_id": session_id, "sdp": sdp}
        elif kind == "answer":
            # Answer 必须由对应设备对已经转发的 Offer 做出。
            if peer is not session.device or session.state != "offered":
                raise ProtocolError("invalid_message", "answer is not expected")
            sdp = required_string(message, "sdp", MAX_SDP_LENGTH)
            session.state = "answered"
            payload = {"version": VERSION, "type": kind, "session_id": session_id, "sdp": sdp}
        else:
            # candidate 可在 Offer 后、Answer 前到达；端点负责在远端 SDP
            # 尚未设置时暂存。云端只验证格式并原样保留 candidate/mid 内容。
            if session.state not in ("offered", "answered"):
                raise ProtocolError("invalid_message", "candidate before offer")
            candidate = required_string(message, "candidate", MAX_CANDIDATE_LENGTH)
            mid = required_string(message, "mid", 64)
            payload = {"version": VERSION, "type": kind, "session_id": session_id,
                       "candidate": candidate, "mid": mid}
        return [Action(other, payload)]

    @staticmethod
    def _closed(session_id: str, reason: str) -> dict[str, Any]:
        """构造发给对端的会话关闭通知。"""
        return {"version": VERSION, "type": "close", "session_id": session_id, "reason": reason}
