"""End-to-end signaling transport test using real loopback WebSockets."""

from __future__ import annotations

import json
import unittest

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosedError

from cloud.signaling_server.config import Settings
from cloud.signaling_server.server import SignalingServer


async def send(websocket: object, kind: str, **fields: object) -> None:
    await websocket.send(json.dumps({"version": 1, "type": kind, **fields}))


async def receive(websocket: object) -> dict[str, object]:
    return json.loads(await websocket.recv())


class WebSocketTest(unittest.IsolatedAsyncioTestCase):
    async def test_wrong_path_and_bad_credentials_are_closed(self) -> None:
        settings = Settings("127.0.0.1", 8765,
                            {"rk3568-001": "device-secret"},
                            {"browser-secret": frozenset({"rk3568-001"})})
        application = SignalingServer(settings)
        async with serve(application.handle_connection, "127.0.0.1", 0) as listener:
            port = listener.sockets[0].getsockname()[1]
            base = f"ws://127.0.0.1:{port}"
            async with connect(base + "/unknown") as unknown:
                with self.assertRaises(ConnectionClosedError):
                    await unknown.recv()
            async with connect(base + "/device") as device:
                await send(device, "register", device_id="rk3568-001",
                           streams=["main"], token="wrong")
                self.assertEqual((await receive(device))["code"], "unauthorized")
                with self.assertRaises(ConnectionClosedError):
                    await device.recv()

    async def test_device_disconnect_notifies_browser(self) -> None:
        settings = Settings("127.0.0.1", 8765,
                            {"rk3568-001": "device-secret"},
                            {"browser-secret": frozenset({"rk3568-001"})})
        application = SignalingServer(settings)
        async with serve(application.handle_connection, "127.0.0.1", 0) as listener:
            port = listener.sockets[0].getsockname()[1]
            base = f"ws://127.0.0.1:{port}"
            async with connect(base + "/device") as device, \
                       connect(base + "/browser") as browser:
                await send(device, "register", device_id="rk3568-001",
                           streams=["main"], token="device-secret")
                await receive(device)
                await send(browser, "auth", token="browser-secret")
                await receive(browser)
                await send(browser, "open", request_id="req-1", device_id="rk3568-001",
                           stream_id="main")
                await receive(device)
                session_id = (await receive(browser))["session_id"]
                await device.close()
                closed = await receive(browser)
                self.assertEqual(closed["type"], "close")
                self.assertEqual(closed["session_id"], session_id)
                self.assertEqual(closed["reason"], "device_offline")

    async def test_two_browser_sessions_and_disconnect(self) -> None:
        settings = Settings("127.0.0.1", 8765,
                            {"rk3568-001": "device-secret"},
                            {"browser-secret": frozenset({"rk3568-001"})})
        application = SignalingServer(settings)
        async with serve(application.handle_connection, "127.0.0.1", 0) as listener:
            port = listener.sockets[0].getsockname()[1]
            base = f"ws://127.0.0.1:{port}"
            async with connect(base + "/device") as device, \
                       connect(base + "/browser") as first, \
                       connect(base + "/browser") as second:
                await send(device, "register", device_id="rk3568-001",
                           streams=["main"], token="device-secret")
                self.assertEqual((await receive(device))["type"], "registered")
                for browser in (first, second):
                    await send(browser, "auth", token="browser-secret")
                    self.assertEqual((await receive(browser))["type"], "authenticated")

                await send(first, "open", request_id="req-1", device_id="rk3568-001",
                           stream_id="main")
                device_opened = await receive(device)
                browser_opened = await receive(first)
                self.assertEqual(device_opened["session_id"], browser_opened["session_id"])
                first_id = browser_opened["session_id"]

                await send(second, "open", request_id="req-2", device_id="rk3568-001",
                           stream_id="main")
                await receive(device)
                second_id = (await receive(second))["session_id"]
                self.assertNotEqual(first_id, second_id)

                await send(first, "offer", session_id=first_id, sdp="v=0\r\n")
                self.assertEqual((await receive(device))["sdp"], "v=0\r\n")
                await send(device, "answer", session_id=first_id, sdp="v=0\r\nanswer")
                self.assertEqual((await receive(first))["type"], "answer")
                await send(first, "candidate", session_id=first_id,
                           candidate="candidate:1", mid="0")
                self.assertEqual((await receive(device))["candidate"], "candidate:1")

                await first.close()
                closed = await receive(device)
                self.assertEqual(closed["session_id"], first_id)
                await send(second, "offer", session_id=second_id, sdp="v=0\r\nsecond")
                self.assertEqual((await receive(device))["session_id"], second_id)


if __name__ == "__main__":
    unittest.main()
