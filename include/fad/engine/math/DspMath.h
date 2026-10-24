#pragma once
#include <algorithm>
#include <cmath>

namespace fad::engine::math
{
template<class Float> Float gainFromDecibels(Float decibels, Float floor = static_cast<Float>(-100)) noexcept
{
    return decibels > floor ? std::pow(static_cast<Float>(10), decibels * static_cast<Float>(.05)) : Float{};
}
template<class Float> Float gainToDecibels(Float gain, Float floor = static_cast<Float>(-100)) noexcept
{
    return gain > Float{} ? std::max(floor, std::log10(gain) * static_cast<Float>(20)) : floor;
}

// Preserve the existing platform-specific software state cleanup. Intel uses
// the original small-state floor (including NaN quarantine); ARM relies on the
// enclosing hardware denormal scope and the local finite-state guards.
template<class Float> void snapSmallState(Float& value) noexcept
{
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    constexpr Float floor = static_cast<Float>(1.0e-8f);
    if (std::isnan(value) || std::abs(value) <= floor) value = Float{};
#else
    (void) value;
#endif
}
}
