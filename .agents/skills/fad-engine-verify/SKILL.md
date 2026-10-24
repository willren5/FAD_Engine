---
name: fad-engine-verify
description: 为指定 FAD Engine 修改选择并执行相关验证，核对source/binary、数值/状态/资源/RT和宿主证据，保留原有失败。适用于验证计划或完成审计，不自动扩大为全仓安全扫描或产品发行。
---

# FAD Engine 验证与证据

从用户要求的结论和实际diff选择验证。只读review或禁止运行时先用现有证据/静态路径，明确缺口，不越界编译或启动宿主。

## 建立身份和范围

确认所选Core/consumer、当前源码、build source/generator/options、实际binary SHA与受影响调用。receipt中的source观察不等于构建来源；陈旧binary、零用例、skip和超时不算当前修改PASS。

使用当前CMake/CTest和精确注册名；下游有协调脚本时按其约定，不要求缺失的私有工具。检查层次见 [按合同选择证据](references/evidence-selection.md)。

## 执行与判读

1. 从复现/最小相关检查开始。失败由本次修改引起时修复并重跑受影响部分；通过后，只有新变更、失败或未闭合风险才扩大验证。
2. 音频比较固定state/assets/seed/reset/rate/block/MIDI与automation时间线，保留未归一化数据和任务开始前的阈值。A/A不代替旧版A/B。
3. 资源/并发检查覆盖完整owner、generation/epoch、pin/retire、容量与teardown；sanitizers不用于正常性能结论。
4. 性能任务保留全部配对轮次、环境和原始wall/threadCPU/RSS/质量记录。完整Noise80必须核对所有矩阵组和raw观测，correctness-only不是替代。
5. 只在修改或请求涉及宿主时运行命名host；实际module、参数枚举、窗口条件和授权模式按该host核对。独立demo无账号，产品正常音频保留真实gate。

有既有严格deadline FAIL或RSS复查标记时如实分列，不挑最快轮、不调预算/容差、不扩大成未请求的优化任务。普通用户账户不能作为可写测试夹具；账号、安装、发行无关项不阻塞DSP验证。

## 完成记录

给出修改/合同、命令与receipt、source/binary identity、PASS/FAIL/NOT_RUN及原因。区分静态、独立/离线、RT审计、性能、命名host与发布层；只声明实际证明的部分。保留原始失败和必要证据，清理自己已完成用途的build/重复产物，不清理源码、基线或他人产物。
