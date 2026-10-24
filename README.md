# FAD Engine

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](#development-and-integration)
[![CMake 3.24+](https://img.shields.io/badge/CMake-3.24%2B-064F8C?logo=cmake&logoColor=white)](#build)
[![openFAD 0.1](https://img.shields.io/badge/license-openFAD%200.1-555555)](LICENSE)

[English](README.md) · [简体中文](README.zh-CN.md)

FAD Engine is the C++20 synthesis engine of FAD Synth. It renders parameter
snapshots, prepared sound assets and MIDI events into audio.

## Features

| Area | Capabilities |
| --- | --- |
| Synthesis | Analog, wavetable and FM oscillators; sub oscillator; sample-based noise; unison, warp and cross-modulation. |
| Filtering and modulation | Filter models, envelopes, eight LFOs, a 32-slot modulation matrix, macros and performance controls. |
| Voices and MIDI | 16-voice allocation, mono/poly, glide, legato, pedals, MPE and MIDI1/UMP channel-voice events. |
| Effects | Drive, Distortion, Bitcrusher, Chorus, Flanger, Phaser, Delay, Reverb, EQ, Compressor, Limiter, Filter and Stereo; graph routing, latency and tails. |
| Assets | Wavetable and noise data, mipmaps, curves and history; generation-based audio access, reader pinning and background retirement. |
| Rendering | Seed-controlled offline rendering, sample-offset event processing and bit-exact regression comparisons. |

## Development and integration

- JUCE-independent Core using C++20, CMake and native thread support.
- Source-level access to synthesis, modulation, filters, voices and effect processors.
- Measurement-based compatibility mappings for Serum-style synthesis behavior.
- Snapshot and asset interfaces for downstream preset conversion and sound-library integration.
- Prepared buffers, bounded event queues and atomic resource handoff in the audio path.
- Commercial integration, source customization and Chinese-language communication through FAD Records.

Applications include audio plugins, standalone instruments, AI music and
sound-generation tools, batch render workers and automated audio regression.
The source and CMake project also provide the starting point for native-platform
porting projects such as ARM64 Linux.

## Build

The project uses a C++20 compiler, CMake3.24+ and a thread library.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --no-tests=error
```

## Render a WAV

```sh
./build/FadEngineRender --output first-sound.wav --sample-rate 48000 \
  --block-size 512 --seconds 3 --note 60 --velocity 100 --seed 1178682369
```

The renderer writes stereo float32 WAV. Its controls are Gain, Tone, Attack and
Release. `FadEngineRender --help` lists the command-line parameters.

## Embed the library

```cmake
add_subdirectory(path/to/FAD_Engine fad-engine)
target_link_libraries(MyInstrument PRIVATE FAD::EngineCore)
```

An installed package uses:

```cmake
find_package(FadEngine 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(MyInstrument PRIVATE FAD::EngineCore)
```

The main interfaces are `RenderEngine`, `VoiceCore`, `ParameterSnapshot`,
`RuntimeWavetableBank`, `DynamicFxChainCore` and `AudioBufferView`.
Preparation initializes voices, buffers and effects. Snapshot publication
updates the sound. Rendering consumes the audio views and timed events supplied
by the host.

## Source map

| Path | Contents |
| --- | --- |
| `include/fad/engine/` | Public types and module interfaces. |
| `src/` | Render engine, voices, oscillators, modulation, filters, assets and effects. |
| `tests/` | Core, runtime primitive, asset publication, voice, render and FX graph checks. |
| `examples/headless/` | WAV renderer. |
| `examples/common/` | Demo controls and snapshot mapping. |
| `examples/vst3/` | VST® 3 instrument example and console host. |

## VST® 3 example

<img src="assets/steinberg/vst-compatible.png" width="160" alt="Steinberg VST Compatible logo">

The example uses the official SDK pinned in
[SDK-LOCK.json](examples/vst3/SDK-LOCK.json). It exposes stereo audio,
sample-offset MIDI, four controls and versioned state.

```sh
cmake -S . -B build-vst3 -DCMAKE_BUILD_TYPE=Release \
  -DFAD_ENGINE_BUILD_VST3_DEMO=ON -DFAD_VST3_SDK_ROOT=/path/to/vst3sdk
cmake --build build-vst3 --target FadEngineDemo --parallel 4
```

## Documentation

[Quick start](docs/QUICK_START.md) · [Architecture](docs/ARCHITECTURE.md) ·
[Developer guide](docs/DEVELOPMENT_GUIDE.zh-CN.md) ·
[Training manuscript](docs/DEVELOPER_NARRATION.zh-CN.md) · [Skills](skills/README.md)

## License

[openFAD 0.1](LICENSE) · [Commercial licensing](legal/COMMERCIAL-LICENSE.md)

## Contact

[fad@fadrecords.com](mailto:fad@fadrecords.com)

VST is a registered trademark of Steinberg Media Technologies GmbH.
