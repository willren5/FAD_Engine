#include <fad/engine/fx/ReverbFx.h>
#include <fad/engine/fx/FxCatalog.h>
#include <fad/engine/fx/FxTail.h>
#include <fad/engine/math/DspSafetyUtils.h>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/Numeric.h>
#include <fad/engine/rt/ScopedNoDenormals.h>
#include <algorithm>
#include <cassert>

#include <numeric>

namespace fadmodel1::dsp::fx
{
namespace
{
constexpr double maxPreDelaySeconds = 0.25;
constexpr auto preDelaySmoothingSeconds = 0.05;
constexpr auto parameterSmoothingSeconds = 0.03;
constexpr int parameterUpdateBlockSize = 16;
constexpr float maxNetworkDelaySeconds = 0.18f;
constexpr float feedbackHighPassCutoffHz = 12.0f;
constexpr std::array<float, ReverbFxCore::networkLineCount> baseDelayMs{
    29.7f, 33.1f, 37.9f, 41.3f, 43.7f, 47.9f, 53.3f, 61.1f
};
constexpr std::array<float, ReverbFxCore::networkLineCount> inputSigns{
    1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f
};

float consumeSmoothedValue(fad::engine::math::LinearSmoother<float>& smoother, int numSamples) noexcept
{
    if (numSamples <= 0)
        return smoother.getCurrentValue();

    const auto currentValue = smoother.getNextValue();
    if (numSamples > 1)
        smoother.skip(numSamples - 1);
    return currentValue;
}
}

void ReverbFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = fadmodel1::dsp::utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    dryBuffer.setSize(numChannels, static_cast<int>(spec.maximumBlockSize), false, false, true);
    const fad::engine::ProcessSpec safeSpec{sampleRate, spec.maximumBlockSize, spec.numChannels};
    const fad::engine::ProcessSpec monoSpec{sampleRate, spec.maximumBlockSize, 1u};

    preDelayLine.prepare(safeSpec);
    preDelayLine.setMaximumDelayInSamples(static_cast<int>(sampleRate * maxPreDelaySeconds) + 1);

    const auto maxDelaySamples = static_cast<int>(sampleRate * maxNetworkDelaySeconds) + 1;
    for (auto& delay : networkDelays)
    {
        delay.prepare(monoSpec);
        delay.setMaximumDelayInSamples(maxDelaySamples);
    }

    nativeHall.prepare(sampleRate);
    hallDecay.reset(sampleRate, parameterSmoothingSeconds);
    hallHiCut.reset(sampleRate, parameterSmoothingSeconds);
    hallLoCut.reset(sampleRate, parameterSmoothingSeconds);
    hallSpinRate.reset(sampleRate, parameterSmoothingSeconds);
    hallSpinDepth.reset(sampleRate, parameterSmoothingSeconds);
    hallBlend.reset(sampleRate, .005);
    modeNormalized = legacyHallDecayNormalized = 0.0f;
    hallDecay.setCurrentAndTargetValue(-1.0f);
    hallHiCut.setCurrentAndTargetValue(0.0f);
    hallLoCut.setCurrentAndTargetValue(0.0f);
    hallSpinRate.setCurrentAndTargetValue(0.0f);
    hallSpinDepth.setCurrentAndTargetValue(0.0f);
    hallBlend.setCurrentAndTargetValue(0.0f);
    roomSize.reset(sampleRate, parameterSmoothingSeconds);
    nativeDecaySeconds.reset(sampleRate, parameterSmoothingSeconds);
    nativeProfileBlend.reset(sampleRate, .005);
    damping.reset(sampleRate, parameterSmoothingSeconds);
    wetLevel.reset(sampleRate, parameterSmoothingSeconds);
    width.reset(sampleRate, parameterSmoothingSeconds);
    highCutHz.reset(sampleRate, parameterSmoothingSeconds);
    lowCutHz.reset(sampleRate, parameterSmoothingSeconds);
    preDelaySamples.reset(sampleRate, preDelaySmoothingSeconds);

    for (auto& filter : outputHighPass)
        filter.prepare(monoSpec);
    for (auto& filter : outputLowPass)
        filter.prepare(monoSpec);
    for (auto& filter : feedbackHighPass)
    {
        filter.prepare(monoSpec);
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setCutoffFrequency(feedbackHighPassCutoffHz);
        filter.setResonance(0.707f);
    }

