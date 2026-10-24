#pragma once

#include <numbers>
#include <fad/engine/math/DspMath.h>
#include <array>
#include <vector>
#include "SerumFilterDrive.h"
#include <fad/engine/math/DspSafetyUtils.h>

namespace fadmodel1::dsp::filters
{
// Bounded host-rate cubic delay. Flg H6+ / H6- use the measured 2x Euler
// high-pass feedback; Cmb L6+ uses the complementary low-pass path.
class SerumCombFilter final
{
public:
    void prepare(double rate)
    {
        sampleRate = utils::sanitizeSampleRate(rate);
        history.assign(static_cast<size_t>(std::ceil(sampleRate / minimumHz)) + 5, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        // O(1) note/model reset: old memory is unreadable until rewritten.
        writePosition = validSamples = 0;
        input1 = input2 = hpOutput = delayedOutput = 0.0;
    }

    void setParameters(float frequencyHz, float resonance, float driveDb,
                       float variation, bool negative, bool lowPassFeedback = false) noexcept
    {
        const auto safeHz = static_cast<double>(utils::sanitizeFiniteClamped(
            frequencyHz, 440.0f, static_cast<float>(minimumHz), static_cast<float>(sampleRate * .5)));
        const auto midi64 = std::round(768.0 * std::log2(safeHz / minimumHz));
        const auto hz = std::clamp(minimumHz * std::exp2(midi64 / 768.0), minimumHz, sampleRate * .5);
        const auto period = std::clamp(sampleRate / hz, 2.0, static_cast<double>(history.size() > 4 ? history.size() - 4 : 2));
        delayInteger = static_cast<size_t>(std::floor(period));
        const auto f = period - static_cast<double>(delayInteger), f2 = f * f, f3 = f2 * f;
        interpolation = {-.5 * f * (1.0 - f) * (1.0 - f), 1.0 - 2.5 * f2 + 1.5 * f3,
            .5 * f + 2.0 * f2 - 1.5 * f3, -.5 * f2 * (1.0 - f)};
        const auto octave = (2.0 * utils::sanitizeUnitInterval(variation) - 1.0) * (128.0 / 12.0);
        const auto hpHz = std::min(sampleRate * .5, hz * std::exp2(octave));
        const auto g = 2.0 * std::sin(std::numbers::pi * hpHz / (2.0 * sampleRate));
        const auto a = (3.0 - g) * .25, b = (1.0 - g) * .25;
        hpNumerator = {a, b - a, -b};
        hpPole = (1.0 - g) * (1.0 - g);
        feedback = utils::sanitizeUnitInterval(resonance);
        dryGain = 1.0 / (1.0 + feedback);
        sign = negative ? -1.0 : 1.0;
        lowPass = lowPassFeedback;
        inputGain = std::pow(10.0, static_cast<double>(utils::sanitizeFiniteClamped(driveDb, 0.0f, 0.0f, 36.0f)) / 20.0);
    }

    float processSample(float input) noexcept
    {
        if (history.size() < 5) return utils::sanitizeFinite(input);
        const auto driven = static_cast<double>(utils::sanitizeFinite(input)) * inputGain;
        const auto dry = static_cast<double>(serumFilterDrive(static_cast<float>(driven), 4.0f));
        double wet = 0.0;
        for (size_t tap = 0; tap < interpolation.size(); ++tap)
        {
            const auto distance = delayInteger + tap - 1;
            if (distance > validSamples || distance >= history.size()) continue;
            const auto position = writePosition >= distance ? writePosition - distance : history.size() + writePosition - distance;
            wet += interpolation[tap] * history[position];
        }
        const auto value = static_cast<double>(serumFilterDrive(static_cast<float>(driven + sign * feedback * wet), 4.0f));
        const auto filtered = hpNumerator[0] * value + hpNumerator[1] * input1
            + hpNumerator[2] * input2 + hpPole * hpOutput;
        input2 = input1; input1 = value; hpOutput = filtered;
        fad::engine::math::snapSmallState(hpOutput);
        // Isolated Cmb L6+ warm-note renders have about 2.8 dB less output
        // than the Flg H6 comb sum across zero-drive/resonance controls.
        const auto currentOutput = (lowPass ? .72 : 1.0)
            * .5 * (dry * dryGain + sign * wet);
        if (!std::isfinite(static_cast<float>(filtered)) || !std::isfinite(static_cast<float>(currentOutput)))
        { reset(); return 0.0f; }
        history[writePosition] = static_cast<float>(lowPass ? value - filtered : filtered);
        if (++writePosition == history.size()) writePosition = 0;
        validSamples = std::min(history.size() - 1, validSamples + 1);
        const auto output = delayedOutput;
        delayedOutput = currentOutput;
        return utils::sanitizeFinite(static_cast<float>(output));
    }

private:
    static constexpr double minimumHz = 8.1757989156;
    std::vector<float> history;
    std::array<double, 4> interpolation{0, 1, 0, 0};
    std::array<double, 3> hpNumerator{};
    size_t writePosition = 0, validSamples = 0, delayInteger = 2;
    double sampleRate = 48000.0, hpPole = 0.0, input1 = 0.0, input2 = 0.0, hpOutput = 0.0;
    double inputGain = 1.0, feedback = 0.0, dryGain = 1.0, sign = 1.0, delayedOutput = 0.0;
    bool lowPass = false;
};
}
