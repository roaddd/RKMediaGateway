# RK3568 MPP多实例编码能力测试

`mpp_encoder_capacity_test` 用多个并发线程分别驱动独立的MPP H.264编码器实例，验证指定主、子码流组合能否持续达到目标帧率。

该程序只测试CPU内存到MPP编码器的路径，不包含Sensor、ISP、V4L2、音频和网络输出。因此，测试通过只代表编码模块具备相应能力，之后仍需使用完整的`rkmgw`链路验证。

## 编译

```bash
cd build
sh build.sh mpp_encoder_capacity_test Release
```

## 推荐测试顺序

```bash
# 当前常规组合：1080p30 + 720p30
./mpp_encoder_capacity_test --preset current --duration 600

# 单路主码流上限：1080p60
./mpp_encoder_capacity_test --preset main60 --duration 600

# 目标组合：1080p60 + 720p30
./mpp_encoder_capacity_test --preset target --duration 600

# 探索性压力组合：1080p60 + 720p60
./mpp_encoder_capacity_test --preset stress --duration 600
```

也可以指定任意组合：

```bash
./mpp_encoder_capacity_test \
  --duration 600 \
  --stream 1920x1080@60:12582912:60 \
  --stream 1280x720@30:2097152:30
```

码流参数格式为：

```text
WIDTHxHEIGHT@FPS:BITRATE:GOP
```

## 系统状态监控

系统监控默认开启，每秒采样一次，并实时刷新到当前目录下的
`mpp_capacity_metrics.csv`。CSV记录以下内容：

- SoC和GPU温度；
- CPU使用率及当前频率；
- DDR负载及当前频率；
- RKVENC时钟使能状态及频率；
- 系统可用内存。

RK3568的RKVENC频率从`/sys/kernel/debug/clk/clk_summary`读取。运行测试前应确认
debugfs已挂载：

```bash
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug
```

可以修改采样周期和输出文件：

```bash
./mpp_encoder_capacity_test \
  --preset target \
  --duration 600 \
  --metrics-interval 1000 \
  --metrics-file target_1080p60_720p30.csv
```

采样周期允许范围为100～60000毫秒。如果只希望测试编码能力，也可以关闭监控：

```bash
./mpp_encoder_capacity_test --preset target --duration 600 --no-metrics
```

某个系统节点不存在时，该字段写为`N/A`并打印一次警告，不会影响编码能力
测试继续执行，也不会改变最终的`PASS/FAIL`判定。

程序使用8幅预生成的动态NV12图案循环输入。这样既能提供帧间变化，又避免逐帧生成图像的CPU开销污染测试结果。

## 结果判定

每路码流同时满足以下条件才返回`PASS`：

- 编码失败次数为0；
- 编码空包次数为0；
- 实际编码FPS不低于目标FPS的98%。

输出还包含平均、P95、P99和最大编码耗时以及错过目标周期的次数。`deadline_misses`可用于观察调度抖动，但偶发超期不会单独判定失败，最终以持续输出FPS和编码错误为主要结论。

10分钟测试通过后，建议再运行1小时，并同时记录温度、RKVENC频率、CPU和DDR状态。最后还要用相同配置运行完整`rkmgw`，验证Sensor、ISP、双路采集和协议输出共同工作时仍能达到目标。
