# RTSP E2E Benchmark

## 1) 脚本作用

`rtsp_e2e_benchmark.py` 会自动多轮拉流，统计：

- 客户端首帧耗时 `client_first_frame`（均值 / P95）
- 可选：服务端日志中的 `detect_to_send_us`（均值 / P95）

## 2) 使用方式

```bash
python3 test/rtsp/rtsp_e2e_benchmark.py \
  --url rtsp://127.0.0.1:8554/live_sub \
  --rounds 30 \
  --transport tcp \
  --timeout-sec 12 \
  --interval-sec 1 \
  --gateway-log /tmp/gw.log
```

说明：

- `--gateway-log` 可选；提供后会解析 `[E2E] event=first_keyframe_sent_after_new_client`。
- 依赖 `ffmpeg`。

## 3) 两种模式如何切换（无需改代码）

已新增配置项：

- `STREAM_MAIN_RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT`
- `STREAM_SUB_RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT`

请直接写在配置文件：`RKMediaGateway/main/rtsp_gateway.conf`

取值：

- `0`：不立马补发 SPS/PPS（基线模式）
- `1`：新客户端接入后立马补发缓存的 SPS/PPS（优化模式）

示例（子码流）：

```ini
STREAM_SUB_RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT=0
```

切到优化模式：

```ini
STREAM_SUB_RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT=1
```

兼容单流旧配置时可用：

```ini
GATEWAY_RTSP_IMMEDIATE_SPS_PPS_ON_NEW_CLIENT=1
```

## 4) 对比建议

1. 先用 `...=0` 跑 30 轮，记录均值/P95。  
2. 再用 `...=1` 跑 30 轮。  
3. 对比 `client_first_frame` 和 `server_detect_to_send` 的均值/P95。
