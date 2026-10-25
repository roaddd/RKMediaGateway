# RK3568 MPP测试

本目录按照测试对象划分MPP相关测试，避免把底层独立压力测试的结论直接外推到正式
网关视频链路。

## 目录说明

| 目录 | 测试对象 | 说明 |
|---|---|---|
| `standalone_capacity/` | V4L2、DMA-BUF与MPP独立测试链路 | 保存既有测试程序、公共统计代码、测试说明和历史结果 |
| `gateway_pipeline/` | RKMediaGateway正式视频链路 | 用于后续验证采集线程、MediaFrameSource、编码worker与MPP的完整过程 |
| `问题解释/` | 两类测试共用的概念文档 | 说明长尾、in-flight和异步流水线等概念 |

## 测试边界

`standalone_capacity`中的sequence gap只能说明独立测试链路中的V4L2取帧情况，不能
直接等同于正式网关中的采集丢帧。正式网关还包含独立采集线程、latest-frame缓存、
编码worker和输出分发，因此应在`gateway_pipeline`中单独验证。

现有测试程序的构建目标和可执行文件名称保持不变，详细命令参见
[独立容量测试说明](standalone_capacity/README.md)。
