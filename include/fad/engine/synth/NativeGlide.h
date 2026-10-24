#pragma once

#include <fad/engine/math/DspSafetyUtils.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fadmodel1::dsp::synth
{
// Native pitch-domain portamento. Measurements use isolated sine notes, not
// whole-preset tuning; see docs/PROVENANCE.md.
// Fixed storage and bounded arithmetic only; legacy FAD ramps do not use this.
class NativeGlide final
{
public:
    void reset(float frequency) noexcept
    {
        current = target = utils::sanitizeFiniteClamped(frequency, 440.0f, 1.0f, 24000.0f);
        elapsed = length = 0;
    }

    void start(float from, float to, double sampleRate, float milliseconds,
               float curve, bool scaled) noexcept
    {
        reset(from);
        target = utils::sanitizeFiniteClamped(to, current, 1.0f, 24000.0f);
        const auto seconds = static_cast<double>(utils::sanitizeFiniteClamped(
            milliseconds, 0.0f, 0.0f, 60000.0f)) * .001;
        startPitch = std::log2(static_cast<double>(current));
        pitchDistance = std::log2(static_cast<double>(target)) - startPitch;
        if (seconds <= 0.0 || std::abs(pitchDistance) < 1.e-12)
        {
            reset(target);
            return;
        }
        const auto duration = seconds * (scaled ? std::abs(pitchDistance) : 1.0);
        length = static_cast<int64_t>(std::max(1.0, std::round(
            utils::sanitizeSampleRate(sampleRate) * duration)));
        coefficient = curveCoefficient(curve);
        curveUsesTable = utils::sanitizeUnitInterval(curve, .5f) != .5f
            && coefficient != 0.0;
        denominator = -std::expm1(-std::abs(coefficient));
    }

    bool isSmoothing() const noexcept { return elapsed < length; }
    float getCurrentValue() const noexcept { return current; }
    float getNextValue() noexcept
    {
        if (!isSmoothing()) return current;
        if (++elapsed >= length) return current = target;
        // Native non-neutral curves have a half-cell offset on the measured
        // 1024-cell phase domain. The neutral path has no such time offset.
        const auto phase = std::max(0.0, static_cast<double>(elapsed) / static_cast<double>(length)
            - (curveUsesTable ? 1.0 / 2048.0 : 0.0));
        auto position = phase;
        if (coefficient > 1.e-6)
            position = -std::expm1(-coefficient * phase) / denominator;
        else if (coefficient < -1.e-6)
            position = (std::exp(coefficient * (1.0 - phase)) - std::exp(coefficient)) / denominator;
        current = static_cast<float>(std::exp2(startPitch + pitchDistance * position));
        return current;
    }

    static double curveCoefficient(float curve) noexcept
    {
        const auto raw = utils::sanitizeUnitInterval(curve, .5f);
        if (raw == .5f) return 0.0;
        const auto index = std::min(255, static_cast<int>(std::floor(static_cast<double>(raw) * 256.0 + .5)));
        if (index == 127) return 0.0;
        // Independently derived from all 256 native bins: k = 2*logit(4*x^3)
        // on the lower half, mirrored on the upper half, x = bin/255.
        // The endpoint plateaus are separately bounded native observations.
        if (index == 0) return -230.73;
        if (index == 255) return 64.50;
        const auto position = static_cast<double>(index) / 255.0;
        const auto x = std::min(position, 1.0 - position);
        const auto midpoint = 4.0 * x * x * x;
        const auto k = 2.0 * (std::log(midpoint) - std::log1p(-midpoint));
        return position < .5 ? k : -k;
    }

private:
    float current = 440.0f, target = 440.0f;
    double startPitch = 0.0, pitchDistance = 0.0, coefficient = 0.0, denominator = 1.0;
    int64_t elapsed = 0, length = 0;
    bool curveUsesTable = false;
};
}
