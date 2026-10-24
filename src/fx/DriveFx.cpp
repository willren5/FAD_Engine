#include <fad/engine/fx/DriveFx.h>
#include <fad/engine/fx/FxCatalog.h>
#include <fad/engine/math/DspSafetyUtils.h>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/Numeric.h>
#include <fad/engine/rt/ScopedNoDenormals.h>
#include <algorithm>

namespace fadmodel1::dsp::fx
{
namespace
{
constexpr auto driveSmoothingSeconds = 0.03;
constexpr auto mixSmoothingSeconds = 0.02;
}

void DriveFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    dryBuffer.setSize(numChannels, static_cast<int>(spec.maximumBlockSize), false, false, true);
    driveGain.reset(sampleRate, driveSmoothingSeconds);
    wetMix.reset(sampleRate, mixSmoothingSeconds);
    modeFadeSamples = std::clamp(fad::engine::math::roundToNearestInt(static_cast<float>(sampleRate) * 0.003f), 16, 256);

    for (auto& filter : highPassFilters)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& filter : lowPassFilters)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    reset();
}

void DriveFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
{
    if (bypassed.load(std::memory_order_relaxed))
        return;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());

    if (blockChannels <= 0 || blockChannels > dryBuffer.getNumChannels())
        return;

    if (numSamples > dryBuffer.getNumSamples())
    {
        const auto chunkCapacity = dryBuffer.getNumSamples();
        if (chunkCapacity <= 0)
            return;

        for (int offset = 0; offset < numSamples;)
        {
            const auto chunkSamples = std::min(chunkCapacity, numSamples - offset);
            auto chunkBlock = outputBlock.getSubBlock(static_cast<size_t>(offset),
                                                      static_cast<size_t>(chunkSamples));
            auto chunkContext = fad::engine::ProcessContextReplacing<float>(chunkBlock);
            processActiveBlock(chunkContext);
            offset += chunkSamples;
        }
        return;
    }

    processActiveBlock(context);
}

void DriveFxCore::processActiveBlock(fad::engine::ProcessContextReplacing<float>& context)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());

    if (blockChannels <= 0
        || blockChannels > dryBuffer.getNumChannels()
        || numSamples > dryBuffer.getNumSamples())
        return;

    for (int channel = 0; channel < blockChannels; ++channel)
        std::copy_n(outputBlock.getChannelPointer(static_cast<size_t>(channel)), numSamples, dryBuffer.getWritePointer(channel));

    updateToneFilters();

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto nextMode = std::clamp(requestedMode.load(std::memory_order_relaxed), 0, 2);
        if (nextMode != activeMode)
        {
            activeMode = nextMode;
            if (hasProcessedSample)
            {
                modeFadeStartSamples = lastOutputSamples;
                modeFadeRemaining = modeFadeSamples;
            }
        }

        const auto gain = utils::sanitizeFiniteClamped(driveGain.getNextValue(), 1.0f, 0.0f, 32.0f);
        const auto excessGain = std::max(0.0f, gain - 1.0f);
        const auto driveCompensation = 1.0f / std::sqrt(1.0f + 0.35f * excessGain);
        const auto mix = utils::sanitizeUnitInterval(wetMix.getNextValue());
        const auto dry = 1.0f - mix;

        for (int channel = 0; channel < blockChannels; ++channel)
        {
            const auto inputSample = utils::sanitizeFinite(dryBuffer.getSample(channel, sampleIndex));
            auto processed = inputSample;
            if (excessGain > 1.0e-4f)
            {
                processed = processWave(
                    inputSample * gain, static_cast<parameters::DriveMode>(activeMode))
                    * driveCompensation;
                processed = highPassFilters[static_cast<size_t>(channel)].processSample(0, processed);
                highPassFilters[static_cast<size_t>(channel)].snapToZero();
                processed = lowPassFilters[static_cast<size_t>(channel)].processSample(0, processed);
                lowPassFilters[static_cast<size_t>(channel)].snapToZero();
                if (! std::isfinite(processed))
                {
                    highPassFilters[static_cast<size_t>(channel)].reset();
                    lowPassFilters[static_cast<size_t>(channel)].reset();
                    processed = 0.0f;
                }
            }
            auto output = utils::sanitizeFinite(inputSample * dry + processed * mix);
            if (modeFadeRemaining > 0)
            {
                const auto fadeOut = static_cast<float>(modeFadeRemaining)
                    / static_cast<float>(modeFadeSamples);
                output = modeFadeStartSamples[static_cast<size_t>(channel)] * fadeOut
                    + output * (1.0f - fadeOut);
            }
            output = utils::sanitizeFinite(output);
            outputBlock.getChannelPointer(static_cast<size_t>(channel))[sampleIndex] = output;
            lastOutputSamples[static_cast<size_t>(channel)] = output;
        }

        hasProcessedSample = true;
        if (modeFadeRemaining > 0)
            --modeFadeRemaining;
    }
}

