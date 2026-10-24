# Select DSP checks

Read actual CMake registrations and test bodies. CoreContract/RuntimePrimitive cover foundations, AssetPublication resource handoff, VoiceCore per-voice behavior, RenderEngine dispatch/time/capacity, and FullRender the real graph. FadEngineMinimal is a consumer check, not broadFXcoverage.

Use current source/build/binary identity and matched state/assets/seed/reset/rate/block/MIDI/automation. Preserve unnormalized audio. A/A repeatability does not prove old-version parity; use a provided frozenoracle if required, and keep the originaltolerance.

Time/state changes especially need irregular smallblocks, note-on/off, reset/restore, extrema and recovery. Select by actualchange; text-only work does not need a full performance run. Debug/sanitizers diagnose safety, not optimizedtiming. Unavailable downstream harnesses are evidence gaps, not inventedPASS.
