"""云端信令 v1 的 JSON 字段校验及错误消息构造。"""

from __future__ import annotations

import json
from typing import Any

VERSION = 1  # 顶层 version；与直连设备模式的旧消息格式明确区分。
MAX_MESSAGE_BYTES = 256 * 1024  # 单条 WebSocket 文本消息的最大 UTF-8 字节数。
MAX_SDP_LENGTH = 128 * 1024  # SDP 字符串长度上限，防止异常 Offer 占用大量内存。
MAX_CANDIDATE_LENGTH = 4096  # 单个 ICE candidate 字符串长度上限。
MAX_STREAMS = 16  # 单台设备登记的最多码流数。


class ProtocolError(Exception):
    """客户端消息违反协议；code 是返回给客户端的稳定错误类型。"""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


def parse_message(raw: str) -> dict[str, Any]:
    """解析单条文本帧，要求 JSON 对象和精确的整数版本号 1。"""
    if not isinstance(raw, str) or len(raw.encode("utf-8")) > MAX_MESSAGE_BYTES:
        raise ProtocolError("invalid_message", "message must be a bounded text frame")
    try:
        message = json.loads(raw)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ProtocolError("invalid_message", "malformed JSON") from error
    if not isinstance(message, dict):
        raise ProtocolError("invalid_message", "JSON object required")
    # bool 在 Python 中是 int 的子类，因此用 type(...) 精确排除 true/false。
    if type(message.get("version")) is not int or message["version"] != VERSION:
        raise ProtocolError("invalid_message", "unsupported protocol version")
    required_string(message, "type", 32)
    return message


def required_string(message: dict[str, Any], name: str, maximum: int = 128) -> str:
    """取必填非空字符串，拒绝数字、空串和超长值，不做隐式类型转换。"""
    value = message.get(name)
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise ProtocolError("invalid_message", f"invalid {name}")
    return value


def required_streams(message: dict[str, Any]) -> frozenset[str]:
    """校验设备声明的 stream_id 列表，拒绝空项、重复项和超量登记。"""
    value = message.get("streams")
    if not isinstance(value, list) or not 1 <= len(value) <= MAX_STREAMS:
        raise ProtocolError("invalid_message", "invalid streams")
    streams: set[str] = set()
    for stream in value:
        if not isinstance(stream, str) or not stream or len(stream) > 64 or stream in streams:
            raise ProtocolError("invalid_message", "invalid streams")
        streams.add(stream)
    return frozenset(streams)


def error_message(code: str, message: str, *, request_id: str = "", session_id: str = "") -> dict[str, Any]:
    """构造协议错误；仅附带已检查长度的关联 ID，不附带原始请求或 token。"""
    result: dict[str, Any] = {"version": VERSION, "type": "error", "code": code, "message": message}
    if request_id:
        result["request_id"] = request_id
    if session_id:
        result["session_id"] = session_id
    return result