void DriveFxCore::reset()
{
    driveGain.setCurrentAndTargetValue(driveGain.getTargetValue());
    wetMix.setCurrentAndTargetValue(wetMix.getTargetValue());

    for (auto& filter : highPassFilters)
        filter.reset();

    for (auto& filter : lowPassFilters)
        filter.reset();

    activeMode = std::clamp(requestedMode.load(std::memory_order_relaxed), 0, 2);
    modeFadeRemaining = 0;
    modeFadeStartSamples = {};
    lastOutputSamples = {};
    hasProcessedSample = false;
    updateToneFilters();
}

void DriveFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        for (auto& filter : highPassFilters)
            filter.reset();
        for (auto& filter : lowPassFilters)
            filter.reset();
        modeFadeRemaining = 0;
        hasProcessedSample = false;
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void DriveFxCore::setGenericParam(int index, float value01)
{
    const auto safeValue01 = utils::sanitizeUnitInterval(value01);

    switch (index)
    {
        case 0:
            snapshot.amount = safeValue01;
            driveGain.setTargetValue(
                fad::engine::math::gainFromDecibels(fad::engine::math::interpolate(safeValue01, 0.0f, 30.0f)));
            break;
        case 1:
            snapshot.tone = safeValue01;
            updateToneFilters();
            break;
        case 2:
            snapshot.mix = safeValue01;
            wetMix.setTargetValue(safeValue01);
            break;
        case 3:
            snapshot.mode = static_cast<parameters::DriveMode>(std::clamp(fad::engine::math::roundToNearestInt(
                    FxCatalog::denormalizeParamValue(FxType::drive, 3, safeValue01)), 0, 2));
            requestedMode.store(static_cast<int>(snapshot.mode), std::memory_order_relaxed);
            break;
        default: break;
    }
}

void DriveFxCore::setSnapshot(const parameters::DriveSnapshot& newSnapshot) noexcept
{
    snapshot = newSnapshot;
    snapshot.amount = utils::sanitizeUnitInterval(snapshot.amount);
    snapshot.tone = utils::sanitizeUnitInterval(snapshot.tone, 0.5f);
    snapshot.mix = utils::sanitizeUnitInterval(snapshot.mix);
    snapshot.mode = static_cast<parameters::DriveMode>(std::clamp(static_cast<int>(snapshot.mode), 0, 2));
    requestedMode.store(static_cast<int>(snapshot.mode), std::memory_order_relaxed);
    driveGain.setTargetValue(fad::engine::math::gainFromDecibels(fad::engine::math::interpolate(snapshot.amount, 0.0f, 30.0f)));
    wetMix.setTargetValue(snapshot.mix);
}

float DriveFxCore::processWave(float input, parameters::DriveMode mode) noexcept
{
    switch (mode)
    {
        case parameters::DriveMode::hardClip:
            return std::clamp(input, -1.0f, 1.0f);

        case parameters::DriveMode::tube:
        {
            const auto asymmetric = input + 0.18f * input * input;
            return 0.92f * std::tanh(asymmetric);
        }

        case parameters::DriveMode::softClip:
        default:
            return std::tanh(input);
    }
}

void DriveFxCore::updateToneFilters() noexcept
{
    const auto tone = utils::sanitizeUnitInterval(snapshot.tone, 0.5f);
    const auto lowCutHz = fad::engine::math::interpolate(1.0f - tone, 20.0f, 160.0f);
    const auto nyquistSafeHighCut =
        static_cast<float>(std::max(2000.0, sampleRate * 0.49));
    const auto highCutHz = std::clamp(fad::engine::math::interpolate(tone, 2200.0f, 18000.0f), lowCutHz + 20.0f, nyquistSafeHighCut);

    for (auto& filter : highPassFilters)
    {
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setCutoffFrequency(lowCutHz);
        filter.setResonance(0.707f);
    }

    for (auto& filter : lowPassFilters)
    {
        filter.setType(fad::engine::filters::TptMode::lowpass);
        filter.setCutoffFrequency(highCutHz);
        filter.setResonance(0.707f);
    }
}
}
