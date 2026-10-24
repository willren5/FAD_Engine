#pragma once

#include "Numeric.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace fadmodel1::dsp::utils
{
inline bool isFinite(float value) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559);
    return (std::bit_cast<uint32_t>(value) & 0x7f800000u) != 0x7f800000u;
#else
    return std::isfinite(value);
#endif
}

inline float sanitizeFinite(float value, float fallback = 0.0f) noexcept
{
    return isFinite(value) ? value : fallback;
}

inline uint32_t sanitizeBlockSize(int64_t value) noexcept
{
    constexpr uint32_t maxSafeBlockSize = 1u << 20;
    if (value <= 0)
        return 1u;

    if (value > static_cast<int64_t>(maxSafeBlockSize))
        return maxSafeBlockSize;

    return static_cast<uint32_t>(value);
}

inline int roundToIntFinite(float value, int fallback = 0) noexcept
{
    return isFinite(value) ? fad::engine::math::roundToNearestInt(value) : fallback;
}

inline float sanitizeFiniteClamped(float value,
                                   float fallback,
                                   float minimum,
                                   float maximum) noexcept
{
    return isFinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

inline double sanitizeSampleRate(double sampleRate,
                                 double fallback = 44100.0,
                                 double minimum = 8000.0,
                                 double maximum = 384000.0) noexcept
{
    if (! std::isfinite(sampleRate))
        return fallback;

    return std::clamp(sampleRate, minimum, maximum);
}

inline float sanitizeUnitInterval(float value, float fallback = 0.0f) noexcept
{
    return sanitizeFiniteClamped(value, fallback, 0.0f, 1.0f);
}

inline int roundToIntSafe(float value,
                          int fallback = 0,
                          int minimum = std::numeric_limits<int>::min(),
                          int maximum = std::numeric_limits<int>::max()) noexcept
{
    if (minimum > maximum)
        std::swap(minimum, maximum);

    if (! std::isfinite(value))
        return std::clamp(fallback, minimum, maximum);

    const auto clamped = std::clamp(static_cast<double>(value), static_cast<double>(minimum), static_cast<double>(maximum));
    const auto rounded = std::llround(clamped);
    return std::clamp(static_cast<int>(rounded), minimum, maximum);
}

inline int roundToIntSafe(double value,
                          int fallback = 0,
                          int minimum = std::numeric_limits<int>::min(),
                          int maximum = std::numeric_limits<int>::max()) noexcept
{
    if (minimum > maximum)
        std::swap(minimum, maximum);

    if (! std::isfinite(value))
        return std::clamp(fallback, minimum, maximum);

    const auto clamped = std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum));
    const auto rounded = std::llround(clamped);
    return std::clamp(static_cast<int>(rounded), minimum, maximum);
}
}
