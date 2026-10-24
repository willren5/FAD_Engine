#pragma once
#include <algorithm>
#include <numbers>
#include <fad/engine/math/Numeric.h>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/DspSafetyUtils.h>

namespace fadmodel1::dsp::fx
{
// A sample-timeline clock, never a host-block clock. Retired FX graphs retain
// the last incoming value until explicitly reset; no allocation or ownership.
template <int Target>
struct NativeWetModulationClock
{
    void clear() noexcept { incoming = 0.0f; }
    void add(int target, float offset) noexcept
    {
        if (target == Target && std::isfinite(offset))
            incoming = std::clamp(incoming + offset, -32.0f, 32.0f);
    }
    int quantum() const noexcept { return remaining > 0 ? remaining : 16; }
    float next() noexcept
    {
        if (remaining <= 0) { applied = incoming; remaining = 16; }
        --remaining;
        return applied;
    }
    void reset() noexcept { incoming = applied = 0.0f; remaining = 0; }
    float incoming = 0.0f, applied = 0.0f;
    int remaining = 0;
};

// Flanger and Phaser reach w=.5 at maximum Mix, unlike Chorus (w=1).
// Independent native wet sweeps establish output = dry*(1-w*w) + wet*w.
inline float serumModulationFxWetGain(float position) noexcept
{
    return .5f - .5f * std::cos((std::numbers::pi_v<float> / 2)
                                * utils::sanitizeUnitInterval(position));
}
}
