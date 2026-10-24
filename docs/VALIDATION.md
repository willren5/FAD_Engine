# Validation scope

## Current source-package checks

Locally checked on macOS arm64 with AppleClang21, Xcode27.1, macOS SDK27.0,
CMake4.2.3 and deployment target13.0. This is not a test on a macOS13 device.
All builds were native and local; no cloud build, global installation, product
account operation or signing was used.

| Check | Actual result |
| --- | --- |
| Standalone Core and examples | Release build from this repository passed. |
| Core contract checks | All six registered Core tests passed; staged-initial-FX regression is included. |
| Minimal consumer | Passed, 48000frames/48kHz/stereo, finite and non-silent. |
| Source/runtime isolation | Compilation and all seven checks passed with private product/Core/JUCE sources and network denied. |
| WAV renderer | Passed at48kHz/block17,144000frames/stereo; peak0.0635286, RMS0.0167271, finite output. |
| Staging/install consumer | Task-local install, exact-version find_package and a fresh real consumer passed; not a system/plugin installation. |
| Optional VST® 3 | Pinned SDK build and actual console module/state/audio checks passed with private sources and network denied. |
| Plugin legal resources | Current Engine LICENSE, NOTICE and separate SDK license were verified in the bundle. |

The console host observed 3654callbacks,96000bit-exact recalled float values,
500concurrent restore cycles and500coherent captures. It exercised MIDI overflow,
automation overflow preservation and zero-sample flush. This run reported
`RT_probe=false`; its zero audit counters do not establish an allocation,
blocking or I/O audit. No named DAW was started for this packaging pass.

The current post-fix module SHA-256 is
`7e84ebff497f1c7561583e67d531268dbfb26a479c23760ed7d71d08a463f4ec`.
It was rebuilt and console-tested after the staged-initial-FX fix. The earlier
REAPER result belonged to a different pre-fix module and is historical evidence;
it does not establish a named-DAW run of this new module.

## Staged offline initial FX repair

Before the repair, stageIsolatedOfflineInitialSnapshot followed by prepare left
the effect graph at its reset/default state: staged reverb and dry output were
bit-identical. A regression was run against the old source and failed first.

An explicit lifecycle flag now causes prepare to apply only an explicitly staged
initial snapshot through the existing control-side publication after reset. The
ordinary, unstaged prepare path is retained. The regression checks wet vs dry,
bit-exact parity with prepare-then-publish for both, and initial limiter latency.
The fix was applied to the public preparation and the private extraction Core.

Current normal, ASan/UBSan and TSan runs each passed all seven configured checks.
The private extraction Core separately passed its six normal checks. README C++
blocks were compiled with Wall/Wextra/Wpedantic/Werror and exercised through
add_subdirectory and exact-version find_package, including the wet variant.
The renderer's printed quick-start line matched exactly. A fresh Xcode Release/arm64 ALL_BUILD and all seven checks also passed, with
explicit signing disabled. Current compiler/runtime sandbox checks were rerun
after the repair, denying private source/JUCE/Main reads and network.

## Historical extraction measurements

The earlier extraction compared a frozen640-case sound matrix:580cases were
bit-exact and all cases passed the original arithmetic tolerance; max_abs was
about9.78e-9. A40-group FX graph matrix was bit-exact. The private original-product
oracles and raw files are not distributed here; these are not advertised as
publicly reproducible tests in this repository.

Five paired A/B rounds of the full Noise80 matrix retained strict deadline
**FAIL for both baseline and candidate**. The candidate's median total thread CPU
changed by+0.42%, cumulative peakRSS by+7.99%. RSS review did not isolate the exact
high-water variability cause; the review flag remains. These measurements belong
to their original artifacts and environment and are not a new benchmark of this
source-package build. Correctness-only and sanitizers do not replace normal
performance measurements.

## Limits

No hardware callback certification, universal host compatibility or all-platform
support claim is made. Windows/iOS/macOS13device runtime, full Max/Live module,
other actual AU/AUv3 hosts and commercial account lifecycle were not run for this
package. Coredevelopment uses independent consumers and does not require those
unrelated product flows.

License scope, third-party rights and contribution authority require their own
review. A successful build or test does not supply a missing copyright grant.
See [provenance](PROVENANCE.md), [licensing](LICENSING.zh-CN.md) and the accepted
CLA/grant records before commercial adoption.


---

<img src="../assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>

## Documentation and license naming update

The introductory READMEs and the main license name were updated to openFAD0.1
after the runtime checks recorded above. This update changes documentation,
license naming and source notice comments; executable DSP behavior is unchanged.
