---
name: fad-engine-fx
description: 实现或修复 FAD Engine 效果器、FxCatalog/Factory、DynamicFxChain 路由、graph切换、latency和retired tails。适用于FX执行或结构修改；独立振荡器/声部问题使用 fad-engine-dsp。
---

# FAD Engine FX 与图

先分清连续参数更新与结构变化，再修改实际Core processor或graph owner。源码入口为选定工作树的 `include/fad/engine/fx/`、`src/fx/`及RenderEngine调用链。

## 处理器与兼容

以 `BaseFxProcessorCore` 为合同，定义prepare内存/状态、process、reset、bypass、modulation、latency、tail和state-copy行为。参数domain/default/range在 `FxCatalog` 有唯一映射，`FxFactory` 创建processor；保留已有enum值和参数ordinal。

新增processor时把 `.cpp` 加到Core唯一源码列表；需要产品暴露时才接private registry/parameter/codec/UI。不要先复制整个产品或让独立demo绕过产品gate。demo默认空FX graph不代表Core没有效果器。

## 结构接受与退出

1. 连续参数复用已有processor/smoother；结构重建、分配和复杂析构在安全线程。
2. 使用既有batch/desired-state准备完整结构；只在匹配snapshot被audio接受后成为audible graph。
3. 真实callback的acceptance boundary只开一次，内部automation chunk不能再次接受不匹配结构。候选未就绪继续旧完整声音。
4. old/new transition报告有效latency上界；host通知在控制侧。退役processor保留必要tail、routing和modulation状态，不立即硬切或delete。
5. 检查oversampling、mix、delay与filter状态的真实语义；不因换了相似API就认定声音/latency等价。

## 验证

使用对应primitive/module/batch/graph差分和当前DynamicFxChain、FxRoutingPreset、FxCatalogLifetime、FullRender等检查。覆盖切type/order/routing/bypass、参数突变、跨block、tail、latency、坏输入、state-copy及资源析构。

算法声音对比保持相同state/seed/timeline和原阈值。只有实际改到宿主通知/插件行为时，补命名宿主验证；REAPER不外推其他host。交付实际graph接受和tail/latency证据，不能只凭factory能创建就宣布集成完成。详细导读见开发指南第10、15章。
