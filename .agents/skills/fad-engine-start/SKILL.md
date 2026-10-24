---
name: fad-engine-start
description: 定位 FAD Engine 独立源码、配置本地 Core 构建、运行首个声音或接入一个最小 C++ 消费者。用于开始 Engine 开发和排查构建入口；已有明确算法问题使用 fad-engine-dsp。
---

# FAD Engine 独立开发起步

完成用户需要的独立构建或首次发声，建立明确的 source、build、binary 对应关系。独立 DSP 不依赖 FAD 账号、商业安装包或产品 UI。

## 定位

以用户指定路径为准；未指定时检查当前工作树。Core 根应同时有 `CMakeLists.txt` 和 `include/fad/engine/synth/RenderEngine.h`；下游完整工程也可能在 `engine/` 子目录满足这些条件。不要自动切到 main、旧导出或另一个日期的副本。

需要确认目录或既有 build 时，运行本 skill 的只读脚本：

```sh
python3 "$SKILL_DIR/scripts/inspect_engine.py" --source "$FAD_SOURCE"
python3 "$SKILL_DIR/scripts/inspect_engine.py" --source "$FAD_SOURCE" --build "$FAD_BUILD"
```

`SKILL_DIR` 是本 SKILL.md 所在目录。脚本只读取路径、限定 CMakeCache 字段和源码 hash；不构建、不创建目录、不核验二进制来源。source、cache 的一致不能单独证明已有 binary 来自当前源码。

## 完成路径

1. 读取选定 Core 的 CMake options，确认 native compiler、generator、目标平台及 deployment floor。复用空闲且兼容的 build；不改他人的 cache/generator。
2. 默认只做 Core、独立检查或 renderer；只有任务需要插件时才启用 SDK。具体命令见 [构建与消费者](references/build-and-consume.md)。
3. 使用当前项目本地CMake/CTest及已有协调约定；下游另有build wrapper时可复用，不要求缺失的私有脚本。
4. 首次发声用现有 renderer 或 `examples/minimal`。检查 finite/non-silent、采样率、帧数、seed 和退出状态；WAV 使用新文件名。
5. 真实算法、资源或宿主问题转入对应范围，不通过关闭产品授权门排查独立声音。

有头文件错误就核对 target/include；有静音就核对实际接受的 snapshot、声源level、MIDI、plane layout 与 bypass。不要因为旧产品 helper 无签名，先要求独立 DSP 开发者准备账户或 Developer ID。

## 交付

给出源码根、构建配置、实际目标/产物和已运行结果。清理自己已完成用途的临时 build/重复 WAV，保留必要交付及精简 receipt。只在授权本机/明确指定的原生开发机编译，不把源码交给远端或生产服务器作为构建fallback。
