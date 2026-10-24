#pragma once
#include <array>
#include <cstddef>

namespace fad::engine::math
{
// Independently derived [4/4] Padé, matched through Taylor order 8. FAD's
// pitch-ratio path uses only the fractional log2 interval; retain that domain
// and the existing power-of-two reconstruction in VoiceCore.
template<class Float> inline Float rationalExp(Float value) noexcept
{
    constexpr std::array<int, 5> numerator{1680, 840, 180, 20, 1};
    constexpr std::array<int, 5> denominator{1680, -840, 180, -20, 1};
    Float p = static_cast<Float>(numerator[4]), q = static_cast<Float>(denominator[4]);
    for (int i = 3; i >= 0; --i)
    {
        p = static_cast<Float>(numerator[static_cast<std::size_t>(i)]) + value * p;
        q = static_cast<Float>(denominator[static_cast<std::size_t>(i)]) + value * q;
    }
    return p / q;
}
}
