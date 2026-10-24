#pragma once
#include <algorithm>
#include <cassert>
#include <cfenv>
#include <cmath>
#include <limits>
#include <type_traits>

namespace fad::engine::math
{
// Preserve the established conversion results. Normal nearest mode is
// ties-to-even. The legacy positive-offset conversion behaves as floor for
// negative fractions in FE_TOWARDZERO; direct lrint would truncate them.
// DSP callers bound finite inputs to the integer domain before conversion.
template<class Float> inline int roundToNearestInt(Float value) noexcept
{
    static_assert(std::is_floating_point_v<Float>);
    assert(std::isfinite(value) && static_cast<double>(value) >= std::numeric_limits<int>::min()
        && static_cast<double>(value) <= std::numeric_limits<int>::max());
    if (value < Float{} && std::fegetround() == FE_TOWARDZERO)
        return static_cast<int>(std::floor(value));
    return static_cast<int>(std::lrint(value));
}

template<class Float> constexpr Float interpolate(Float fraction, Float first, Float last) noexcept
{ return first + fraction * (last - first); }
template<class Float> constexpr Float mapInterval(Float value, Float sourceFirst, Float sourceLast,
                                                 Float targetFirst, Float targetLast) noexcept
{ return targetFirst + ((targetLast - targetFirst) * (value - sourceFirst)) / (sourceLast - sourceFirst); }
}
