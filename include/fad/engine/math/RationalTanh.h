#pragma once
#include <array>
#include <cstddef>

namespace fad::engine::math
{
// [3/3] Padé for tanh(x)/x in z=x². Coefficients are generated independently
// from y'=1-y² with exact fractions (private derivation receipt). Horner stages
// retain the existing arithmetic order. This is an approximation, not std::tanh
// and not an added clamp; callers retain their original input/safety domains.
template<class Float> inline Float rationalTanh(Float value) noexcept
{
    constexpr std::array<int, 4> numerator{135135, 17325, 378, 1};
    constexpr std::array<int, 4> denominator{135135, 62370, 3150, 28};
    const Float squared = value * value;
    Float p = static_cast<Float>(numerator[3]), q = static_cast<Float>(denominator[3]);
    for (int i = 2; i >= 0; --i)
    {
        p = static_cast<Float>(numerator[static_cast<std::size_t>(i)]) + squared * p;
        q = static_cast<Float>(denominator[static_cast<std::size_t>(i)]) + squared * q;
    }
    return (value * p) / q;
}
}
