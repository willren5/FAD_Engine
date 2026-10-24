---
name: fad-engine-state
description: 开发 FAD Engine snapshot、参数映射、sample-accurate automation、restore 和私有 Preview 接入，防止 torn state、旧事件回写和参数兼容破坏。用于状态或参数路径修改，不处理账户权益。
---

# FAD Engine 参数与状态

先识别用户要改的是独立consumer控制、Core runtime字段，还是私有产品的可保存/可自动化参数。不要把demo四控制当产品参数合同，也不要把runtime cache变成第二个preset owner。

## 读取实际 owner

- runtime：`include/fad/engine/parameters/ParameterSnapshot.h`、`ParameterTypes.h`及 `src/ParameterSnapshot.cpp` 的构造默认值。
- 独立demo：`examples/common/DemoPatch.h`、VST® 3的controller/state。
- 下游产品：自己的ID/contract/publisher、codec/Preview和wrapper；产品代码不在本仓库。
- 音频automation：RenderParameterAccess、RenderTimeline及RenderEngine的callback接受边界。

路径相对于本仓库；下游产品集成不在仓库中，不把私有能力当作Core依赖。

## 实现

1. 写清字段单位、domain/default、normalization、choice ordinal、持久化和automation语义。读整体构造函数，不能只看成员初值。
2. 新增产品参数时贯通contract → publisher → snapshot → DSP → codec/manifest/实际暴露路径；保留旧ID、enum数值、参数前缀和版本合同。
3. 共同影响声音的字段、asset generation和FX候选完整发布。bulk-write保持begin/end配对；读取/接受失败时复用上一完整状态，不在audio自旋。
4. 正在运行的实例用对应线程的发布入口。`stageIsolatedOfflineInitialSnapshot`只用于prepare前的新独占离线实例；控制侧set方法不能因为noexcept就从callback使用。
5. restore epoch使旧automation/异步候选失效；同一个真实callback的内部chunk不重新打开结构接受边界。
6. Preview未Commit时host capture保存baseline及合同允许的override；host restore获胜后旧Preview不能再Commit。reset/reseed不擅自取消Preview。

## 验证和输出

检查默认声音、sample-offset/ramp、旧状态和坏状态拒绝、preset/session回读、Preview commit/rollback/host winner及参数manifest，按影响范围选现有检查。保存raw snapshot的内存布局不是持久化方案；独立state逐字段版本化。

交付字段/ID/schema的实际变化、权威owner、线程交接、迁移和已验证范围。只读分析不写状态；DSP开发不要求改真实账号或安装发布。详细案例见工作树的开发指南第7、9、12章。


---

<img src="../../../assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