    stereoInputSigns = inputSigns;
    reset();
}

void ReverbFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
{
    if (bypassed.load(std::memory_order_relaxed))
        return;

    auto& block = context.getOutputBlock();
    const auto numSamples = static_cast<int>(block.getNumSamples());
    const auto scratchCapacity = dryBuffer.getNumSamples();

    if (numSamples > scratchCapacity)
    {
        if (scratchCapacity <= 0)
            return;

        for (int offset = 0; offset < numSamples; offset += scratchCapacity)
        {
            auto chunkBlock = block.getSubBlock(static_cast<size_t>(offset),
                                                static_cast<size_t>(std::min(scratchCapacity, numSamples - offset)));
            auto chunkContext = fad::engine::ProcessContextReplacing<float>(chunkBlock);
            processBlockInternal(chunkContext);
        }
        return;
    }

    processBlockInternal(context);
}

void ReverbFxCore::processBlockInternal(fad::engine::ProcessContextReplacing<float>& context)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& block = context.getOutputBlock();
    const auto numSamples = static_cast<int>(block.getNumSamples());
    const auto blockChannels = static_cast<int>(block.getNumChannels());
    if (blockChannels <= 0 || blockChannels > numChannels || numSamples <= 0)
        return;

    const auto channels = blockChannels;
    assert(channels <= dryBuffer.getNumChannels());
    assert(numSamples <= dryBuffer.getNumSamples());

    for (int channel = 0; channel < channels; ++channel)
    {
        auto* dry = dryBuffer.getWritePointer(channel);
        auto* samples = block.getChannelPointer(static_cast<size_t>(channel));
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto value = samples[sample];
            const auto safeValue = std::isfinite(value) ? value : 0.0f;
            dry[sample] = safeValue;
            samples[sample] = safeValue;
        }
    }

    for (int offset = 0; offset < numSamples;)
    {
        if (parameterQuantumSamplesRemaining <= 0)
        {
            refreshAppliedParameters();
            parameterQuantumSamplesRemaining = parameterUpdateBlockSize;
        }

        const auto chunkSize = std::min(parameterQuantumSamplesRemaining, numSamples - offset);

        auto chunkBlock = block.getSubBlock(static_cast<size_t>(offset),
                                            static_cast<size_t>(chunkSize));

        for (int i = 0; i < chunkSize; ++i)
        {
            float left = chunkBlock.getChannelPointer(0)[i];
            float right = channels > 1 ? chunkBlock.getChannelPointer(1)[i] : left;

            // Advance history on every sample, including zero-ms direct
            // output. Otherwise turning delay back on replays expired input.
            const auto delaySmp = fadmodel1::dsp::utils::sanitizeFiniteClamped(
                preDelaySamples.getNextValue(), 0.0f, 0.0f, static_cast<float>(sampleRate * maxPreDelaySeconds));
            preDelayLine.pushSample(0, left);
            left = preDelayLine.popSample(0, delaySmp);

            if (channels > 1)
            {
                preDelayLine.pushSample(1, right);
                right = preDelayLine.popSample(1, delaySmp);
            }
            else
                right = left; // Mirror after delay, not the undelayed mono input.

            std::array<float, 2> wetStereo{};
            if (appliedHallBlend < 1.0f) {
                wetStereo = processNetworkSample(left, right, appliedWidth);
                wetStereo[0] = outputHighPass[0].processSample(0, wetStereo[0]);
                outputHighPass[0].snapToZero();
                wetStereo[0] = outputLowPass[0].processSample(0, wetStereo[0]);
                outputLowPass[0].snapToZero();
                wetStereo[0] = fadmodel1::dsp::utils::sanitizeFinite(wetStereo[0]);

                if (channels > 1)
                {
                    wetStereo[1] = outputHighPass[1].processSample(0, wetStereo[1]);
                    outputHighPass[1].snapToZero();
                    wetStereo[1] = outputLowPass[1].processSample(0, wetStereo[1]);
                    outputLowPass[1].snapToZero();
                    wetStereo[1] = fadmodel1::dsp::utils::sanitizeFinite(wetStereo[1]);
                }
                else
                {
                    wetStereo[1] = wetStereo[0];
                }

            }
            if (appliedHallBlend > 0.0f) {
                const auto hall = nativeHall.process(left, right);
                for (size_t channel = 0; channel < 2; ++channel)
                    wetStereo[channel] += appliedHallBlend * (hall[channel] - wetStereo[channel]);
            }
            const auto dryLeft = dryBuffer.getReadPointer(0, offset)[i];
            chunkBlock.getChannelPointer(0)[i] = dryLeft * appliedDryGain + wetStereo[0] * appliedWetGain;

            if (channels > 1)
            {
                const auto dryRight = dryBuffer.getReadPointer(1, offset)[i];
                chunkBlock.getChannelPointer(1)[i] = dryRight * appliedDryGain + wetStereo[1] * appliedWetGain;
            }
        }

        parameterQuantumSamplesRemaining -= chunkSize;
        offset += chunkSize;
    }
}

