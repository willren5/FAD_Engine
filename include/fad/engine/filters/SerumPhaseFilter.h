#pragma once

#include <numbers>
#include <fad/engine/math/DspMath.h>
#include <array>
#include "SerumFilterDrive.h"
#include <fad/engine/math/DspSafetyUtils.h>

namespace fadmodel1::dsp::filters
{
// Independently measured Phs 36+/48+ core. The caller owns rate conversion:
// native main filters run at 2x, while the native FX versions run at host rate.
class SerumPhaseFilter final
{
public:
    void prepare(double rate) noexcept
    {
        sampleRate = utils::sanitizeSampleRate(rate);
        reset();
    }

    void reset() noexcept
    {
        states.fill(0.0f);
        previousWet = 0.0f;
    }

    void setParameters(float frequencyHz, float resonance, int model) noexcept
    {
        const int requestedStages = model == 59 ? 6 : 8;
        if (requestedStages != stages)
        {
            reset();
            stages = requestedStages;
        }
        const auto hz = utils::sanitizeFiniteClamped(frequencyHz, 440.0f,
            1.0f, static_cast<float>(sampleRate * .49));
        constexpr double midiZeroHz = 8.1757989156;
        const auto midi64 = std::round(768.0 * std::log2(static_cast<double>(hz) / midiZeroHz));
        const auto gridHz = midiZeroHz * std::exp2(midi64 / 768.0);
        // This is the measured native coefficient, not the usual bilinear
        // (tan(w)-1)/(tan(w)+1) mapping used by the legacy Phaser effect.
        coefficient = static_cast<float>(std::clamp(1.0 - std::tan(
            std::numbers::pi * gridHz / sampleRate), -.9999, .99999));
        feedback = utils::sanitizeFiniteClamped(resonance, 0.0f, 0.0f, .9999f);
    }

    float processSample(float input) noexcept
    {
        input = utils::sanitizeFinite(input);
        const auto dry = serumFilterDrive(input, 4.0f);
        auto wet = serumFilterDrive(input + feedback * previousWet, 4.0f);
        for (int stage = 0; stage < stages; ++stage)
        {
            auto& state = states[static_cast<size_t>(stage)];
            const auto output = -coefficient * wet + state;
            state = wet + coefficient * output;
            fad::engine::math::snapSmallState(state);
            if (!std::isfinite(output) || !std::isfinite(state))
            {
                reset();
                return 0.0f;
            }
            wet = output;
        }
        previousWet = wet;
        fad::engine::math::snapSmallState(previousWet);
        return utils::sanitizeFinite(.5f * (dry / (1.0f + feedback) + wet));
    }

private:
    std::array<float, 8> states{};
    double sampleRate = 48000.0;
    float coefficient = 0.5f;
    float feedback = 0.0f;
    float previousWet = 0.0f;
    int stages = 8;
};
}
