# S3 逐 RX 字段合并与统一提交

## 范围与来源

分支 `codex/s3-rx-coordinator` 从 `6e4f9e33c5c02eca4e83cd8a59a24a2802a5c3d8` 建立。
本次完成第一阶段：业务模块提交字段意图、统一合成、统一提交。

参考项目为用户指定的 `ESP32-P4-WIFI6-3CAN`，参考提交为 `8e0114f2653d8174bd09bdf6453ca4ccbf6bca65`。
只参考其 `vcleft_mux1_composer` 的字段掩码层、Coordinator 的源代次消费，以及发送仲裁器对接受结果的区分。
参考文件哈希和与 HEAD 的一致性见 `reference_manifest.json`。
没有导入其 3CAN 微内核、跨核调度、多总线队列、车身功能、P4 驱动、5 ms 截止值或逐帧 token。
参考目录存在其他未提交修改，本次没有修改该项目。业务编码均来自当前 S3 基线。

## 已实现

```
有效 RX -> FrameContext -> collectIntents -> FrameCoordinator.finalize
       -> TxRequest -> TxBroker -> CanDriver.sendCritical
```

- 每个有效、实际处理的 RX 分配独立的 64 位序号。相同 ID、相同 payload 的下一次 RX 仍是新事务。
- 原帧在事务中只读；业务处理器不再接收 `CanDriver&`。
- `FieldIntent` 包含功能标识、8 字节掩码和值；按车型/ID/MUX/功能校验写入权限。
- 不重叠字段合并；相同目标的重叠字段合并；冲突或越权导致整个修改事务拒绝。
- ID、DLC、MUX 不可修改；Legacy MPP、可选 ISA/Nag 的完整性字段由合成阶段集中处理。
- 每个上下文只能 finalize 一次；请求不可复制；Broker 消费请求并拒绝重复或倒序提交。
- 不保存可由定时器重新计算的旧请求，不等待其他 MUX。
- HW4 原有 `EverySource` 刷新语义保留；兼容模式 Legacy/HW3 MUX0 保留 `ChangedOnly`。
- `framesSent` 与 Dashboard `tx` 只计驱动接受。Broker 分开记录合成、接受、拒绝、重复、无修改和冲突。
- HW3 平滑的已接受值和时间只在驱动接受后推进；纯计算或拒绝不更新已接受历史。
- TWAI 入口排除扩展帧、远程帧和非法 DLC；事务再次验证帧类型。

当前 Broker 是同步、无待发送队列的单任务适配。现有 TWAI `sendCritical` 的内部有界重试保持原样，
重试同一个合成结果。Broker 无论接受还是拒绝都消费本次请求，等待下一个真实 RX。
没有引入应用层延迟重试，避免改变现有发送顺序。

## 车型输出策略

| 路径 | 合并内容 | 刷新/完整性 |
|---|---|---|
| Legacy 0x3EE MUX0，兼容模式 | FSD 请求、手动风格 | 有变化才提交 |
| HW3 0x3FD MUX0，兼容模式 | FSD 请求、手动风格 | 有变化才提交 |
| HW3 0x3FD MUX2 | 目标速度、编码、下降平滑 | 保留模式对应的原有刷新策略 |
| HW4 0x3FD MUX0/1/2 | 各 MUX 原有字段 | 保留逐源帧刷新 |
| Legacy 0x2F8 | MPP 提高 | 集中重算 checksum |
| 非 Dashboard 可选 ISA/Nag | 原有可选编码 | 集中处理 checksum/counter；未在目标固件新增启用 |

Legacy MPP 继续使用自己的开关，未把 FSD 总开关扩展成所有 CAN 功能的总开关。
WiFi/DNS、车型默认值、休眠条件、接收过滤策略和硬件引脚保持基线设置。

## 诊断语义

- `tx` / `framesSent` / `coordinator.accepted`：驱动排队或启动发送成功。
- `txerr` / `coordinator.rejected`：最终未被驱动接受。
- CSV 的 `T` 为保持格式兼容继续表示已接受的发送请求，不表示已确认 TX-done。
- `hw3OffsetLast` 和 `legacyMppLastSentRaw` 是保留的旧 JSON 键，其值现在明确表示已入队值。
- `perFrameTxDoneKnown=false`；`timeBasis=software_dequeue`。没有逐帧完成或 ECU 接受断言。
- Broker 计数从本次启动开始累计，与现有 Dashboard 可清零统计分开。

## 验证

`scripts/check_frame_migration.py` 从 Git 冻结基线提取 include，分别编译基线和移植实现，
比较每个 RX 的输出帧数量和全部输出字节。没有用移植代码自身生成期望值。

- Dashboard 2.5.2 兼容模式：229,376 次 RX。
- Dashboard 内置模式：229,376 次 RX。
- 原生可选功能模式：229,376 次 RX。
- 合计 688,128 次 RX，包含 Legacy/HW3/HW4/Nag、128 组设置、各 MUX 和连续相同 payload。
- 另有合并、字段权限、不可修改 MUX、checksum、重复提交、无变化刷新、拒绝计数和平滑提交测试。
- 原生历史测试全部迁移到事务调用入口。基线已有 11 个失败断言，主要仍预期旧 AP 门控、
  bit6 选择及观察不发送行为；已依据冻结基线修正测试名称和期望，生产行为由差分测试保证。
- Python 设置测试改用 `sys.executable`，以使用当前隔离解释器而不是 Windows Store 的 `python3` 别名。

执行：

```powershell
& .\_tools\python\Scripts\python.exe scripts\check_frame_migration.py
& .\_tools\python\Scripts\python.exe scripts\run_native_checks.py
& .\_tools\python\Scripts\python.exe -m unittest discover -s test -p 'test_*.py'
& .\scripts\build_s3_isolated.ps1
```

构建只使用本项目 `_tools` 下的 Python、PlatformIO Core 和工具包。
部分版本匹配的本机工具缓存复制到该目录，来源保存在 `_tools/*provenance.json`，未调用全局 PlatformIO。
构建脚本仅编译 `waveshare_ESP32_S3_RS485_CAN`，使用同盘 8.3 短路径避免 Windows 参数长度与 SUBST 跨盘解析问题。
构建输出独立存放在 `.pio/build-s3-rx/waveshare_ESP32_S3_RS485_CAN`，失败的旧缓存保留用于追踪。
`scripts/requirements-build.txt` 固定 Python 构建依赖。
`patch_isolated_idf_builder.py` 只修改本项目私有 PlatformIO 7.0.0 构建器：
对组件源路径两侧统一解析，再通过 JSON 参数文件执行 ldgen，避免 Windows 命令长度限制。
修改前后哈希保存在 `_tools/idf-builder-patch.json`；ESP-IDF 源码未修改。
详细测试和构建日志位于 `.tests/`，固定验证摘要随本文件保存。

## 后续阶段边界

本阶段不宣称完成配置快照/代次切换、控制器代次、源帧物理时间戳、截止时间、非阻塞应用重试，
也未修改 CAN 优先过滤与休眠的依赖集合、OTA/深睡硬件队列交接。
WebUI 配置并发更新和控制器生命周期仍沿用基线，下一阶段需要单独设计和验证。
深睡仍等待重新上电/复位，没有增加 CAN 唤醒。

本次证据是离线输出对比、测试和目标构建；未烧录、复位、连接串口或执行实车操作。