void ReverbFxCore::reset()
{
    originalTailValidSamples = -1;
    nativeWetOffset = nativeDecayOffset = 0.0f;
    for (auto* control : {&hallDecay, &hallHiCut, &hallLoCut, &hallSpinRate, &hallSpinDepth, &hallBlend})
        control->setCurrentAndTargetValue(control->getTargetValue());
    appliedHallBlend = hallBlend.getCurrentValue();
    nativeHall.setControls(roomSize.getTargetValue(), hallDecay.getCurrentValue(), hallHiCut.getCurrentValue(), hallLoCut.getCurrentValue(),
                           hallSpinRate.getCurrentValue(), hallSpinDepth.getCurrentValue());
    nativeHall.reset();
    preDelayLine.reset();
    for (auto& delay : networkDelays)
        delay.reset();

    std::fill(feedbackLowpassState.begin(), feedbackLowpassState.end(), 0.0f);
    std::fill(networkDelaySamples.begin(), networkDelaySamples.end(), 1.0f);

    roomSize.setCurrentAndTargetValue(roomSize.getTargetValue());
    nativeDecaySeconds.setCurrentAndTargetValue(nativeDecaySeconds.getTargetValue());
    nativeProfileBlend.setCurrentAndTargetValue(nativeProfileBlend.getTargetValue());
    damping.setCurrentAndTargetValue(damping.getTargetValue());
    wetLevel.setCurrentAndTargetValue(wetLevel.getTargetValue());
    width.setCurrentAndTargetValue(width.getTargetValue());
    highCutHz.setCurrentAndTargetValue(highCutHz.getTargetValue());
    lowCutHz.setCurrentAndTargetValue(lowCutHz.getTargetValue());
    preDelaySamples.setCurrentAndTargetValue(preDelaySamples.getTargetValue());

    for (auto& filter : outputHighPass)
        filter.reset();
    for (auto& filter : outputLowPass)
        filter.reset();
    for (auto& filter : feedbackHighPass)
        filter.reset();

    updateNetworkState(roomSize.getCurrentValue(), damping.getCurrentValue());
    updateOutputFilters(highCutHz.getCurrentValue(), lowCutHz.getCurrentValue());
    parameterQuantumSamplesRemaining = 0;
    appliedWidth = std::clamp(width.getCurrentValue(), 0.0f, 1.0f);
    const auto wet = std::clamp(wetLevel.getCurrentValue(), 0.0f, 1.0f);
    updateMix(wet);
}

void ReverbFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        nativeHall.reset();
        preDelayLine.reset();
        for (auto& delay : networkDelays)
            delay.reset();
        std::fill(feedbackLowpassState.begin(), feedbackLowpassState.end(), 0.0f);
        for (auto& filter : outputHighPass)
            filter.reset();
        for (auto& filter : outputLowPass)
            filter.reset();
        for (auto& filter : feedbackHighPass)
            filter.reset();
        parameterQuantumSamplesRemaining = 0;
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void ReverbFxCore::copyRuntimeStateFrom(const BaseFxProcessorCore& other)
{
    const auto* source = dynamic_cast<const ReverbFxCore*>(&other);
    if (source == nullptr)
        return;

    assert(std::abs(sampleRate - source->sampleRate) <= 1.0e-9);
    assert(numChannels == source->numChannels);
    if (std::abs(sampleRate - source->sampleRate) > 1.0e-9
        || numChannels != source->numChannels)
        return;

    nativeHall.reset();
    preDelayLine.reset();
    for (auto& delay : networkDelays)
        delay.reset();
    std::fill(feedbackLowpassState.begin(), feedbackLowpassState.end(), 0.0f);
    stereoInputSigns = source->stereoInputSigns;
    nativeWetOffset = source->nativeWetOffset;
    nativeDecayOffset = source->nativeDecayOffset;
    for (auto& filter : outputHighPass)
        filter.reset();
    for (auto& filter : outputLowPass)
        filter.reset();
    for (auto& filter : feedbackHighPass)
        filter.reset();
    const auto copyCurrentPreservingTarget = [](auto& target, const auto& sourceValue)
    {
        const auto targetValue = target.getTargetValue();
        target.setCurrentAndTargetValue(sourceValue.getCurrentValue());
        target.setTargetValue(targetValue);
    };
    copyCurrentPreservingTarget(hallDecay, source->hallDecay);
    copyCurrentPreservingTarget(hallHiCut, source->hallHiCut);
    copyCurrentPreservingTarget(hallLoCut, source->hallLoCut);
    copyCurrentPreservingTarget(hallSpinRate, source->hallSpinRate);
    copyCurrentPreservingTarget(hallSpinDepth, source->hallSpinDepth);
    copyCurrentPreservingTarget(hallBlend, source->hallBlend);
    appliedHallBlend = hallBlend.getCurrentValue();
    nativeHall.setControls(roomSize.getTargetValue(), hallDecay.getCurrentValue(), hallHiCut.getCurrentValue(), hallLoCut.getCurrentValue(),
                           hallSpinRate.getCurrentValue(), hallSpinDepth.getCurrentValue());
    copyCurrentPreservingTarget(roomSize, source->roomSize);
    copyCurrentPreservingTarget(nativeDecaySeconds, source->nativeDecaySeconds);
    copyCurrentPreservingTarget(nativeProfileBlend, source->nativeProfileBlend);
    copyCurrentPreservingTarget(damping, source->damping);
    copyCurrentPreservingTarget(wetLevel, source->wetLevel);
    copyCurrentPreservingTarget(width, source->width);
    copyCurrentPreservingTarget(highCutHz, source->highCutHz);
    copyCurrentPreservingTarget(lowCutHz, source->lowCutHz);
    copyCurrentPreservingTarget(preDelaySamples, source->preDelaySamples);
    feedbackGain = source->feedbackGain;
    dampingCoefficient = source->dampingCoefficient;
    updateNetworkState(roomSize.getCurrentValue(), damping.getCurrentValue());
    updateOutputFilters(highCutHz.getCurrentValue(), lowCutHz.getCurrentValue());
    parameterQuantumSamplesRemaining = 0;
    appliedWidth = std::clamp(width.getCurrentValue(), 0.0f, 1.0f);
    const auto wet = std::clamp(wetLevel.getCurrentValue(), 0.0f, 1.0f);
    updateMix(wet);
}

void ReverbFxCore::addNativeModulation(int target, float offset) noexcept
{
    if (target == 81 && std::isfinite(offset))
        nativeWetOffset = std::clamp(nativeWetOffset + offset, -32.0f, 32.0f);
    else if (target == 83 && std::isfinite(offset))
        nativeDecayOffset = std::clamp(nativeDecayOffset + offset, -32.0f, 32.0f);
}

