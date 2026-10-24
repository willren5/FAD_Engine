---
name: fad-engine-dsp
description: 实现或修复 FAD Engine 的振荡器、滤波、包络、LFO、调制、voice 和数值保护，保持声明的声音与实时合同。适用于共享 DSP 源码修改；FX graph 和资源发布分别使用对应 Engine skill。
---

# FAD Engine DSP 修改

从用户的可复现声音或明确功能开始，在真正执行的 Core owner 上完成最小、完整修改。所有路径相对于选定仓库根。

## 确定路径

- 调度、MIDI/voice选择：`src/RenderEngine.cpp`。
- 单声部、smoothing、envelope和调制应用：`src/VoiceCore.cpp`。
- 振荡器/FM/Noise：OscillatorBlock、NoiseSamplePlayer及 `oscillators/`。
- 滤波、数值和基础处理：`filters/`、`math/`、`modulation/`、`safety/`。

若下游有forwarding/facade，追实际render调用和编译源，避免修错副本，保留其他dirty修改。

## 修改

1. 固定 patch、assets、seed、reset、MIDI/automation timeline、rate与block。已有明确复现和冻结基线时直接使用，不要求再做整个仓库审查。
2. 分清原有FAIL、本次回归、预期新声音。定义要保持的旧语义；只有确需改变兼容面时准备新模式或迁移。
3. 追到第一次失效的状态/系数/输入domain，再修 owning module。保留反馈恢复、denormal、安全输出、随机调用顺序和sample-time smoothing；不通过顶层裁剪或整体归一化掩盖问题。
4. 新工作量和内存放在准备侧，callback只用预分配状态和有界计算。不要把另一个voice表、参数定义或算法副本引入产品。
5. 按实际变更运行现有相关检查和A/B。验证选择见 [DSP验证入口](references/dsp-checks.md)。失败由本次修改引起就修复并精确重跑，成功后不机械重复全套宿主。

以本仓库AGENTS.md和当前Core合同为准。若本次新增/迁移依赖API，现查维护方官方资料和实际固定版本，保留部署下限与项目补丁。

## 交付

说明具体trigger、root cause、owner、声音/兼容性选择、实际source/binary与检查。静态结论、数值/离线声音、听感、真实host和性能分别陈述。既有性能失败不能通过降阈值或correctness-only转成PASS；账号和发行无关项不阻塞独立DSP修改。
