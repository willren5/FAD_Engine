# Changelog

## 0.1.0

- Independent C++20 Core and a single render/voice/resource implementation.
- Native oscillators, modulation, filters, thirteen effects, routing and existing
  safety/lifetime behavior.
- Fixed explicitly staged offline initial FX: prepare now applies the staged graph and latency through the normal control path; wet/dry and prepare-then-publish parity are covered.
- Minimal consumer, float32 WAV renderer and optional pinned-SDK VST® 3 example.
- Developer guide, training manuscript and seven scoped development skills.

Known performance and platform limitations are in [VALIDATION.md](docs/VALIDATION.md).
This initial source package does not claim a binary release or hardware certification.


---

<img src="assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