void ReverbFxCore::setGenericParam(int index, float value01)
{
    switch (index)
    {
        case 0: snapshot.size       = FxCatalog::denormalizeParamValue(FxType::reverb, 0, value01); break;
        case 1: snapshot.damping    = FxCatalog::denormalizeParamValue(FxType::reverb, 1, value01); break;
        case 2: snapshot.wet        = FxCatalog::denormalizeParamValue(FxType::reverb, 2, value01); break;
        case 3: snapshot.width      = FxCatalog::denormalizeParamValue(FxType::reverb, 3, value01); break;
        case 4: snapshot.highCutHz  = FxCatalog::denormalizeParamValue(FxType::reverb, 4, value01); break;
        case 5: snapshot.lowCutHz   = FxCatalog::denormalizeParamValue(FxType::reverb, 5, value01); break;
        case 7:
            nativeDecaySeconds.setTargetValue(FxCatalog::denormalizeParamValue(FxType::reverb, 7, value01));
            nativeProfileBlend.setTargetValue(nativeDecaySeconds.getTargetValue() > 0.0f ? 1.0f : 0.0f);
            break;
        case 8: {
            const auto raw = FxCatalog::denormalizeParamValue(FxType::reverb, 8, value01);
            legacyHallDecayNormalized = fadmodel1::dsp::utils::sanitizeUnitInterval(value01);
            hallDecay.setTargetValue(raw >= 0.0f ? raw : 0.35f);
            hallBlend.setTargetValue(reverbUsesHall(modeNormalized, legacyHallDecayNormalized) ? 1.0f : 0.0f);
            break;
        }
        case 13:
            modeNormalized = fadmodel1::dsp::utils::sanitizeUnitInterval(value01);
            hallBlend.setTargetValue(reverbUsesHall(modeNormalized, legacyHallDecayNormalized) ? 1.0f : 0.0f);
            break;
        case 9: hallHiCut.setTargetValue(fadmodel1::dsp::utils::sanitizeUnitInterval(value01)); break;
        case 10: hallLoCut.setTargetValue(fadmodel1::dsp::utils::sanitizeUnitInterval(value01)); break;
        case 11: hallSpinRate.setTargetValue(fadmodel1::dsp::utils::sanitizeUnitInterval(value01)); break;
        case 12: hallSpinDepth.setTargetValue(fadmodel1::dsp::utils::sanitizeUnitInterval(value01)); break;
        case 14: extendedPreDelayNormalized = fadmodel1::dsp::utils::sanitizeUnitInterval(value01); break;
        case 6: snapshot.preDelayMs = FxCatalog::denormalizeParamValue(FxType::reverb, 6, value01); break;
        default: break;
    }
    setSnapshot(snapshot);
}

void ReverbFxCore::setSnapshot(const parameters::ReverbSnapshot& newSnapshot) noexcept
{
    snapshot = newSnapshot;
    roomSize.setTargetValue(fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.size, 0.5f, 0.0f, 1.0f));
    damping.setTargetValue(fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.damping, 0.5f, 0.0f, 1.0f));
    wetLevel.setTargetValue(fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.wet, 0.0f, 0.0f, 1.0f));
    width.setTargetValue(fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.width, 1.0f, 0.0f, 1.0f));
    highCutHz.setTargetValue(fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.highCutHz, 20000.0f, 1000.0f, 20000.0f));
    lowCutHz.setTargetValue(fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.lowCutHz, 20.0f, 20.0f, 4000.0f));
    updatePreDelayTarget();
}

void ReverbFxCore::updatePreDelayTarget() noexcept
{
    const auto legacyDelayMs = fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.preDelayMs, 0.0f, 0.0f, 100.0f);
    const auto useExtended = ! reverbUsesHall(modeNormalized, legacyHallDecayNormalized)
        && reverbHasExtendedPreDelay(extendedPreDelayNormalized);
    auto delayMs = useExtended ? reverbPreDelayMilliseconds(0.0f, extendedPreDelayNormalized) : legacyDelayMs;
    // Serum target 83 is Hall Decay or Plate Pre-Delay. Its offsets are in
    // the source knob domain: Plate milliseconds = 250 * knob^2. Leave an
    // unmodulated legacy value bit-exact instead of roundtripping sqrt/square.
    if (! reverbUsesHall(modeNormalized, legacyHallDecayNormalized) && nativeDecayOffset != 0.0f)
    {
        const auto knob = std::clamp(std::sqrt(delayMs / 250.0f) + nativeDecayOffset, 0.0f, 1.0f);
        delayMs = 250.0f * knob * knob;
    }
    const auto delaySmp = static_cast<float>(delayMs * 0.001 * sampleRate);
    preDelaySamples.setTargetValue(fadmodel1::dsp::utils::sanitizeFiniteClamped(
        delaySmp, 0.0f, 0.0f, static_cast<float>(sampleRate * maxPreDelaySeconds)));
}

