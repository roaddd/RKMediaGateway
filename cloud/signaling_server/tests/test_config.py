"""Credential-file validation tests."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cloud.signaling_server.config import load_settings


class ConfigTest(unittest.TestCase):
    def test_load_private_credentials(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "auth.json"
            path.write_text(json.dumps({
                "device_tokens": {"rk3568-001": "device-secret"},
                "browser_tokens": {"browser-secret": ["rk3568-001"]},
            }), encoding="utf-8")
            with patch.dict(os.environ, {"RKMGW_SIGNAL_PORT": "9876"}):
                settings = load_settings(str(path))
            self.assertEqual(settings.host, "127.0.0.1")
            self.assertEqual(settings.port, 9876)
            self.assertEqual(settings.browser_tokens["browser-secret"], frozenset({"rk3568-001"}))

    def test_unknown_device_in_browser_acl_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "auth.json"
            path.write_text(json.dumps({
                "device_tokens": {"rk3568-001": "device-secret"},
                "browser_tokens": {"browser-secret": ["unknown"]},
            }), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "browser_tokens"):
                load_settings(str(path))


if __name__ == "__main__":
    unittest.main()
