#pragma once

#include <array>
#include <algorithm>
#include <cmath>

namespace fadmodel1::dsp::oscillators
{
// Measured from independent native sine renders: 11 Blend positions at 3/4/7/16
// voices, held out Blend=.37 at every count, and separate width/pan sweeps.
// Compatibility measurements are described in docs/PROVENANCE.md.
// Detune, oscillator phase and global pan are separate from these mix weights.
inline constexpr float serumUnisonWidthCurvature = 0.4139263175f;
inline constexpr float serumUnisonCentrePan = 0.5f - serumUnisonWidthCurvature * 0.25f;

inline float serumModulatedDetuneCents(float baseCents, float offset) noexcept
{
    baseCents = std::isfinite(baseCents) ? std::clamp(baseCents, 0.0f, 400.0f) : 0.0f;
    if (!std::isfinite(offset) || offset == 0.0f) return baseCents;
    const float basePosition = std::pow(baseCents / 400.0f, 0.4f);
    const float position = std::clamp(basePosition + offset, 0.0f, 1.0f);
    return 400.0f * std::pow(position, 2.5f);
}

inline bool serumUnisonCentreVoice(int index, int count) noexcept
{
    return index == count / 2 || (count % 2 == 0 && index == count / 2 - 1);
}

inline float serumUnisonBlendGain(int index, int count, float blend) noexcept
{
    if (count == 2)
        return 0.8073235115f; // The two-voice case ignores Blend.
    const float squared = blend * blend;
    const float countGain = std::pow(10.0f, -0.25f * static_cast<float>(count - 3) / 20.0f);
    const float gain = serumUnisonCentreVoice(index, count)
        ? (0.43840567f - 0.24325067f * squared) / serumUnisonCentrePan
        : 0.04916505f + squared * (1.42280797f - 0.81731961f * squared);
    return gain * countGain;
}

inline std::array<float, 2> serumUnisonStereoGains(int index, int count, float width) noexcept
{
    const bool oddCentre = count % 2 != 0 && index == count / 2;
    // Alternation skips the centred voice in odd stacks.
    const int sideIndex = index - (count % 2 != 0 && index > count / 2 ? 1 : 0);
    const float pan = oddCentre ? 0.0f : (sideIndex % 2 == 0 ? width : -width);
    const float t = (pan + 1.0f) * 0.5f;
    const float arc = serumUnisonWidthCurvature * t * (1.0f - t);
    return {1.0f - t - arc, t - arc};
}
}
