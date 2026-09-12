# RK3568/RK809 ALSA Playback 硬件验证记录

## 1. 验证目的

在实现 WebRTC 双向对讲的 `audioPlayback` 模块之前，先确认 RK3568 开发板上 RK809 声卡的以下能力：

1. ALSA Playback 设备节点已正常注册。
2. 播放硬件支持目标 PCM 格式。
3. `960` 帧 period、`3840` 帧 buffer 的配置可用。
4. RK809 耳机输出路径可以打开。
5. 录音与播放可以同时运行，满足全双工对讲的硬件前提。

本次不验证 J33 扬声器路径，因为当前硬件尚未焊接该器件。

## 2. 硬件和软件环境

- 处理器：Rockchip RK3568
- 音频 Codec：RK809/RK817 HIFI Codec
- ALSA 声卡：`card 0: rockchiprk809co`
- PCM 设备：`hw:0,0`
- 目标格式：48 kHz、S16_LE、双声道、交错存储
- 目标 period：960 帧（20 ms）
- 目标 buffer：3840 帧（4 个 period，80 ms）

## 3. 确认 Playback 设备节点

执行：

```sh
aplay -l
aplay -L
cat /proc/asound/cards
cat /proc/asound/devices
ls -l /proc/asound/card0/
```

关键结果：

```text
card 0: rockchiprk809co [rockchip,rk809-codec]
device 0: fe410000.i2s-rk817-hifi rk817-hifi-0

[ 0- 0]: digital audio playback
[ 0- 0]: digital audio capture

pcm0c
pcm0p
```

结论：

- `pcm0c` 是采集节点。
- `pcm0p` 是播放节点。
- 采集和播放节点都已正常注册。
- `hw:0,0`、`default:CARD=rockchiprk809co` 和 `sysdefault:CARD=rockchiprk809co` 可用。

## 4. 查询 Playback 硬件参数范围

执行：

```sh
aplay -D hw:0,0 --dump-hw-params \
  -f S16_LE \
  -r 48000 \
  -c 2 \
  -d 1 \
  -t raw /dev/zero
```

关键结果：

```text
ACCESS:      MMAP_INTERLEAVED RW_INTERLEAVED
FORMAT:      S16_LE S24_LE S32_LE
CHANNELS:    [2 8]
RATE:        [8000 96000]
PERIOD_SIZE: [8 65536]
BUFFER_SIZE: [16 131072]
```

结论：

- 硬件支持 `RW_INTERLEAVED`。
- 硬件支持 `S16_LE / 48000 Hz / 2 channels`。
- `hw:0,0` 硬件侧的通道数范围是 2～8，不支持直接配置为单声道。
- WebRTC Opus 解码得到单声道 PCM 时，在写入硬件前需要将单声道复制成双声道。

## 5. 验证目标 period 和 buffer

执行：

```sh
aplay -D hw:0,0 \
  -f S16_LE \
  -r 48000 \
  -c 2 \
  --period-size=960 \
  --buffer-size=3840 \
  -v \
  -d 5 \
  -t raw /dev/zero
```

实际生效的参数：

```text
stream          : PLAYBACK
access          : RW_INTERLEAVED
format          : S16_LE
channels        : 2
rate            : 48000
buffer_size     : 3840
period_size     : 960
period_time     : 20000 us
avail_min       : 960
start_threshold : 3840
stop_threshold  : 3840
```

在 `/proc/asound/card0/pcm0p/sub0/hw_params` 中也观察到：

```text
access: RW_INTERLEAVED
format: S16_LE
channels: 2
rate: 48000 (48000/1)
period_size: 960
buffer_size: 3840
```

参数换算：

```text
period 时长 = 960 / 48000 = 20 ms
buffer 时长 = 3840 / 48000 = 80 ms
每个 period 字节数 = 960 帧 × 2 声道 × 2 字节 = 3840 字节
buffer 总字节数 = 3840 帧 × 2 声道 × 2 字节 = 15360 字节
```

`/dev/zero` 是静音数据，这一步用于验证参数协商和 PCM 运行状态，不用于判断物理输出是否有声音。

## 6. 确认 RK809 播放路由

查询 mixer 控件：

```sh
amixer -c 0 scontrols
amixer -c 0 contents
```

RK809 提供的 `Playback Path` 选项包括：

```text
OFF
RCV
SPK
HP
HP_NO_MIC
BT
SPK_HP
RING_SPK
RING_HP
RING_HP_NO_MIC
RING_SPK_HP
```

