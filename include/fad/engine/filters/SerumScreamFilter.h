#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace fadmodel1::dsp::filters
{
// Scream LP/BP: host-rate all-pass feedback, followed by a 2x Chamberlin SVF.
// The feedback delay must not be doubled when the caller uses oversampling.
// Independent cutoff/drive/variation/noise probes are in
// Compatibility measurement origin: see docs/PROVENANCE.md.
class SerumScreamFilter final
{
public:
    void prepare(double rate) noexcept
    {
        sampleRate = std::isfinite(rate) ? std::clamp(rate, 8000.0, 384000.0) : 48000.0;
        dcPole = std::exp(-2.0 * pi * 4.0 / sampleRate);
        reset();
        setParameters(1000.0, 0.0, 0.0, .5, false);
    }

    void reset() noexcept
    {
        allPassStates.fill(0.0);
        previousDcInput = previousDcOutput = previousFilterInput = low = band = 0.0;
    }

    // Control-rate only. Arguments are physical Hz and original normalized
    // controls; a native Drive knob is not a generic decibel preamplifier.
    void setParameters(double cutoffHz, double resonance, double drive,
                       double variation, bool bandPass) noexcept
    {
        cutoffHz = finiteClamped(cutoffHz, 1000.0, 1.0, sampleRate * .49);
        resonance = finiteClamped(resonance, 0.0, 0.0, 1.0);
        drive = finiteClamped(drive, 0.0, 0.0, 1.0);
        variation = finiteClamped(variation, .5, 0.0, 1.0);
        const auto variationHz = 8.1757989156 * std::exp2(variation * (128.0 / 12.0));
        // The measured all-pass coefficient uses angular frequency directly.
        // Adding pi here moves the feedback resonances to the wrong pitches.
        const auto tangent = std::tan(variationHz / (2.0 * sampleRate));
        allPassCoefficient = (1.0 - tangent) / (1.0 + tangent);
        filterCoefficient = 2.0 * std::sin(pi * cutoffHz / (2.0 * sampleRate));
        damping = std::min(2.0 * (1.0 - std::pow(resonance * .5, .25)),
                           2.0 / filterCoefficient - filterCoefficient * .5);
        const auto complement = 1.0 - drive;
        const auto shapedDrive = 1.0 - .7 * complement * complement * complement;
        const auto feedbackScale = bandPass ? 1.25 : 1.3;
        inputGain = .5 * feedbackScale * shapedDrive;
        feedbackGain = feedbackScale * shapedDrive * shapedDrive;
        saturationCeiling = .4 * feedbackScale;
        bandPassOutput = bandPass;
    }

    float processSample(float input) noexcept
    {
        if (!std::isfinite(input)) { reset(); return 0.0f; }
        auto value = inputGain * static_cast<double>(input) + feedbackGain * previousDcOutput;
        for (auto& state : allPassStates)
        {
            const auto output = state - allPassCoefficient * value;
            state = value + allPassCoefficient * output;
            if (!finiteState(state) || !finiteState(output)) { reset(); return 0.0f; }
            value = output;
        }

        // This is the independently measured, model-specific Scream saturation
        // node AFTER both all-passes, not a generic output or safety hard clip.
        value = std::clamp(value, -saturationCeiling, saturationCeiling);
        const auto dc = value - previousDcInput + dcPole * previousDcOutput;
        previousDcInput = value;
        previousDcOutput = dc;
        const auto midpoint = .5 * (dc + previousFilterInput);
        previousFilterInput = dc;
        const auto first = processSvf(midpoint);
        const auto second = processSvf(dc);
        if (!finiteState(dc) || !finiteState(low) || !finiteState(band)) { reset(); return 0.0f; }
        return static_cast<float>(.5 * (first + second));
    }

private:
    static constexpr double pi = 3.14159265358979323846;
    static double finiteClamped(double value, double fallback, double minimum, double maximum) noexcept
    { return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback; }
    // Emergency corrupt-state recovery, far outside the model's signal range.
    static bool finiteState(double value) noexcept { return std::isfinite(value) && std::abs(value) < 1.e12; }
    double processSvf(double input) noexcept
    {
        low += filterCoefficient * band;
        const auto high = input - low - damping * band;
        band += filterCoefficient * high;
        return bandPassOutput ? band : low;
    }

    double sampleRate = 48000.0;
    double dcPole = 0.0, allPassCoefficient = 0.0, filterCoefficient = 0.0;
    double damping = 2.0, inputGain = .195, feedbackGain = .117;
    double saturationCeiling = .52;
    std::array<double, 2> allPassStates{};
    double previousDcInput = 0.0, previousDcOutput = 0.0, previousFilterInput = 0.0;
    double low = 0.0, band = 0.0;
    bool bandPassOutput = false;
};
}
