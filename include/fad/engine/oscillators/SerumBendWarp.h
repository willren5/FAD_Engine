#pragma once

#include <algorithm>
#include <cmath>

namespace fadmodel1::dsp::oscillators
{
inline bool isSerumBendMode(int mode) noexcept { return mode >= 4 && mode <= 6; }

// Independently measured ramp and held-out sine probes establish a symmetric
// rational phase map. This is not FAD's power warp or an amplitude saturator.
// Native Bend +/- is strongest Bend+ at zero, neutral at .5, Bend- at one.
inline float serumBendPhase(float phase, float position, int mode) noexcept
{
    const auto p = std::clamp(std::isfinite(phase) ? phase : 0.0f, 0.0f, 1.0f);
    if (!isSerumBendMode(mode)) return p;
    const auto raw = std::clamp(std::isfinite(position) ? position : .5f, 0.0f, 1.0f);
    const auto amount = mode == 4 ? raw : mode == 5 ? -raw : 1.0f - 2.0f * raw;
    const auto ratio = (1.0f + .8f * amount) / (1.0f - .8f * amount);
    const auto x = p < .5f ? 2.0f * p : 2.0f * (1.0f - p);
    // ratio is in [1/9,9]; the denominator stays strictly positive.
    const auto y = x / (ratio + (1.0f - ratio) * x);
    return std::clamp(p < .5f ? .5f * y : 1.0f - .5f * y, 0.0f, 1.0f);
}
}