void ReverbFxCore::refreshAppliedParameters()
{
    updatePreDelayTarget();
    const auto currentSize =
        fadmodel1::dsp::utils::sanitizeFiniteClamped(
            consumeSmoothedValue(roomSize, parameterUpdateBlockSize), 0.5f, 0.0f, 1.0f);
    const auto currentDamping =
        fadmodel1::dsp::utils::sanitizeFiniteClamped(
            consumeSmoothedValue(damping, parameterUpdateBlockSize), 0.5f, 0.0f, 1.0f);
    const auto currentWet =
        fadmodel1::dsp::utils::sanitizeFiniteClamped(
            consumeSmoothedValue(wetLevel, parameterUpdateBlockSize), 0.0f, 0.0f, 1.0f);
    appliedWidth =
        fadmodel1::dsp::utils::sanitizeFiniteClamped(
            consumeSmoothedValue(width, parameterUpdateBlockSize), 1.0f, 0.0f, 1.0f);
    const auto currentHiCut =
        fadmodel1::dsp::utils::sanitizeFiniteClamped(
            consumeSmoothedValue(highCutHz, parameterUpdateBlockSize), 20000.0f, 1000.0f, 20000.0f);
    const auto currentLoCut =
        fadmodel1::dsp::utils::sanitizeFiniteClamped(
            consumeSmoothedValue(lowCutHz, parameterUpdateBlockSize), 20.0f, 20.0f, 4000.0f);

    const auto previousHallBlend = appliedHallBlend;
    appliedHallBlend = consumeSmoothedValue(hallBlend, parameterUpdateBlockSize);
    // Retire history only once the crossfade has silenced that algorithm.
    // Both clears are bounded and leave the selected algorithm untouched.
    if (previousHallBlend < 1.0f && appliedHallBlend >= 1.0f)
        discardOriginalTail();
    if (previousHallBlend > 0.0f && appliedHallBlend <= 0.0f)
        nativeHall.reset();
    const auto currentHallDecay = std::clamp(consumeSmoothedValue(hallDecay, parameterUpdateBlockSize) + nativeDecayOffset, 0.0f, 1.0f);
    nativeHall.setControls(currentSize, currentHallDecay,
                           consumeSmoothedValue(hallHiCut, parameterUpdateBlockSize),
                           consumeSmoothedValue(hallLoCut, parameterUpdateBlockSize),
                           consumeSmoothedValue(hallSpinRate, parameterUpdateBlockSize),
                           consumeSmoothedValue(hallSpinDepth, parameterUpdateBlockSize));
    updateNetworkState(currentSize, currentDamping);
    updateOutputFilters(currentHiCut, currentLoCut);
    updateMix(currentWet);
}

void ReverbFxCore::updateMix(float wet) noexcept
{
    wet = std::clamp(wet + nativeWetOffset, 0.0f, 1.0f);
    const auto legacyWet = std::sqrt(wet);
    const auto legacyDry = std::sqrt(std::clamp(1.0f-wet, 0.0f, 1.0f));
    if (appliedNativeBlend <= 0.0f) {
        appliedWetGain = legacyWet; appliedDryGain = legacyDry;
    } else {
        const auto nativeWet = .5f - .5f * std::cos(std::numbers::pi_v<float> * wet);
        appliedWetGain = legacyWet + appliedNativeBlend * (nativeWet-legacyWet);
        appliedDryGain = legacyDry + appliedNativeBlend * (1.0f-nativeWet*nativeWet-legacyDry);
    }
}

void ReverbFxCore::updateNetworkState(float currentSize, float currentDamping) noexcept
{
    const auto sizeScale = fad::engine::math::interpolate(std::clamp(currentSize, 0.0f, 1.0f), 0.65f, 1.85f);
    for (size_t i = 0; i < networkDelaySamples.size(); ++i)
        networkDelaySamples[i] = std::max(1.0f, baseDelayMs[i] * sizeScale * 0.001f * static_cast<float>(sampleRate));

    feedbackGain = fad::engine::math::interpolate(std::clamp(currentSize, 0.0f, 1.0f), 0.62f, 0.92f);
    const auto decay = consumeSmoothedValue(nativeDecaySeconds, parameterUpdateBlockSize);
    appliedNativeBlend = consumeSmoothedValue(nativeProfileBlend, parameterUpdateBlockSize);
    for (size_t i = 0; i < nativeFeedbackGains.size(); ++i) {
        const auto nativeGain = decay > 0.0f
            ? std::exp(-6.90775528f * networkDelaySamples[i] / (static_cast<float>(sampleRate) * std::max(.05f, decay)))
            : feedbackGain;
        nativeFeedbackGains[i] = feedbackGain + appliedNativeBlend * (nativeGain-feedbackGain);
    }
    dampingCoefficient = fad::engine::math::interpolate(1.0f - std::clamp(currentDamping, 0.0f, 1.0f), 0.08f, 0.55f);
}