默认值为 `OFF`，此时即使 PCM 数字流已经运行，也没有打开耳机或扬声器的模拟输出路径。

本次使用耳机路径：

```sh
amixer -c 0 cset name='Playback Path' HP
```

返回：

```text
: values=3
```

说明 `Playback Path` 已切换到 `HP`。设备系统中没有安装 `speaker-test`，因此使用 WAV 文件和 `aplay` 完成播放验证。

## 7. 普通文件播放的缓冲参数

未显式指定 period 和 buffer 时，`aplay` 曾自动选择：

```text
period_size     : 6000
buffer_size     : 24000
avail_min       : 6000
start_threshold : 24000
delay           : 18256
```

对应时长：

```text
period          = 6000 / 48000 = 125 ms
buffer          = 24000 / 48000 = 500 ms
delay           = 18256 / 48000 ≈ 380.3 ms
```

该配置可以用于普通文件播放，但会显著增加对讲延迟，不能作为 `audioPlayback` 的默认配置。

## 8. 全双工验证

同时运行 `arecord` 和 `aplay`，然后分别查询采集和播放状态：

```sh
cat /proc/asound/card0/pcm0c/sub0/status
cat /proc/asound/card0/pcm0p/sub0/status
```

采集侧实际状态：

```text
state: RUNNING
owner_pid: 734
delay: 488
avail: 488
avail_max: 960
hw_ptr: 349944
appl_ptr: 349456
```

播放侧实际状态：

```text
state: RUNNING
owner_pid: 735
delay: 3112
avail: 728
avail_max: 2880
hw_ptr: 180264
appl_ptr: 183376
```

分析：

- 采集和播放由不同进程同时持有，两个 PCM 都处于 `RUNNING` 状态。
- 未出现 `Device or resource busy`，说明当前驱动和声卡支持全双工。
- 未观察到 `XRUN`、`underrun` 或 `overrun` 状态。
- 播放侧 `delay + avail = 3112 + 728 = 3840` 帧，与配置的 buffer 容量一致。
- 播放侧尚有 3112 帧等待硬件播放，对应约 64.8 ms。
- 采集侧有 488 帧等待应用读取，对应约 10.2 ms。
- `hw_ptr` 和 `appl_ptr` 都在有效推进，说明应用和硬件正在正常交换 PCM 数据。

## 9. audioPlayback 模块的配置基线

根据本次验证，第一版 `audioPlayback` 可以采用以下基线：

```text
device          = "hw:0,0"
access          = RW_INTERLEAVED
sample_format   = S16_LE
sample_rate     = 48000
channels        = 2
period_frames   = 960
buffer_periods  = 4
buffer_frames   = 3840
avail_min       = 960
start_threshold = 1920
```

`start_threshold=1920` 表示先积累两个 period（40 ms）再启动播放，在启动延迟和抗线程调度抖动之间取平衡。稳定后可继续测试降为 `960`。

WebRTC 下行 Opus 解码按 48 kHz 单声道输出时，写入 RK809 前应转换为双声道交错 PCM：

```text
mono[0] -> left[0], right[0]
mono[1] -> left[1], right[1]
...
```

写入接口应以“帧”为单位；对于上述双声道 S16_LE 格式，一帧占 4 字节。

## 10. 未验证项和范围边界

- J33 扬声器硬件未焊接，本次不验证 `SPK` 路径。
- 本次验证的物理输出是 RK809 耳机 `HP` 路径。
- 本次只验证本地 PCM 全双工能力，不包括 WebRTC 网络传输、Opus 解码、抖动缓冲、丢包隐藏和回声消除。
- `Playback Path` 默认为 `OFF`。后续需明确它由系统启动脚本、业务配置还是 `audioPlayback` 初始化流程负责打开，不能依赖人工执行 `amixer`。

## 11. 最终结论

RK3568/RK809 当前软硬件环境已满足 WebRTC 双向对讲的基础播放条件：

1. Playback PCM 节点存在且能正常进入 `RUNNING` 状态。
2. 48 kHz、S16_LE、双声道、20 ms period 和 80 ms buffer 均已成功配置。
3. RK809 `HP` 播放路径可以打开。
4. 采集和播放可同时运行，未观察到 PCM 资源冲突或 XRUN。
5. 可以进入独立 `audioPlayback` 模块及其测试程序的实现阶段。

