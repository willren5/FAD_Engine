# Independent examples

These examples use the same `FAD::EngineCore` source as the private product.
They contain generated oscillator content and four example-specific normalized
controls: Gain, Tone, Attack and Release. No FAD account, UI, factory preset,
commercial sample or private asset path is required. They are not FAD Synth
and do not implement or bypass its commercial authorization.

The Engine and examples use the openFAD 0.1 or a separate Commercial License. See `../LICENSE` and `../legal/`. The separately obtained SDK has its own license.

## Headless WAV renderer

From the engine source directory:

```sh
cmake -S . -B build-examples -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-examples --target FadEngineRender
build-examples/FadEngineRender --output build-examples/new.wav
```

This target has no SDK download or JUCE dependency. The output is stereo
IEEE float32 RIFF/WAV with a `fact` chunk, without normalization or clipping
added by the file writer. Existing files are refused through exclusive creation.
`--help` lists the numeric options. Note-on is at sample zero and note-off at
two thirds of the requested duration. Use a sufficient duration for the selected
release. The default seed is fixed; seed zero is refused.

The renderer streams one block at a time. File I/O and encoding are outside
the engine's render call. The supported CLI ranges are 8–384 kHz, blocks 1–8192,
and duration 0.01–60 seconds; this is not a claim that every host/platform has
been verified across those ranges.

## Optional VST® 3 example

The default build does not read or fetch the SDK. Obtain an independent official
`steinbergmedia/vst3sdk` checkout at `v3.8.1_build_84`, including `base`, `cmake`,
`pluginterfaces` and `public.sdk`. Exact revisions and license hashes are in
`vst3/SDK-LOCK.json`; CMake refuses a different or modified checkout.

```sh
cmake -S . -B build-vst3 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAD_ENGINE_BUILD_VST3_DEMO=ON \
  -DFAD_VST3_SDK_ROOT=../vst3sdk
cmake --build build-vst3 --target FadEngineDemo
```

The demo has separate processor/controller IDs and bundle identity
`com.fadrecords.engine.communitydemo`. It exposes stereo float32 output,
sample-offset MIDI note-on/off and four automatable controls. It uses a fixed
seed and the core's generated oscillator, filter, envelopes, voice and graph.
Its FX graph is empty; all thirteen engine effects remain available through
the core API. There is no custom editor; hosts can expose the parameter list.

Component state is versioned, field-wise little-endian data containing only
the four normalized controls. Invalid or truncated state is rejected before
publication. It is not a dump of `ParameterSnapshot` and is not the private
FAD preset/session format. UI restore and audio automation have separate
single-writer atomic mailboxes; callbacks keep the previous complete state
when a bounded read cannot complete.

The input limits are 4096 MIDI events and 4096 automation points per block.
MIDI overflow follows the core's panic/retirement request. Oversized automation
retains the previous complete control state for that block. Normal automation
uses its sample offsets. Unsupported event types are ignored. These limits and
the four-control surface belong to the demo, not to the private product API.

Builds do not install/link the plugin into a global plugin directory, invoke
account services, or run signing/release commands. The SDK's packaging and
platform entry-point helpers are used directly because its high-level helper
otherwise adds unconditional post-build signing on recent Xcode versions.
The SDK license and the Engine notice are copied into the bundle.
Validation and host tests are separate steps; a successful build is not host
or release acceptance.

On macOS, `FadEngineDemoHost candidate.vst3` loads that module through
the SDK hosting API without a visible window. It verifies MIDI audio,
automation/state readback, truncated and non-finite state rejection, input
capacity handling, zero-sample flush, concurrent UI restore/audio processing,
and bit-exact audio after recall into a fresh instance. It does not substitute
for a named DAW or a hardware callback. An explicitly supplied local
`FAD_DEMO_RT_PROBE_LIBRARY` adds an audit to the host probe only; the plugin
never links that library. The default probe prints `RT_probe=false` and makes
no allocation/blocking/I/O audit claim. With the audit enabled, a deliberate
allocation/free must first be detected before callback counters are accepted.


---

<img src="../assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
