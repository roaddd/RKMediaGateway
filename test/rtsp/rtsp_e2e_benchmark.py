#!/usr/bin/env python3
import argparse
import math
import os
import re
import statistics
import subprocess
import sys
import time
from typing import List, Optional, Tuple


SHOWINFO_RE = re.compile(r"showinfo", re.IGNORECASE)
E2E_SERVER_RE = re.compile(
    r"\[E2E\]\s+event=first_keyframe_sent_after_new_client.*detect_to_send_us=(\d+)"
)


def percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    s = sorted(values)
    k = (len(s) - 1) * p
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return s[int(k)]
    return s[f] * (c - k) + s[c] * (k - f)


def parse_session_from_url(url: str) -> str:
    idx = url.rfind("/")
    if idx < 0 or idx == len(url) - 1:
        return ""
    return url[idx + 1 :]


def run_one_round(url: str, transport: str, timeout_sec: float) -> Tuple[bool, Optional[float], str]:
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "info",
        "-rtsp_transport",
        transport,
        "-fflags",
        "nobuffer",
        "-flags",
        "low_delay",
        "-i",
        url,
        "-vf",
        "showinfo",
        "-an",
        "-f",
        "null",
        "-",
    ]
    start_ms = time.time_ns() // 1_000_000
    proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace")
    got_frame = False
    first_frame_delay_ms = None
    reason = ""
    try:
        deadline = time.time() + timeout_sec
        assert proc.stderr is not None
        while time.time() < deadline:
            line = proc.stderr.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.01)
                continue
            if SHOWINFO_RE.search(line):
                now_ms = time.time_ns() // 1_000_000
                first_frame_delay_ms = float(now_ms - start_ms)
                got_frame = True
                break
        if not got_frame:
            reason = "timeout_or_no_showinfo"
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=1.0)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
    return got_frame, first_frame_delay_ms, reason


def read_new_log_chunk(path: str, offset: int) -> Tuple[str, int]:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        f.seek(offset, os.SEEK_SET)
        chunk = f.read()
        new_offset = f.tell()
    return chunk, new_offset


def parse_server_detect_to_send_us(chunk: str, session_name: str) -> Optional[float]:
    for line in chunk.splitlines():
        if "event=first_keyframe_sent_after_new_client" not in line:
            continue
        if session_name and f"session={session_name}" not in line:
            continue
        m = E2E_SERVER_RE.search(line)
        if m:
            return float(m.group(1)) / 1000.0
    return None


def summary(name: str, values: List[float]) -> str:
    if not values:
        return f"{name}: no data"
    mean = statistics.mean(values)
    p95 = percentile(values, 0.95)
    return f"{name}: n={len(values)} mean={mean:.2f}ms p95={p95:.2f}ms min={min(values):.2f}ms max={max(values):.2f}ms"


def main() -> int:
    ap = argparse.ArgumentParser(description="RTSP 多轮首帧时延测试并汇总均值/P95")
    ap.add_argument("--url", required=True, help="RTSP URL, e.g. rtsp://127.0.0.1:8554/live_sub")
    ap.add_argument("--rounds", type=int, default=20, help="测试轮数，默认20")
    ap.add_argument("--timeout-sec", type=float, default=12.0, help="每轮超时时间，默认12秒")
    ap.add_argument("--transport", choices=["tcp", "udp"], default="tcp", help="RTSP transport，默认 tcp")
    ap.add_argument("--interval-sec", type=float, default=1.0, help="轮次间隔，默认1秒")
    ap.add_argument("--gateway-log", default="", help="网关日志文件路径，可选；用于提取服务端 detect_to_send_us")
    ap.add_argument("--session", default="", help="session 名称，可选；默认从 URL 最后一级路径推断")
    args = ap.parse_args()

    session_name = args.session.strip() or parse_session_from_url(args.url.strip())
    server_log_offset = 0
    if args.gateway_log:
        if not os.path.exists(args.gateway_log):
            print(f"[WARN] gateway log not found: {args.gateway_log}")
            args.gateway_log = ""
        else:
            server_log_offset = os.path.getsize(args.gateway_log)

    client_delays_ms: List[float] = []
    server_detect_to_send_ms: List[float] = []
    fail_count = 0

    print(f"[INFO] url={args.url}")
    print(f"[INFO] rounds={args.rounds} transport={args.transport} timeout={args.timeout_sec}s interval={args.interval_sec}s")
    if args.gateway_log:
        print(f"[INFO] gateway_log={args.gateway_log} session={session_name}")

    for i in range(1, args.rounds + 1):
        round_begin = time.strftime("%H:%M:%S")
        ok, delay_ms, reason = run_one_round(args.url, args.transport, args.timeout_sec)
        if ok and delay_ms is not None:
            client_delays_ms.append(delay_ms)
            print(f"[ROUND {i:02d}] {round_begin} client_first_frame={delay_ms:.2f}ms")
        else:
            fail_count += 1
            print(f"[ROUND {i:02d}] {round_begin} FAIL reason={reason}")

        if args.gateway_log:
            try:
                chunk, server_log_offset = read_new_log_chunk(args.gateway_log, server_log_offset)
                v = parse_server_detect_to_send_us(chunk, session_name)
                if v is not None:
                    server_detect_to_send_ms.append(v)
                    print(f"[ROUND {i:02d}] server_detect_to_send={v:.2f}ms")
                else:
                    print(f"[ROUND {i:02d}] server_detect_to_send=NA")
            except Exception as ex:
                print(f"[ROUND {i:02d}] server_log_parse_error={ex}")

        if i != args.rounds and args.interval_sec > 0:
            time.sleep(args.interval_sec)

    print("\n===== SUMMARY =====")
    print(summary("client_first_frame", client_delays_ms))
    if args.gateway_log:
        print(summary("server_detect_to_send", server_detect_to_send_ms))
    print(f"failed_rounds={fail_count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

