#include <fad/engine/safety/AudioSafetyStage.h>
#include <fad/engine/rt/ScopedNoDenormals.h>
#include <numbers>

#include <algorithm>
#include <cmath>

namespace fad::engine
{
namespace
{
float sanitizeAndEmergencyLimit(float value, uint64_t& interventionCount) noexcept
{
    if (! std::isfinite(value) || std::abs(value) < 1.0e-20f)
        return 0.0f;

    // This stage is a quarantine guard, not a musical limiter. Leave normal
    // floating-point overs above 0 dBFS intact and only shape runaway peaks.
    constexpr float ceiling = 8.0f;   // +18.06 dBFS
    constexpr float kneeHeadroom = ceiling - AudioSafetyStage::emergencyKneeStart;
    const auto magnitude = std::abs(value);
    if (magnitude <= AudioSafetyStage::emergencyKneeStart)
        return value;

    ++interventionCount;
    const auto excess = magnitude - AudioSafetyStage::emergencyKneeStart;
    const auto shapedMagnitude =
        AudioSafetyStage::emergencyKneeStart
        + kneeHeadroom * (1.0f - std::exp(-excess / kneeHeadroom));
    return std::copysign(std::min(shapedMagnitude, ceiling), value);
}
} // namespace

void AudioSafetyStage::prepare(double sampleRate) noexcept
{
    this->sampleRate = std::isfinite(sampleRate) ? std::max(1.0, sampleRate) : 48000.0;
    nativeCompatibility = false;
    dcBlockCoeff.reset(this->sampleRate, .005);
    dcBlockCoeff.setCurrentAndTargetValue(static_cast<float>(std::exp(-2.0 * std::numbers::pi * 20.0 / this->sampleRate)));
    reset();
}

void AudioSafetyStage::setNativeCompatibility(bool enabled) noexcept
{
    if (enabled == nativeCompatibility) return;
    nativeCompatibility = enabled;
    // Preserve DC rejection and the emergency guard without audibly rotating
    // the low fundamentals of imported instruments. Legacy FAD remains at 20 Hz.
    const auto cutoff = enabled ? 2.0 : 20.0;
    const auto coefficient = static_cast<float>(std::exp(-2.0 * std::numbers::pi * cutoff / sampleRate));
    if (hasProcessedSamples)
        dcBlockCoeff.setTargetValue(coefficient);
    else
        dcBlockCoeff.setCurrentAndTargetValue(coefficient);
}

void AudioSafetyStage::reset() noexcept
{
    dcBlockCoeff.setCurrentAndTargetValue(dcBlockCoeff.getTargetValue());
    previousInput = {};
    previousOutput = {};
    hasProcessedSamples = false;
}

uint64_t AudioSafetyStage::process(AudioBufferView buffer) noexcept
{
    rt::ScopedNoDenormals noDenormals;

    const auto channelCount = std::min(2, buffer.channels());
    const auto sampleCount = buffer.frames();
    uint64_t interventionCount = 0;
    if (sampleCount > 0 && channelCount > 0)
        hasProcessedSamples = true;

    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto coefficient = dcBlockCoeff.getNextValue();
        for (int channel = 0; channel < channelCount; ++channel)
        {
            auto* samples = buffer.channel(channel);
            auto& prevIn = previousInput[static_cast<size_t>(channel)];
            auto& prevOut = previousOutput[static_cast<size_t>(channel)];
            auto value = samples[sample];
            if (!std::isfinite(value)) value = 0.0f;
            const auto filtered = value - prevIn + coefficient * prevOut;
            prevIn = value;
            prevOut = sanitizeAndEmergencyLimit(filtered, interventionCount);
            samples[sample] = prevOut;
        }
    }

    for (int channel = channelCount; channel < buffer.channels(); ++channel)
        buffer.clearChannel(channel);

    return interventionCount;
}
} // namespace fad::engine
