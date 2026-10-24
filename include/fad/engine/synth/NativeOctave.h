#pragma once

#include <fad/engine/math/DspSafetyUtils.h>
#include <algorithm>
#include <cmath>

namespace fadmodel1::dsp::synth
{
// Add in the original knob domain, then quantize. .3 + .015625 crosses an
// octave boundary; reconstructing .3 from its displayed octave (-2) does not.
inline int nativeOctaveForPosition(float position) noexcept
{
    return static_cast<int>(std::floor(8.0f * utils::sanitizeUnitInterval(position, .5f) + .5f)) - 4;
}

inline float nativeOctavePitchOffset(int octave, float savedPosition, float offset) noexcept
{
    if (!std::isfinite(offset) || offset == 0.0f) return 0.0f;
    const auto safeOctave = std::clamp(octave, -4, 4);
    // A changed ordinary octave takes precedence. Old states use -1/unset.
    const auto base = std::isfinite(savedPosition) && savedPosition >= 0.0f
        && savedPosition <= 1.0f && nativeOctaveForPosition(savedPosition) == safeOctave
        ? savedPosition : static_cast<float>(safeOctave + 4) * .125f;
    return 12.0f * static_cast<float>(nativeOctaveForPosition(base + offset) - safeOctave);
}
}
