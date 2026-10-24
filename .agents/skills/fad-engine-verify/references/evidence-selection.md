# 按合同选择证据

| 修改/请求 | 首选证据 | 必要时补充 |
| --- | --- | --- |
| 文字/skill/非执行说明 | 当前源码对照、链接/frontmatter/脚本行为 | 只对新增可运行示例编译运行；不重跑全部DSP。 |
| Core math/算法 | 当前独立primitive/voice/full-render与冻结差分 | rate/block/输入domain、分块一致、相关产品调用。 |
| 参数/状态/automation | 默认值和manifest、state roundtrip、timeline/restore | Preview/host winner、命名host回读。 |
| bank/Noise/history | publication/lifetime、资源budget、快速更替、teardown | ASan/TSan、真实consumer fixture。 |
| FX/graph | processor/module/graph差分、bypass/tail/latency | host PDC/切换和生命周期。 |
| 依赖隔离 | 当前源码闭包、transitive inputs/link/runtime dependency | 禁止私有/JUCE/网络读取的独立构建；不只grep。 |
| 性能 | 正常优化配置、同输入配对和完整raw观测 | 原RT正控制、原Noise80全矩阵、目标硬件callback。 |
| VST® 3 adapter | 固定SDK、validator与console host实际module/state/audio | 命名DAW；其他host/platform单列NOT_RUN。 |

精确注册项从当前CMake和 `ctest -N` 获取，Core六项检查之外，最小示例按配置另注册。不要从表里的短描述猜target或用历史二进制替代。

## 完整 Noise80

任务确需完整性能时，先看当前NoisePerformanceTests和独立verifier，保留原矩阵、原预算和阈值。检查每组完成、raw callback数量/次序、valid threadCPU、wall/CPU deadline、allocation、finite/nonSilent、retirement、rapid transitions和质量标记。必要的配对按原任务范围执行，不把小范围DSP修复一律升级为10轮。

本仓库不分发产品私有Noise80 harness；若任务提供外部工具，先读实际help/source与binary入口，不虚构缺失工具的结果。有压缩raw CSV时通过解压hash核对原数据，不删除原始观测或选择性保留最快轮。

## 合理的交付摘要

```text
修改和保持的合同：
本次source / build / binary：
实际运行：命令、receipt、PASS/FAIL。
基线原有失败：
未运行：具体范围和原因。
结论：仅覆盖哪一层；仍需哪条证据。
清理：删了自己的什么；保留什么以及用途。
```

明确缺口即可。没有请求正式发行时，不把法务、签名、账户生命周期和跨平台认证都变成普通DSP开发的新审批流程。


---

<img src="../../../../assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
