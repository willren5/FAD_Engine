#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/Numeric.h>
#include "SerumFilterDrive.h"
#include <fad/engine/math/DspSafetyUtils.h>

namespace fadmodel1::dsp::filters
{
// Independently measured distortion Pre/Post SVF, with explicit 2x conversion.
// This is not the legacy TPT filter: the state update order is audible.
class SerumDistortionFilter final
{
public:
    void prepare(double rate) noexcept
    {
        sampleRate = utils::sanitizeSampleRate(rate);
        reset();
    }
    void reset() noexcept { low = band = previousInput = 0.0f; }

    void setParameters(float hz, float bandwidth) noexcept
    {
        const auto frequency = utils::sanitizeFiniteClamped(hz, 440.0f,
            1.0f, static_cast<float>(sampleRate * .49));
        const auto bw = utils::sanitizeUnitInterval(bandwidth);
        constexpr double midiZeroHz = 8.1757989156;
        const auto midi64 = std::round(768.0 * std::log2(static_cast<double>(frequency) / midiZeroHz));
        const auto gridHz = midiZeroHz * std::exp2(midi64 / 768.0);
        damping = std::max(.005f, 2.0f * (1.0f - std::sqrt(std::sqrt(.01f + bw * bw))));
        const auto requested = static_cast<float>(2.0 * std::sin(
            std::numbers::pi_v<double> * gridHz / (2.0 * sampleRate)));
        // Schur bound for this two-state update: f*f + 2*q*f < 4.
        // Only out-of-domain rate/frequency combinations reach this guard.
        const auto stableLimit = .999f * (std::sqrt(damping*damping + 4.0f) - damping);
        coefficient = std::clamp(requested, 0.0f, stableLimit);
    }

    float processSample(float input, float morph) noexcept
    {
        const auto shaped = serumFilterDrive(input, 1.0f);
        const auto position = 2.0f * utils::sanitizeUnitInterval(morph);
        const auto lowWeight = std::max(0.0f, 1.0f-position);
        const auto highWeight = std::max(0.0f, position-1.0f);
        const auto bandWeight = 1.0f-std::abs(position-1.0f);
        const auto process = [&](float sample) noexcept {
            low += coefficient * band;
            const auto high = sample - low - damping * band;
            band += coefficient * high;
            fad::engine::math::snapSmallState(low);
            fad::engine::math::snapSmallState(band);
            if (!std::isfinite(low) || !std::isfinite(band) || !std::isfinite(high))
            {
                low = band = 0.0f;
                return 0.0f;
            }
            return lowWeight*low + bandWeight*band + highWeight*high;
        };
        const auto first = process(.5f * (previousInput + shaped));
        const auto second = process(shaped);
        previousInput = shaped;
        return utils::sanitizeFinite(.5f * (first + second));
    }

private:
    double sampleRate = 48000.0;
    float coefficient = 0.0f, damping = 1.0f;
    float low = 0.0f, band = 0.0f, previousInput = 0.0f;
};
}
