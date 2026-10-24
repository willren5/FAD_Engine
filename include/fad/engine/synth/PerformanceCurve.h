#pragma once

#include <algorithm>
#include <cmath>

namespace fadmodel1::dsp::synth
{
// The midpoint is deliberately an exact identity for existing presets.
inline float shapeVelocity(float velocity, float curve) noexcept
{
    const float safeVelocity = std::isfinite(velocity) ? std::clamp(velocity, 0.0f, 1.0f) : 0.0f;
    const float safeCurve = std::isfinite(curve) ? std::clamp(curve, 0.0f, 1.0f) : 0.5f;
    if (safeCurve == 0.5f || safeVelocity == 0.0f || safeVelocity == 1.0f)
        return safeVelocity;
    return std::pow(safeVelocity, std::exp2(2.0f * (safeCurve - 0.5f)));
}
}
