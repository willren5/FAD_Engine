#pragma once
#include <algorithm>
#include <cmath>

namespace fadmodel1::dsp::filters
{
// Native complementary LP/HP probes isolate this input stage from the filter.
// The input reaches its flat region at 1.5 * ceiling, with zero slope there.
inline float serumFilterDrive(float input, float ceiling) noexcept
{
    if (!std::isfinite(input)) return 0.0f;
    const auto limit = 1.5f * ceiling;
    const auto x = std::clamp(input, -limit, limit);
    return x - x * x * x / (3.0f * limit * limit);
}
}
