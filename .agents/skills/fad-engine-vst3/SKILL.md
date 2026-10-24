---
name: fad-engine-vst3
description: 使用固定官方 SDK 构建或扩展 FAD Engine 独立 VST3 示例，处理bus、sample-offset MIDI/automation、版本化state和真实模块验证。适用于独立插件适配，不代替商业FAD产品发行。
---

# FAD Engine 独立 VST® 3

使用选定Core的 `examples/vst3`，保持独立产品身份和自生成内容。Core默认不需要SDK；用户只要headless/DSP时不启动插件构建。

## 依赖和构建

读取当前 `SDK-LOCK.json`、CMake和实际SDKroot/子模块revision/dirty；不把历史tag写成永远最新，不修改锁来掩盖依赖不符。不存在SDK时先确认当前依赖获取约定，不复制JUCE wrapper或私有产品UI。

使用现有Core CMake `FAD_ENGINE_BUILD_VST3_DEMO` 和SDK root选项。审阅实际generator输出的安装、链接、签名和post-build副作用；保持当前独立候选不自动全局安装/签名。编译走当前环境已有的本地build协调入口。

## 适配合同

1. 验证sample format、bus、planes、block和zero-frame flush；当前支持面读DemoPlugin，新增float64/bus/UI需要明确实现与覆盖。
2. SDK MIDI channel转换只做一次；保留sample offsets和同点稳定顺序。按实际固定容量处理溢出，不能在callback扩容。
3. normalized controls通过DemoPatch转成完整snapshot；audio只作有界读取/接受，失败保留旧状态。一次真实process中的automation chunks共用callback boundary。
4. state采用magic/version与逐字段编码；截断、NaN/Inf和越界在发布前拒绝。加参数时定义旧版本默认值与迁移，不能dump runtime struct。
5. UI restore/audio automation各自保持声明的writer；UI工作不能等价搬进audio。borrowed host指针不跨process存活。

新增/迁移SDK或Apple API使用方式时，核对本次官方文档、固定header和本机SDK；不顺带升级依赖或系统下限。

## 验证与交付

按修改选择SDK validator、当前console host、fresh-instance state/audio、边界/并发及命名DAW。只有audit probe启用且正控制成功才报告分配/阻塞/I/O审计；普通host不证明硬件deadline。

真实宿主使用本次module SHA和当前参数枚举，视觉才用UI，可见窗口留在已核对竖屏。示例不调用FAD账户；产品授权门不改。交付module/SDK/source身份、实际执行层次和未验证平台；构建/签名不代表公共发行批准。


---

<img src="../../../assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
