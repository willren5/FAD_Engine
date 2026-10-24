---
name: fad-engine-assets
description: 开发或修复 FAD Engine 的 wavetable、Noise PCM、曲线/history、bank generation、reader pin 和资源退役。用于资源准备与运行时发布问题；文件UI和账户流程不在该范围。
---

# FAD Engine 资源与生命周期

围绕同一 `RuntimeWavetableBank` 完成prepared asset → 完整snapshot → audio pin → safe retirement。先追实际资源owner和退役路径，不新增与现有bank并行的另一套生命周期。

## 源码入口

路径相对于本仓库：

- `include/fad/engine/assets/WavetableData.h`、WavetableBank及对应 `.cpp`。
- RenderEngine的Noise ownership、`publishNoiseSampleSlot`、`commitQueuedWavetableTransaction`和deferred release。
- 曲线/history通过现有curve owner、bank和graph连接；NoiseSamplePlayer消费PCM。
- 下游metadata/serialization由其codec/asset attachment负责，不向Core引入文件或UI依赖。

## 实现边界

1. 冻结原始输入、目标state和任务identity；在非音频侧验证尺寸/domain、生成frames/mips/PCM/native curves。发布后数据不可原地修改。
2. 查清字段单位和布局。PCM packing不同于循环表；不把storage frame容量解释成音频时长，不丢sampleRate/channels/length和历史语义。
3. 共同影响声音的asset与snapshot generation匹配发布。音频侧使用现有resolution guard → cohort pin → raw block view协议；memory order改变需证明整套handoff正确。
4. queue/commit、shared-owner查询、mipmap准备和Noise发布属于控制侧。活动reader存在时不要用无retire-list的直接commit旁路。
5. 容量不足在publication前受控失败，保留旧声音；不能扩容、阻塞或让callback成为复杂资源的最终析构者。
6. worker结果迟到时检查取消/epoch/当前权威；host restore或新patch获胜后旧任务不可覆盖。名称/路径等私有信息留在attachment/codec，Core不读取。

## 验证

优先独立AssetPublication和FullRender，再按变化使用私有Wavetable、NoiseLocalPublication、ResourceStateCopy、DrawingHistory、ThreadSafety、SharedSoundCoreNoiseIntegration等实际注册项。只选当前真实fixture已注册的用例。

验证快速更替、满队列拒绝、generation不匹配、旧reader、reset/restore、关闭/析构和可用音频恢复。运行时分配/销毁有正控制时才作相应RT审计结论；static lifetime推理与实际压力证据分开。

交付资源布局与owner、完整发布/退役过程、capacity/失败策略和实际receipt。素材权利不明的文件不进入公共fixture；日常DSP开发不扩大到账户或发行。开发指南第8、13章可用于补充。
