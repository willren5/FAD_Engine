#pragma once

#include <cmath>

#include <array>
#include <algorithm>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/Numeric.h>

#include <fad/engine/parameters/ParameterTypes.h>

namespace fadmodel1::dsp::oscillators
{
inline float wrapPhase01(float phase) noexcept
{
    auto wrapped = phase - std::floor(phase);

    if (wrapped < 0.0f)
        wrapped += 1.0f;

    return wrapped;
}

inline float applyWarpToPhase(float localPhase,
                              float warp,
                              parameters::WarpMode mode) noexcept
{
    constexpr auto twoPi = (static_cast<float>(2) * std::numbers::pi_v<float>);

    switch (mode)
    {
        case parameters::WarpMode::phaseDist:
            return wrapPhase01(localPhase + std::sin(twoPi * localPhase) * 0.2f * warp);

        case parameters::WarpMode::sync:
        {
            const auto ratio = 1.0f + warp * 3.0f;
            return wrapPhase01(localPhase * ratio);
        }

        case parameters::WarpMode::bendPlus:
        {
            const auto exponent = 1.0f + warp * 3.0f;
            return std::pow(localPhase, exponent);
        }

        case parameters::WarpMode::bendMinus:
        {
            const auto exponent = 1.0f / (1.0f + warp * 3.0f);
            return std::pow(localPhase, exponent);
        }

        case parameters::WarpMode::fm:
            return wrapPhase01(localPhase + std::sin(twoPi * localPhase) * warp * 2.0f);

        case parameters::WarpMode::asym:
        {
            const auto pivot = 0.5f - warp * 0.3f;
            if (localPhase < pivot)
                return localPhase / pivot * 0.5f;

            return 0.5f + (localPhase - pivot) / (1.0f - pivot) * 0.5f;
        }
    }

    return localPhase;
}
} // namespace fadmodel1::dsp::oscillators
