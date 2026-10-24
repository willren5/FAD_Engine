# FAD Engine

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](#开发与接入)
[![CMake 3.24+](https://img.shields.io/badge/CMake-3.24%2B-064F8C?logo=cmake&logoColor=white)](#构建)
[![openFAD 0.1](https://img.shields.io/badge/license-openFAD%200.1-555555)](LICENSE)

[简体中文](README.md) · [English](README.en.md)

FAD Engine 是 FAD Synth 的 C++20 合成器内核，将参数快照、已准备的声音资产和
MIDI事件渲染为音频。

## 功能

| 模块 | 内容 |
| --- | --- |
| 合成 | 模拟、波表和FM振荡器，Sub、样本Noise、Unison、Warp及交叉调制。 |
| 滤波与调制 | 滤波模型、包络、8个LFO、32槽调制矩阵、Macro与演奏控制。 |
| 声部与MIDI | 16声部分配、Mono/Poly、Glide、Legato、踏板、MPE及MIDI1/UMP channel-voice事件。 |
| 效果 | Drive、Distortion、Bitcrusher、Chorus、Flanger、Phaser、Delay、Reverb、EQ、Compressor、Limiter、Filter和Stereo，配合Graph路由、Latency和Tail。 |
| 资产 | 波表、Noise、Mipmap、曲线与History；按generation读取、reader pin和后台退役。 |
| 渲染 | Seed控制的离线渲染、Sample Offset事件处理和Bit-exact回归比较。 |

## 开发与接入

- JUCE独立的Core，使用C++20、CMake和原生线程支持。
- 源码可编辑，覆盖合成、调制、滤波、声部和效果器。
- 来自测量与兼容工作中的Serum类合成行为映射。
- Snapshot与Asset接口用于接入预设转换和音色库。
- 音频路径采用预分配Buffer、固定容量事件队列和原子资源交接。
- FAD Records提供商业接入、源码定制和中文交流渠道。

应用场景包括音频插件、独立乐器、AI音乐与声音生成、批量渲染服务和自动化音频回归。
C++20源码与CMake工程也可用于ARM64 Linux等原生平台的移植项目。

## 构建

项目使用C++20编译器、CMake3.24+和线程库。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --no-tests=error
```

## 渲染WAV

```sh
./build/FadEngineRender --output first-sound.wav --sample-rate 48000 \
  --block-size 512 --seconds 3 --note 60 --velocity 100 --seed 1178682369
```

Renderer输出双声道float32 WAV，控制项为Gain、Tone、Attack和Release。
命令行参数见 `FadEngineRender --help`。

## 接入项目

```cmake
add_subdirectory(path/to/FAD_Engine fad-engine)
target_link_libraries(MyInstrument PRIVATE FAD::EngineCore)
```

安装后的Package通过以下方式接入：

```cmake
find_package(FadEngine 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(MyInstrument PRIVATE FAD::EngineCore)
```

主要接口为 `RenderEngine`、`VoiceCore`、`ParameterSnapshot`、
`RuntimeWavetableBank`、`DynamicFxChainCore` 和 `AudioBufferView`。
Prepare准备声部、Buffer和效果器，Snapshot发布更新声音，Render处理宿主提供的
音频视图和时间线事件。

## 源码结构

| 目录 | 内容 |
| --- | --- |
| `include/fad/engine/` | 公共类型与模块接口。 |
| `src/` | 渲染、声部、振荡器、调制、滤波、资产和效果实现。 |
| `tests/` | Core、Primitive、资产发布、Voice、Render和FX Graph检查。 |
| `examples/headless/` | WAV Renderer。 |
| `examples/common/` | Demo控制与Snapshot映射。 |
| `examples/vst3/` | VST® 3乐器示例及Console Host。 |

## VST® 3示例

<img src="assets/steinberg/vst-compatible.png" width="160" alt="Steinberg VST Compatible logo">

示例使用 [SDK-LOCK.json](examples/vst3/SDK-LOCK.json) 指定的官方SDK，
提供Stereo Audio、Sample Offset MIDI、四个控制和版本化State。

```sh
cmake -S . -B build-vst3 -DCMAKE_BUILD_TYPE=Release \
  -DFAD_ENGINE_BUILD_VST3_DEMO=ON -DFAD_VST3_SDK_ROOT=/path/to/vst3sdk
cmake --build build-vst3 --target FadEngineDemo --parallel 4
```

## 文档

[快速开始](docs/QUICK_START.md) · [架构](docs/ARCHITECTURE.md) ·
[开发指南](docs/DEVELOPMENT_GUIDE.zh-CN.md) ·
[培训文稿](docs/DEVELOPER_NARRATION.zh-CN.md) · [开发Skills](skills/README.md)

## 许可

[openFAD 0.1](LICENSE) · [商业授权](legal/COMMERCIAL-LICENSE.md)

## 联系

[fad@fadrecords.com](mailto:fad@fadrecords.com)

VST is a registered trademark of Steinberg Media Technologies GmbH.
