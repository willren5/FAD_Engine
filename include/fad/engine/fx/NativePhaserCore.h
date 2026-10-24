#pragma once

#include <algorithm>
#include <numbers>
#include <fad/engine/math/Numeric.h>
#include <fad/engine/math/DspMath.h>
#include <array>
#include <fad/engine/math/DspSafetyUtils.h>
#include <fad/engine/fx/NativeWetModulation.h>

namespace fadmodel1::dsp::fx
{
// Independently identified from native static, stereo and moving sweeps.
// This owns the sample clock, not a host block clock. No allocation or I/O.
class NativePhaserCore final
{
public:
    void prepare(double rate) noexcept
    {
        sampleRate = utils::sanitizeSampleRate(rate);
        idleLimit = std::max(1, fad::engine::math::roundToNearestInt(sampleRate * (1504.0 / 48000.0)));
        reset();
    }

    void reset() noexcept
    {
        states = {};
        previousWet = {};
        phase = 0.0f;widePhase=0;
        idleSamples = 0;
    }

    void setParameters(float frequency, float rateHz, float depth, float stereo,
                       float feedback, float mix, bool preciseRate = false) noexcept
    {
        const auto raw = utils::sanitizeUnitInterval(frequency);
        // The displayed frequency is not the DSP frequency. The native core
        // uses 20*800^raw, capped at 10 kHz, before its sine coefficient law.
        const auto hz = std::min(10000.0, 20.0 * std::pow(800.0, static_cast<double>(raw)));
        const auto d = static_cast<double>(utils::sanitizeUnitInterval(depth));
        const auto sweep = 14210.0 * d * d * d;
        for (size_t i = 0; i < spacing.size(); ++i)
        {
            baseG[i] = std::max(1.e-8, 2.0 * std::sin(spacing[i] * hz / (2.0 * sampleRate)));
            sweepG[i] = spacing[i] * sweep / sampleRate;
        }
        if(preciseRate && !precise)widePhase=phase;
        if(!preciseRate && precise)phase=static_cast<float>(widePhase);
        precise=preciseRate;
        wideIncrement=utils::sanitizeFiniteClamped(rateHz,0.0f,0.0f,100.0f)/sampleRate;
        increment = static_cast<float>(utils::sanitizeFiniteClamped(rateHz, 0.0f, 0.0f, 100.0f) / sampleRate);
        stereoPhase = utils::sanitizeUnitInterval(stereo);
        feedbackGain = .99 * static_cast<double>(utils::sanitizeUnitInterval(feedback));
        wetGain = serumModulationFxWetGain(mix);
        dryGain = 1.0 - wetGain * wetGain;
    }

    std::array<float, 2> process(std::array<float, 2> input, bool activityKnown,
                                 bool voiceActive) noexcept
    {
        bool active = activityKnown && voiceActive;
        for (auto& sample : input)
        {
            sample = utils::sanitizeFinite(sample);
            if (!activityKnown) active = active || std::abs(sample) > 1.e-10f;
        }
        const bool advance = active || idleSamples < idleLimit;
        idleSamples = active ? 0 : std::min(idleLimit, idleSamples + 1);
        std::array<float, 2> output{};
        for (size_t channel = 0; channel < states.size(); ++channel)
        {
            auto p = (precise?widePhase:static_cast<double>(phase)) + static_cast<double>(channel) * stereoPhase;
            p -= std::floor(p);
            const auto triangle = 1.0 - std::abs(2.0 * p - 1.0);
            auto wet = static_cast<double>(input[channel]) + feedbackGain * previousWet[channel];
            bool finite = true;
            for (size_t stage = 0; stage < spacing.size(); ++stage)
            {
                const auto g = baseG[stage] + sweepG[stage] * triangle;
                const auto a = (1.0 - g) / (1.0 + g);
                auto& history = states[channel][stage];
                const auto value = history - a * wet;
                history = wet + a * value;
                fad::engine::math::snapSmallState(history);
                finite = finite && std::isfinite(value) && std::isfinite(history);
                wet = value;
            }
            const auto value = dryGain * input[channel] + wetGain * wet;
            if (!finite || !std::isfinite(static_cast<float>(value)))
            {
                states[channel] = {};
                previousWet[channel] = 0.0;
                output[channel] = input[channel];
            }
            else
            {
                previousWet[channel] = wet;
                fad::engine::math::snapSmallState(previousWet[channel]);
                output[channel] = static_cast<float>(value);
            }
        }
        if (advance)
        {
            // Native repeated-addition float phase is observable in long notes.
            if(precise){widePhase+=wideIncrement;widePhase-=std::floor(widePhase);}
            else {phase += increment;phase -= std::floor(phase);}
        }
        return output;
    }

private:
    double widePhase=0,wideIncrement=0;
    bool precise=false;
    static constexpr std::array<double, 8> spacing{
        3.817756106839613, 4.250275461242923, 4.254998986613720, 4.975561502418792,
        9.060489495387662, 9.798114804097322, 15.41919971020319, 15.975656283036317};
    double sampleRate = 48000.0;
    std::array<std::array<double, 8>, 2> states{};
    std::array<double, 8> baseG{}, sweepG{};
    std::array<double, 2> previousWet{};
    double feedbackGain = 0.0, wetGain = 0.0, dryGain = 1.0, stereoPhase = 0.0;
    float phase = 0.0f, increment = 0.0f;
    int idleSamples = 0, idleLimit = 1504;
};
}
