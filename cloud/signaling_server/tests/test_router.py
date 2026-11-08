"""Protocol and routing tests that do not require a WebSocket connection."""

from __future__ import annotations

import json
import unittest

from cloud.signaling_server.router import SignalingRouter


def wire(kind: str, **fields: object) -> str:
    """Encode one valid-version test message."""
    return json.dumps({"version": 1, "type": kind, **fields})


class RouterTest(unittest.TestCase):
    def setUp(self) -> None:
        self.router = SignalingRouter(
            {"rk3568-001": "device-secret"},
            {"browser-secret": frozenset({"rk3568-001"}),
             "other-secret": frozenset()},
        )
        self.device = object()
        self.browser = object()
        self.router.connect(self.device, "device")
        self.router.connect(self.browser, "browser")

    def register(self, peer: object | None = None) -> None:
        target = peer or self.device
        actions = self.router.process(target, wire(
            "register", device_id="rk3568-001", streams=["main", "sub"], token="device-secret"))
        self.assertEqual(actions[-1].message["type"], "registered")

    def authenticate(self, peer: object | None = None, token: str = "browser-secret") -> None:
        target = peer or self.browser
        actions = self.router.process(target, wire("auth", token=token))
        self.assertEqual(actions[0].message["type"], "authenticated")

    def open(self, browser: object | None = None, stream_id: str = "main",
             device: object | None = None) -> str:
        target = browser or self.browser
        actions = self.router.process(target, wire(
            "open", request_id="req-1", device_id="rk3568-001", stream_id=stream_id))
        self.assertEqual(len(actions), 2)
        self.assertIs(actions[0].peer, device or self.device)
        self.assertIs(actions[1].peer, target)
        self.assertNotIn("request_id", actions[0].message)
        self.assertEqual(actions[1].message["request_id"], "req-1")
        return actions[1].message["session_id"]

    def test_full_negotiation_and_close(self) -> None:
        self.register()
        self.authenticate()
        session_id = self.open()
        offer = self.router.process(self.browser, wire("offer", session_id=session_id, sdp="v=0\r\n"))
        self.assertEqual(offer[0].message["sdp"], "v=0\r\n")
        browser_ice = self.router.process(self.browser, wire(
            "candidate", session_id=session_id, candidate="candidate:browser", mid="0"))
        self.assertIs(browser_ice[0].peer, self.device)
        answer = self.router.process(self.device, wire("answer", session_id=session_id, sdp="v=0\r\n"))
        self.assertIs(answer[0].peer, self.browser)
        device_ice = self.router.process(self.device, wire(
            "candidate", session_id=session_id, candidate="candidate:device", mid="1"))
        self.assertIs(device_ice[0].peer, self.browser)
        closed = self.router.process(self.browser, wire("close", session_id=session_id))
        self.assertEqual(closed[0].message["type"], "close")
        self.assertIs(closed[0].peer, self.device)
        self.assertEqual(self.router.process(self.browser, wire("close", session_id=session_id)), [])
        late = self.router.process(self.device, wire("candidate", session_id=session_id,
                                                     candidate="late", mid="0"))
        self.assertEqual(late[0].message["code"], "session_not_found")

    def test_multiple_browsers_have_independent_sessions(self) -> None:
        second_browser = object()
        self.router.connect(second_browser, "browser")
        self.register()
        self.authenticate()
        self.authenticate(second_browser)
        first = self.open()
        second = self.open(second_browser, "sub")
        self.assertNotEqual(first, second)
        self.router.process(self.browser, wire("close", session_id=first))
        routed = self.router.process(second_browser, wire("offer", session_id=second, sdp="v=0"))
        self.assertIs(routed[0].peer, self.device)

    def test_session_cannot_be_spoofed(self) -> None:
        attacker = object()
        self.router.connect(attacker, "browser")
        self.register()
        self.authenticate()
        self.authenticate(attacker)
        session_id = self.open()
        result = self.router.process(attacker, wire("offer", session_id=session_id, sdp="v=0"))
        self.assertEqual(result[0].message["code"], "unauthorized")
        self.assertIs(result[0].peer, attacker)

    def test_authentication_and_access_control(self) -> None:
        premature = self.router.process(self.browser, wire(
            "open", request_id="req-1", device_id="rk3568-001", stream_id="main"))
        self.assertEqual(premature[0].message["code"], "unauthorized")
        self.assertEqual(premature[1].close_code, 1008)

        other_browser = object()
        self.router.connect(other_browser, "browser")
        self.authenticate(other_browser, "other-secret")
        self.register()
        denied = self.router.process(other_browser, wire(
            "open", request_id="req-2", device_id="rk3568-001", stream_id="main"))
        self.assertEqual(denied[0].message["code"], "unauthorized")
        self.assertEqual(denied[0].message["request_id"], "req-2")

    def test_missing_stream_and_invalid_candidate(self) -> None:
        self.register()
        self.authenticate()
        missing = self.router.process(self.browser, wire(
            "open", request_id="req-1", device_id="rk3568-001", stream_id="missing"))
        self.assertEqual(missing[0].message["code"], "stream_not_found")
        session_id = self.open()
        early = self.router.process(self.browser, wire(
            "candidate", session_id=session_id, candidate="candidate:x", mid="0"))
        self.assertEqual(early[0].message["code"], "invalid_message")
        self.router.process(self.browser, wire("offer", session_id=session_id, sdp="v=0"))
        no_mid = self.router.process(self.browser, wire(
            "candidate", session_id=session_id, candidate="candidate:x"))
        self.assertEqual(no_mid[0].message["code"], "invalid_message")

    def test_disconnect_and_device_takeover(self) -> None:
        self.register()
        self.authenticate()
        first = self.open()
        replacement = object()
        self.router.connect(replacement, "device")
        actions = self.router.process(replacement, wire(
            "register", device_id="rk3568-001", streams=["main"], token="device-secret"))
        self.assertEqual(actions[0].message["reason"], "device_offline")
        self.assertIs(actions[0].peer, self.browser)
        self.assertEqual(actions[1].close_code, 1008)
        self.assertEqual(actions[2].message["type"], "registered")
        self.assertEqual(self.router.disconnect(self.device), [])
        second = self.open(device=replacement)
        self.assertNotEqual(first, second)
        self.assertEqual(self.router.disconnect(self.browser)[0].peer, replacement)

    def test_invalid_message_limits(self) -> None:
        oversized = self.router.process(self.device, "x" * (256 * 1024 + 1))
        self.assertEqual(oversized[0].message["code"], "invalid_message")
        self.assertEqual(oversized[1].close_code, 1008)
        wrong_version = self.router.process(self.device, '{"version":true,"type":"register"}')
        self.assertEqual(wrong_version[0].message["code"], "invalid_message")
        too_deep = self.router.process(self.device, "[" * 2000 + "]" * 2000)
        self.assertEqual(too_deep[0].message["code"], "invalid_message")

    def test_session_limit_is_enforced(self) -> None:
        self.register()
        self.authenticate()
        for _ in range(4):
            self.open()
        limit = self.router.process(self.browser, wire(
            "open", request_id="req-extra", device_id="rk3568-001", stream_id="main"))
        self.assertEqual(limit[0].message["code"], "resource_limit")


if __name__ == "__main__":
    unittest.main()
