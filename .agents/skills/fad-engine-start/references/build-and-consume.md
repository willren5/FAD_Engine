# Build and consume

Use the selected Core source and a task-owned native local build. Read current options, toolchain and deployment settings first; do not reconfigure an active peer build.

```sh
cmake -S "$FAD_CORE" -B "$FAD_BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DFAD_ENGINE_BUILD_TESTS=ON -DFAD_ENGINE_BUILD_RENDERER=ON \
  -DFAD_ENGINE_BUILD_MINIMAL_EXAMPLE=ON -DFAD_ENGINE_BUILD_VST3_DEMO=OFF
cmake --build "$FAD_BUILD" --parallel 4
ctest --test-dir "$FAD_BUILD" -N
ctest --test-dir "$FAD_BUILD" --output-on-failure --no-tests=error
```

Multi-config generators need explicit configuration and executable layout. Existing downstream wrappers can provide locks/receipts, but Core has no private workflow dependency. Run renderer --help before rendering a new output. Check rate/frames/seed/finite/non-silent; demo controls are not a full product contract.

See examples/minimal/minimal_render.cpp and docs/QUICK_START.md for embedding/staging. Link FAD::EngineCore; rebuild consumers after source/header/compiler changes. Optional SDK work belongs to fad-engine-vst3.


---

<img src="../../../../assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST® Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
