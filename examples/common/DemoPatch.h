#pragma once
#include <fad/engine/parameters/ParameterSnapshot.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fad::engine::example
{
// Example-specific source-level controls, not FAD Synth parameter IDs or presets.
inline constexpr uint32_t demoSeed = 0x46414401;
inline constexpr std::size_t controlCount = 4;
struct DemoState
{
    // Gain, tone, attack, release, all normalized. No asset or private file names.
    std::array<float, controlCount> values{.5f, .75f, .04f, .3f};
};
inline bool valid(const DemoState& state) noexcept
{
    for (const auto value : state.values)
        if (!std::isfinite(value) || value < 0 || value > 1) return false;
    return true;
}
inline fadmodel1::parameters::ParameterSnapshot makeSnapshot(const DemoState& state)
{
    fadmodel1::parameters::ParameterSnapshot result;
    auto& oscillator = result.oscillators[0];
    oscillator.enabled = true;
    oscillator.mode = fadmodel1::parameters::OscillatorMode::analog;
    oscillator.analogWave = fadmodel1::parameters::AnalogWave::saw;
    oscillator.level = .5f * state.values[0];
    oscillator.unisonVoices = 1;
    oscillator.unisonRand = 0;
    oscillator.drift = 0;
    oscillator.phaseDegrees = 0;
    result.oscillators[1].enabled = false;
    result.subEnabled = false;
    result.noise.enabled = false;
    result.ampEnvelope = {.001f + .999f * state.values[2] * state.values[2],
                          .06f, .75f, .01f + 2 * state.values[3] * state.values[3]};
    result.filter.enabled = true;
    result.filter.cutoffHz = 80 * std::pow(200.0f, state.values[1]);
    result.filter.resonance = .1f;
    // This example has no FX controls. It still uses the real core graph.
    result.dynamicFx.hasExplicitTopology = true;
    result.dynamicFx.slotCount = 0;
    return result;
}
}