std::array<float, 2> ReverbFxCore::processNetworkSample(float leftIn, float rightIn, float currentWidth) noexcept
{
    std::array<float, networkLineCount> delayed{};
    float sumDelayed = 0.0f;
    for (size_t i = 0; i < delayed.size(); ++i)
    {
        float delayedSample;
        if (originalTailValidSamples >= 0) {
            // Mask each interpolation tap separately. Masking the entire
            // fractional read would drop the first fresh excitation sample.
            const auto whole = std::floor(networkDelaySamples[i]);
            const auto fraction = networkDelaySamples[i] - whole;
            auto first = networkDelays[i].popSample(0, whole, false);
            auto second = networkDelays[i].popSample(0, whole + 1.0f);
            if (static_cast<float>(originalTailValidSamples) < whole) first = 0.0f;
            if (static_cast<float>(originalTailValidSamples) < whole + 1.0f) second = 0.0f;
            delayedSample = first + fraction * (second - first);
        } else
            delayedSample = networkDelays[i].popSample(0, networkDelaySamples[i]);
        if (! std::isfinite(delayedSample))
            delayedSample = 0.0f;

        delayed[i] = delayedSample;
        sumDelayed += delayedSample;
    }

    const auto inputMid = 0.5f * (leftIn + rightIn);
    const auto inputSide = 0.5f * (leftIn - rightIn);
    const auto widthScale = fad::engine::math::interpolate(std::clamp(currentWidth, 0.0f, 1.0f), 0.15f, 1.0f);

    for (size_t i = 0; i < delayed.size(); ++i)
    {
        const auto mixedFeedback = delayed[i] - 0.25f * sumDelayed;
        feedbackLowpassState[i] += dampingCoefficient * (mixedFeedback - feedbackLowpassState[i]);
        if (! std::isfinite(feedbackLowpassState[i]))
            feedbackLowpassState[i] = 0.0f;
        else
            fad::engine::math::snapSmallState(feedbackLowpassState[i]);
        auto feedbackValue = feedbackHighPass[i].processSample(0, feedbackLowpassState[i]);
        feedbackHighPass[i].snapToZero();
        if (! std::isfinite(feedbackValue))
            feedbackValue = 0.0f;
        const auto excitation = inputMid * 0.18f + inputSide * stereoInputSigns[i] * 0.12f;
        const auto rawWrite = excitation + nativeFeedbackGains[i] * feedbackValue;
        const auto writeSample =
            std::isfinite(rawWrite) ? std::clamp(rawWrite, -4.0f, 4.0f) : 0.0f;
        networkDelays[i].pushSample(0, writeSample);
    }

    if (originalTailValidSamples >= 0
        && ++originalTailValidSamples > static_cast<int>(sampleRate * maxNetworkDelaySeconds) + 1)
        originalTailValidSamples = -1;

    float wetMid = 0.0f;
    float wetSide = 0.0f;
    for (size_t i = 0; i < delayed.size(); ++i)
    {
        wetMid += delayed[i];
        wetSide += delayed[i] * stereoInputSigns[i];
    }

    wetMid *= 0.125f;
    wetSide *= 0.125f * widthScale;

    return {wetMid + wetSide, wetMid - wetSide};
}

void ReverbFxCore::discardOriginalTail() noexcept
{
    originalTailValidSamples = 0;
    feedbackLowpassState.fill(0.0f);
    for (auto& filter : outputHighPass) filter.reset();
    for (auto& filter : outputLowPass) filter.reset();
    for (auto& filter : feedbackHighPass) filter.reset();
}

void ReverbFxCore::updateOutputFilters(float currentHiCutHz, float currentLoCutHz) noexcept
{
    const auto nyquistSafe = static_cast<float>(std::max(2000.0, sampleRate * 0.49));
    const auto hiCut = std::clamp(currentHiCutHz, 1000.0f, nyquistSafe);
    const auto loCut = std::clamp(currentLoCutHz, 20.0f, std::min(4000.0f, hiCut - 20.0f));

    for (auto& filter : outputHighPass)
    {
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setCutoffFrequency(loCut);
        filter.setResonance(0.707f);
    }
    for (auto& filter : outputLowPass)
    {
        filter.setType(fad::engine::filters::TptMode::lowpass);
        filter.setCutoffFrequency(hiCut);
        filter.setResonance(0.707f);
    }
}
}
