# Quick start

## Build the standalone Core

Use a C++20 compiler, CMake 3.24+ and native thread support. Build locally with
your installed generator; no dependency download is part of the default build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --no-tests=error
./build/FadEngineMinimal
```

With the default top-level options, the six Core checks and the minimal-example
check are registered. `ctest -N` lists the actual configured tests. A subproject
can disable tests, renderer and the minimal example independently.

On the locally tested macOS arm64 configuration, retain the existing deployment
floor explicitly:

```sh
cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build build-macos --parallel 4
ctest --test-dir build-macos --output-on-failure --no-tests=error
```

A successful build targeting macOS13 does not establish testing on a macOS13
device. Other platforms require their own native build/run evidence. With a
multi-configuration generator, add `--config Release` to the build, `-C Release`
to CTest, and use that generator's executable layout.

## Render a WAV

```sh
./build/FadEngineRender --help
./build/FadEngineRender --output first-sound.wav --sample-rate 48000 \
  --block-size 512 --seconds 3 --note 60 --velocity 100 \
  --seed 1178682369 --gain 0.5 --tone 0.75 --attack 0.04 --release 0.3
```

The output is stereo IEEE float32 WAV. Files are created exclusively: change
the output name for a new render. No writer normalization or extra clipping is
added. Note-on is at frame0 and note-off at two thirds of the requested duration.
The four controls are example-specific normalized values; read the current
`--help` for accepted ranges.

The [minimal C++ consumer](../examples/minimal/minimal_render.cpp) uses the real
bank, graph and engine and prints finite/non-silent audio metrics without file
I/O or an audio device. Use it to understand lifecycle and sample offsets.

## Embed with CMake

```cmake
set(FAD_ENGINE_BUILD_TESTS OFF CACHE BOOL "No embedded tests")
set(FAD_ENGINE_BUILD_RENDERER OFF CACHE BOOL "No embedded renderer")
set(FAD_ENGINE_BUILD_MINIMAL_EXAMPLE OFF CACHE BOOL "No embedded example")
set(FAD_ENGINE_BUILD_VST3_DEMO OFF CACHE BOOL "No SDK requirement")
add_subdirectory(path/to/FAD_Engine fad-engine)
target_link_libraries(MyInstrument PRIVATE FAD::EngineCore)
```

Or install to a task-local prefix and use the generated config package:

```sh
cmake --install build --prefix "$PWD/stage"
```

```cmake
find_package(FadEngine 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(MyInstrument PRIVATE FAD::EngineCore)
```

Configure the consumer with `CMAKE_PREFIX_PATH` set to that prefix. The package
uses exact-version compatibility and includes the notices/license. This is a
source-level API, not a stable binary ABI.

## Optional VST® 3

Acquire the unmodified official SDK and required submodules at the exact revisions
in [SDK-LOCK.json](../examples/vst3/SDK-LOCK.json). The example refuses mismatched
tracked revisions/content; it does not download the SDK for you.

```sh
cmake -S . -B build-vst3 -DCMAKE_BUILD_TYPE=Release \
  -DFAD_ENGINE_BUILD_VST3_DEMO=ON \
  -DFAD_VST3_SDK_ROOT=/absolute/path/to/vst3sdk
cmake --build build-vst3 --target FadEngineDemo --parallel 4
```

On macOS, also build `FadEngineDemoHost` and run it against the actual generated
VST3 bundle. With the tested Ninja layout:

```sh
cmake --build build-vst3 --target FadEngineDemoHost --parallel 4
./build-vst3/bin/Release/FadEngineDemoHost \
  './build-vst3/VST3/Release/FAD Engine Community Demo.vst3'
```

This does not install, sign or publicly release a plugin. The console host is a
separate proof layer from a named DAW and hardware callback. See
[examples/README.md](../examples/README.md) and [validation](VALIDATION.md).


---

<img src="../assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
