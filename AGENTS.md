# FAD Engine development agreements

Use the selected checkout and its current source/build metadata. Keep edits
focused and preserve unrelated dirty work. Core lives in `include/fad/engine/`
and `src/`; there is no private product or account dependency here.

- First build/consumer: `fad-engine-start`; algorithms: `fad-engine-dsp`;
  parameters/state: `fad-engine-state`; resources: `fad-engine-assets`;
  effects/graph: `fad-engine-fx`; independent plugin: `fad-engine-vst3`;
  evidence: `fad-engine-verify`.
- Canonical skills are `.agents/skills/`. Claude entrypoints forward to those
  files. Detailed explanations are in `docs/`.
- Preserve one bank, one voice owner, complete snapshot/asset/graph publication,
  parameter semantics, latency/tails and repeatability under declared inputs.
- Audio callbacks stay bounded and allocation/wait/I/O/log/UI-free. Prepare and
  reclaim on safe threads; avoid complex final-owner destruction on audio.
- Run relevant existing checks. Retain old FAIL/NOT_RUN; do not weaken thresholds,
  normalize audio to hide changes or replace requested performance with correctness-only.
- Agent builds run only on explicitly authorized native local development
  machines. Do not move source/config to remote, cloud or production build
  hosts as a fallback. No compilation CI is configured here.
- API/SDK changes require current official docs and actual pinned versions.
  Preserve deployment floors and notices.
- Independent DSP tasks do not require account, production or installer work.
  Uploads, publishing, signing and installation need corresponding task authority.
- Clean owned unneeded builds/renders after use. Preserve source, user data,
  necessary deliverables, concise evidence and other tasks' artifacts.


---

<img src="assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST® Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
