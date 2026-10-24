# Engine development skills

The complete, canonical skills live in `.agents/skills/`. Claude entrypoints
forward to the same files; references and scripts are not duplicated.

| Skill | Use for |
| --- | --- |
| [fad-engine-start](../.agents/skills/fad-engine-start/SKILL.md) | Locate Core, inspect a build, build/run the first sound and embed a consumer. |
| [fad-engine-dsp](../.agents/skills/fad-engine-dsp/SKILL.md) | Oscillator/filter/envelope/LFO/modulation/voice and numerical fixes. |
| [fad-engine-state](../.agents/skills/fad-engine-state/SKILL.md) | Parameters, coherent snapshots, automation, restore and downstream state. |
| [fad-engine-assets](../.agents/skills/fad-engine-assets/SKILL.md) | WT/Noise/history, generation, pin/retire and late resource results. |
| [fad-engine-fx](../.agents/skills/fad-engine-fx/SKILL.md) | FX processor/catalog/factory, routing, graph transitions, latency and tails. |
| [fad-engine-vst3](../.agents/skills/fad-engine-vst3/SKILL.md) | Pinned-SDK plugin adaptation, buses, events and versioned state. |
| [fad-engine-verify](../.agents/skills/fad-engine-verify/SKILL.md) | Select relevant checks, bind source/binary identity and report actual evidence. |

For example: `Use $fad-engine-assets to investigate this rapid Noise-switching
lifetime defect, keeping the existing bank and retirement protocol.` Combine
skills only when the real call path crosses their responsibilities.

## Read-only source/build inspector

```sh
python3 .agents/skills/fad-engine-start/scripts/inspect_engine.py --source .
python3 .agents/skills/fad-engine-start/scripts/inspect_engine.py \
  --source . --build build
```

It emits selected source-marker hashes and allowed CMakeCache metadata. It does
not create directories, build, load plugins, access accounts or prove a binary
was built from the observed source. `build_reuse_verified=false` means active
processes/locks and actual inputs still need checking. Paths in its diagnostic
output describe the caller's chosen local environment; do not upload unredacted
local reports as public source files.

## Maintenance

Change canonical files first; keep Claude forwarding links valid. Use the normal
skill-creator validator where available and run changed helper scripts against
real/isolated inputs. Do not turn text validation into unrelated full DSP or
account/release acceptance. The detailed guide is in [docs](../docs/README.md).


---

<img src="../assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST® Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
