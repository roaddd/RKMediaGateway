"""云端信令的 WebSocket 适配层；媒体数据不经过此模块。"""

from __future__ import annotations

import asyncio
import json
import logging

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from .config import Settings, load_settings
from .protocol import MAX_MESSAGE_BYTES
from .router import Action, SignalingRouter

LOGGER = logging.getLogger("rkmgw.signaling")
AUTH_TIMEOUT_SECONDS = 10  # 首条 register/auth 的最长等待时间。
OUTBOX_CAPACITY = 64  # 单连接的有界发送队列，避免慢客户端无限积压消息。
MAX_CONNECTIONS = 512  # 同时接入的 WebSocket 上限；与会话数上限独立。


class SignalingServer:
    """管理 WebSocket 生命周期；身份和会话路由交给 SignalingRouter。"""

    def __init__(self, settings: Settings) -> None:
        self.router = SignalingRouter(settings.device_tokens, settings.browser_tokens)
        # 每条连接单独排队和顺序发送；慢连接不会阻塞其他连接的消息处理。
        self._outboxes: dict[ServerConnection, asyncio.Queue[Action]] = {}

    async def handle_connection(self, websocket: ServerConnection) -> None:
        """按请求路径确定角色，接收并路由消息，断开时清理关联会话。"""
        # 路径是角色的唯一来源；客户端 JSON 不能自称为另一种角色。
        path = websocket.request.path
        if path not in ("/device", "/browser"):
            await websocket.close(code=1008, reason="unknown signaling path")
            return
        if len(self._outboxes) >= MAX_CONNECTIONS:
            await websocket.close(code=1013, reason="connection limit reached")
            return
        role = path[1:]
        outbox: asyncio.Queue[Action] = asyncio.Queue(maxsize=OUTBOX_CAPACITY)
        self._outboxes[websocket] = outbox
        self.router.connect(websocket, role)
        writer = asyncio.create_task(self._writer(websocket, outbox))
        LOGGER.info("connection opened role=%s", role)
        try:
            # 未认证连接超时即关闭，避免连接占满后无人真正登记设备。
            first_message = await asyncio.wait_for(websocket.recv(), AUTH_TIMEOUT_SECONDS)
            actions = self.router.process(websocket, first_message)
            self._dispatch(actions)
            # 首条消息被拒绝时让 writer 先发送错误，再执行 WebSocket 关闭。
            if any(action.peer is websocket and action.close_code is not None for action in actions):
                await websocket.wait_closed()
                return
            # 通过认证后，router 根据会话归属决定每条消息的目标连接。
            async for raw in websocket:
                self._dispatch(self.router.process(websocket, raw))
        except asyncio.TimeoutError:
            LOGGER.warning("authentication timeout role=%s", role)
            await websocket.close(code=1008, reason="authentication timeout")
        except ConnectionClosed as error:
            # 保留 WebSocket 关闭方向、状态码和原因，用于区分客户端主动关闭、
            # 代理断链以及服务端 keepalive ping 超时；消息正文和凭据不进入日志。
            LOGGER.warning("connection interrupted role=%s detail=%s", role, error)
        finally:
            # 先禁止继续向本连接排队，再移除路由并通知仍在线的对端。
            # 设备断线会关闭它承载的全部浏览会话；浏览器断线只影响自己的会话。
            self._outboxes.pop(websocket, None)
            self._dispatch(self.router.disconnect(
                websocket, reason="device_offline" if role == "device" else "client_closed"))
            writer.cancel()
            try:
                await writer
            except asyncio.CancelledError:
                pass
            LOGGER.info("connection closed role=%s code=%s reason=%r",
                        role, websocket.close_code, websocket.close_reason)

    def _dispatch(self, actions: list[Action]) -> None:
        """按路由器返回的顺序入队，不在收包回调内等待网络发送。"""
        for action in actions:
            outbox = self._outboxes.get(action.peer)
            if outbox is None:
                continue
            try:
                outbox.put_nowait(action)
            except asyncio.QueueFull:
                # 丢弃整条失控连接，让 handler 的 finally 统一清理相关会话。
                LOGGER.warning("closing slow signaling connection: outbound queue full")
                asyncio.create_task(action.peer.close(code=1013, reason="outbound queue full"))

    @staticmethod
    async def _writer(websocket: ServerConnection, outbox: asyncio.Queue[Action]) -> None:
        """独占一条连接的发送方向，保证消息先于随后的关闭动作发出。"""
        try:
            while True:
                action = await outbox.get()
                if action.message is not None:
                    payload = json.dumps(action.message, separators=(",", ":"), ensure_ascii=False)
                    await websocket.send(payload)
                if action.close_code is not None:
                    await websocket.close(code=action.close_code, reason="signaling policy")
                    return
        except ConnectionClosed:
            return


async def run(settings: Settings) -> None:
    """启动明文回环监听；公网 WSS 由同机反向代理提供。"""
    # 拒绝把未加 TLS 的监听器直接暴露到公网。
    if settings.host != "127.0.0.1":
        raise ValueError("plain WebSocket listener must bind to 127.0.0.1")
    application = SignalingServer(settings)
    async with serve(
        application.handle_connection,
        settings.host,
        settings.port,
        max_size=MAX_MESSAGE_BYTES,
        max_queue=16,
        ping_interval=20,
        ping_timeout=20,
        compression=None,
    ) as listener:
        LOGGER.info("signaling listener started address=%s:%d", settings.host, settings.port)
        await listener.serve_forever()


def main() -> None:
    """从私有配置加载凭据并运行独立服务器，启动失败返回非零状态。"""
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s %(message)s")
    try:
        settings = load_settings()
        asyncio.run(run(settings))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        LOGGER.error("signaling server startup failed: %s", error)
        raise SystemExit(1) from error
    except KeyboardInterrupt:
        LOGGER.info("signaling server stopped")
