"""从仓库外的私有文件读取云端信令凭据和监听配置。"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    """启动配置；明文 WebSocket 仅绑定回环地址，由 WSS 反向代理对外服务。"""

    host: str  # 监听地址，固定为 127.0.0.1，禁止公开明文 WebSocket。
    port: int  # 回环监听端口。
    device_tokens: dict[str, str]  # device_id -> 设备 token。
    browser_tokens: dict[str, frozenset[str]]  # 浏览器 token -> 允许访问的 device_id 集合。


def load_settings(auth_file: str | None = None) -> Settings:
    """读取并校验私有凭据文件；路径来自参数或 RKMGW_SIGNAL_AUTH_FILE。"""
    path_text = auth_file or os.environ.get("RKMGW_SIGNAL_AUTH_FILE", "")
    if not path_text:
        raise ValueError("RKMGW_SIGNAL_AUTH_FILE is required")
    path = Path(path_text)
    with path.open("r", encoding="utf-8") as source:
        data = json.load(source)
    if not isinstance(data, dict):
        raise ValueError("authentication file must contain a JSON object")

    # 启动期一次性校验 ACL：浏览器只可访问已经配置了设备凭据的 ID。
    devices = data.get("device_tokens")
    browsers = data.get("browser_tokens")
    if not isinstance(devices, dict) or not devices:
        raise ValueError("device_tokens must be a nonempty object")
    if not isinstance(browsers, dict) or not browsers:
        raise ValueError("browser_tokens must be a nonempty object")
    if any(not isinstance(name, str) or not name or
           not isinstance(token, str) or not token for name, token in devices.items()):
        raise ValueError("device_tokens contains an invalid entry")

    browser_tokens: dict[str, frozenset[str]] = {}
    for token, allowed in browsers.items():
        if (not isinstance(token, str) or not token or not isinstance(allowed, list) or
                not allowed or any(not isinstance(name, str) or not name or
                                   name not in devices for name in allowed)):
            raise ValueError("browser_tokens contains an invalid entry")
        browser_tokens[token] = frozenset(allowed)

    # 端口可以由部署环境覆盖，但监听地址始终保持回环地址。
    port_text = os.environ.get("RKMGW_SIGNAL_PORT", "8765")
    try:
        port = int(port_text)
    except ValueError as error:
        raise ValueError("RKMGW_SIGNAL_PORT must be an integer") from error
    if not 1 <= port <= 65535:
        raise ValueError("RKMGW_SIGNAL_PORT must be between 1 and 65535")
    return Settings("127.0.0.1", port, devices, browser_tokens)
