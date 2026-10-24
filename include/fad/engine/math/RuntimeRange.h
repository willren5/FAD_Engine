#pragma once
#include <algorithm>
#include <cmath>

namespace fad::engine::math
{
// The engine's continuous/stepped power-law parameter map. All ranges used by
// the current DSP have finite, increasing endpoints and a positive exponent.
// u=((x-start)/(end-start))^exponent; inverse uses exp(log(u)/exponent).
// Discrete controls round half an interval toward the higher step, then clamp.
// No callback, string, host object, or dynamic remapping function is retained.
struct RuntimeRange
{
    float start = 0, end = 1, step = 0, exponent = 1;
    float normalise(float plain) const noexcept
    {
        const float unit = std::clamp((plain - start) / (end - start), 0.0f, 1.0f);
        return exponent == 1.0f ? unit : std::pow(unit, exponent);
    }
    float denormalise(float unit) const noexcept
    {
        float shaped = std::clamp(unit, 0.0f, 1.0f);
        if (exponent != 1.0f && shaped > 0.0f) shaped = std::exp(std::log(shaped) / exponent);
        return start + (end - start) * shaped;
    }
    float snap(float plain) const noexcept
    {
        float result = plain;
        if (step > 0.0f) result = start + step * std::floor((plain - start) / step + 0.5f);
        return std::clamp(result, start, end);
    }
};
}
