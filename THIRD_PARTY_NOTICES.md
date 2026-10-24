# Third-party notices

## Optional Steinberg VST® 3 SDK

The optional VST3 example uses an independently obtained, pinned official
[Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk). Exact root and
submodule revisions and the license hash are in [SDK-LOCK.json](examples/vst3/SDK-LOCK.json).
The SDK is not vendored here or downloaded by the default Core build.

The pinned SDK has its own MIT license. Obtain and preserve the complete license
from that checkout. Example packaging copies that license, the Engine notice
and the Engine license into the plugin bundle. These licenses remain separate.

## Toolchain and system libraries

C++ standard-library/thread support, compilers, SDKs, system libraries and
floating-point-control intrinsics remain governed by their respective terms.
Those dependencies are obtained separately, not supplied or relicensed here.

## Technical references

JUCE was used in private comparison/design tooling during extraction. The Core
does not include or link JUCE. Numerical-data and compatibility origins are
recorded in [PROVENANCE.md](docs/PROVENANCE.md); technical comparison does not
by itself establish redistribution rights.

Names describing compatibility with other instruments do not imply vendor
authorship, affiliation, endorsement or rights to vendor code/presets/samples.
Those product assets are not bundled in this repository.


---

<img src="assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>

## Official trademark artwork

The unmodified official VST Compatible logo is included only for permitted
format identification under Steinberg usage guidelines. It is not relicensed
under the openFAD 0.1 or the SDK MIT code license. Origin, exact
hash and usage reference are in [the artwork notice](assets/steinberg/README.md).
