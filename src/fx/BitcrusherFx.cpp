#include <fad/engine/fx/BitcrusherFx.h>
#include <fad/engine/fx/FxCatalog.h>
#include <fad/engine/math/DspSafetyUtils.h>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/Numeric.h>
#include <fad/engine/rt/ScopedNoDenormals.h>
#include <algorithm>
#include <cassert>

namespace fadmodel1::dsp::fx
{
namespace
{
constexpr auto smoothingSeconds = 0.03;
constexpr float minCrushRateHz = 500.0f;

const fad::engine::math::RuntimeRange bitsRange(1.0f, 16.0f, 1.0f);

float resolveCrushRateHz(float normalizedRate, double hostSampleRate) noexcept
{
    const auto safeNormalizedRate = utils::sanitizeUnitInterval(normalizedRate);
    const auto hostRate = static_cast<float>(
        std::max(static_cast<double>(minCrushRateHz), hostSampleRate));
    if (FxCatalog::isHostRateEndpoint(FxType::bitcrusher, 1, safeNormalizedRate))
        return hostRate;

    const auto semanticRate = FxCatalog::denormalizeParamValue(
        FxType::bitcrusher, 1, safeNormalizedRate);
    return std::clamp(semanticRate, minCrushRateHz, hostRate);
}
}

void BitcrusherFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    dryBuffer.setSize(numChannels, static_cast<int>(spec.maximumBlockSize), false, false, true);

    bits.reset(sampleRate, smoothingSeconds);
    crushRate.reset(sampleRate, smoothingSeconds);
    mix.reset(sampleRate, smoothingSeconds);
    jitter.reset(sampleRate, smoothingSeconds);
    driveGain.reset(sampleRate, smoothingSeconds);

    bits.setCurrentAndTargetValue(16.0f);
    crushRate.setCurrentAndTargetValue(resolveCrushRateHz(crushRateNormalized, sampleRate));
    mix.setCurrentAndTargetValue(0.0f);
    jitter.setCurrentAndTargetValue(0.0f);
    driveGain.setCurrentAndTargetValue(1.0f);
    parametersPrimed = false;
    reset();
}

void BitcrusherFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
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

void BitcrusherFxCore::processActiveBlock(fad::engine::ProcessContextReplacing<float>& context)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());

    if (blockChannels <= 0
        || blockChannels > dryBuffer.getNumChannels()
        || numSamples <= 0
        || numSamples > dryBuffer.getNumSamples())
        return;

    parametersPrimed = true;

    for (int channel = 0; channel < blockChannels; ++channel)
        std::copy_n(outputBlock.getChannelPointer(static_cast<size_t>(channel)), numSamples, dryBuffer.getWritePointer(channel));

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto currentBits = utils::sanitizeFiniteClamped(bits.getNextValue(), 16.0f, 1.0f, 16.0f);
        const auto currentRate = utils::sanitizeFiniteClamped(crushRate.getNextValue(),
                                                              static_cast<float>(sampleRate),
                                                              minCrushRateHz,
                                                              static_cast<float>(sampleRate));
        const auto currentMix = utils::sanitizeUnitInterval(mix.getNextValue());
        const auto currentJitter = utils::sanitizeUnitInterval(jitter.getNextValue());
        const auto currentDriveGain = utils::sanitizeFiniteClamped(driveGain.getNextValue(), 1.0f, 0.0f, 32.0f);
        const auto dry = 1.0f - currentMix;
        const bool transparentMode = currentBits >= 15.5f
            && currentRate >= static_cast<float>(sampleRate) - 1.0f
            && currentJitter <= 1.0e-4f
            && currentDriveGain <= 1.0001f;

        float step = 0.0f;
        float levels = 0.0f;
        float jitterOffset = 0.0f;
        if (! transparentMode)
        {
            step = currentRate / static_cast<float>(sampleRate);
            levels = std::pow(2.0f, std::max(1.0f, currentBits)) - 1.0f;
            // Jitter intentionally advances once per sample and is shared by both channels.
            jitterOffset = currentJitter * (rng.nextFloat() * 2.0f - 1.0f) * step;
        }

        for (int channel = 0; channel < blockChannels; ++channel)
        {
            const auto inputSample = utils::sanitizeFinite(dryBuffer.getSample(channel, sampleIndex));

            if (transparentMode)
            {
                outputBlock.getChannelPointer(static_cast<size_t>(channel))[sampleIndex] = inputSample;
                continue;
            }

            // 1. Pre-drive
            const auto driven = currentDriveGain > 1.0001f
                ? std::tanh(inputSample * currentDriveGain)
                : inputSample;

            // 2. Sample rate reduction
            auto& counter = (channel == 0) ? counterL : counterR;
            auto& hold = (channel == 0) ? holdL : holdR;

            if (! std::isfinite(counter))
                counter = 0.0f;
            if (! std::isfinite(hold))
                hold = 0.0f;

            counter += utils::sanitizeFinite(step + jitterOffset);

            if (counter >= 1.0f)
            {
                counter -= std::floor(counter);
                hold = utils::sanitizeFinite(driven);
            }

            // 3. Bit reduction
            const auto crushed = std::round(hold * levels) / levels;

            // 4. Mix
            outputBlock.getChannelPointer(static_cast<size_t>(channel))[sampleIndex] =
                utils::sanitizeFinite(inputSample * dry + crushed * currentMix);
        }
    }
}

void BitcrusherFxCore::reset()
{
    holdL = 0.0f;
    holdR = 0.0f;
    counterL = 0.0f;
    counterR = 0.0f;
    rng.setSeed(jitterSeed);
    parametersPrimed = false;

    bits.setCurrentAndTargetValue(bits.getTargetValue());
    crushRate.setCurrentAndTargetValue(crushRate.getTargetValue());
    mix.setCurrentAndTargetValue(mix.getTargetValue());
    jitter.setCurrentAndTargetValue(jitter.getTargetValue());
    driveGain.setCurrentAndTargetValue(driveGain.getTargetValue());
}

void BitcrusherFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        holdL = 0.0f;
        holdR = 0.0f;
        counterL = 0.0f;
        counterR = 0.0f;
        rng.setSeed(jitterSeed);
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void BitcrusherFxCore::setGenericParam(int index, float value01)
{
    const auto safeValue01 = utils::sanitizeUnitInterval(value01);

    const auto setSmoothedValue = [this](auto& smoother, float value)
    {
        if (! parametersPrimed)
            smoother.setCurrentAndTargetValue(value);
        else
            smoother.setTargetValue(value);
    };

    switch (index)
    {
        case 0: // Bits (1-16 discrete)
            setSmoothedValue(bits, std::round(bitsRange.denormalise(safeValue01)));
            break;
        case 1: // Rate: <1 keeps fixed 500-44100 Hz semantics; 1 is the host-rate transparent endpoint.
        {
            crushRateNormalized = safeValue01;
            setSmoothedValue(crushRate, resolveCrushRateHz(crushRateNormalized, sampleRate));
            break;
        }
        case 2: // Mix (0-1)
            setSmoothedValue(mix, safeValue01);
            break;
        case 3: // Jitter (0-1)
            setSmoothedValue(jitter, safeValue01);
            break;
        case 4: // Drive (0-30 dB)
            setSmoothedValue(driveGain,
                             fad::engine::math::gainFromDecibels(fad::engine::math::interpolate(safeValue01, 0.0f, 30.0f)));
            break;
        default: break;
    }
}
}
