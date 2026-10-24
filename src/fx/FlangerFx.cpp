#include <fad/engine/fx/WideFxControls.h>
#include <fad/engine/fx/FlangerFx.h>
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
constexpr auto paramSmoothingSeconds = 0.03;
constexpr int filterUpdateIntervalSamples = 16;
constexpr int modulationUpdateIntervalSamples = 16;

float sanitizeSmoothedValue(float value, float fallback, float minimum, float maximum) noexcept
{
    return utils::sanitizeFiniteClamped(value, fallback, minimum, maximum);
}

float sanitizeCutoffValue(float value, float fallback) noexcept
{
    return utils::sanitizeFinite(value, fallback);
}
}

void FlangerFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    dryBuffer.setSize(numChannels, static_cast<int>(spec.maximumBlockSize), false, false, true);

    const auto maxDelaySamples = static_cast<int>(std::ceil(0.01 * sampleRate)) + 1;

    for (auto& dl : delayLines)
    {
        dl.setMaximumDelayInSamples(maxDelaySamples);
        dl.prepare({sampleRate, spec.maximumBlockSize, 1u});
    }

    for (auto& filter : outputHP)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& filter : outputLP)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& filter : feedbackDcBlockers)
    {
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setCutoffFrequency(5.0f);
        filter.setResonance(0.707f);
    }

    rate.reset(sampleRate, paramSmoothingSeconds);
    depth.reset(sampleRate, paramSmoothingSeconds);
    manualMs.reset(sampleRate, paramSmoothingSeconds);
    feedback.reset(sampleRate, paramSmoothingSeconds);
    mix.reset(sampleRate, paramSmoothingSeconds);
    nativeMixBlend.reset(sampleRate, .005);
    hiCutHz.reset(sampleRate, paramSmoothingSeconds);
    loCutHz.reset(sampleRate, paramSmoothingSeconds);

    reset();
}

void FlangerFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
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

void FlangerFxCore::processActiveBlock(fad::engine::ProcessContextReplacing<float>& context)
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

    const auto maxDelaySamples = static_cast<float>(std::ceil(0.01 * sampleRate));
    const auto resetSmoothedParameters = [this]() noexcept
    {
        const auto resetToFiniteTarget = [] (auto& smoother,
                                             float fallback,
                                             float minimum,
                                             float maximum) noexcept
        {
            const auto target = smoother.getTargetValue();
            smoother.setCurrentAndTargetValue(std::isfinite(target)
                ? std::clamp(target, minimum, maximum)
                : fallback);
        };

        resetToFiniteTarget(rate, 0.0f, 0.0f, 50.0f);
        resetToFiniteTarget(depth, 0.0f, 0.0f, 1.0f);
        resetToFiniteTarget(manualMs, 0.1f, 0.1f, 10.0f);
        resetToFiniteTarget(feedback, 0.0f, -0.95f, 0.95f);
        resetToFiniteTarget(mix, 0.0f, 0.0f, 1.0f);
        resetToFiniteTarget(hiCutHz, 20000.0f, 1000.0f, 20000.0f);
        resetToFiniteTarget(loCutHz, 20.0f, 20.0f, 4000.0f);
    };

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto rawRate = rate.getNextValue();
        const auto rawDepth = depth.getNextValue();
        const auto rawManualMs = manualMs.getNextValue();
        const auto rawFeedback = feedback.getNextValue();
        const auto nativeBlend = nativeMixBlend.getNextValue();
        const auto rawMix = mix.getNextValue() + nativeBlend * nativeWetModulation.next();
        const auto rawHiCut = hiCutHz.getNextValue();
        const auto rawLoCut = loCutHz.getNextValue();
        if (! std::isfinite(rawRate)
            || ! std::isfinite(rawDepth)
            || ! std::isfinite(rawManualMs)
            || ! std::isfinite(rawFeedback)
            || ! std::isfinite(rawMix)
            || ! std::isfinite(rawHiCut)
            || ! std::isfinite(rawLoCut))
        {
            resetSmoothedParameters();
        }

        const auto currentRate = sanitizeSmoothedValue(rawRate, 0.0f, 0.0f, 50.0f);
        const auto currentDepth = sanitizeSmoothedValue(rawDepth, 0.0f, 0.0f, 1.0f);
        const auto currentManualMs = sanitizeSmoothedValue(rawManualMs, 0.1f, 0.1f, 10.0f);
        const auto currentFeedback = sanitizeSmoothedValue(rawFeedback, 0.0f, -0.95f, 0.95f);
        const auto currentMix = sanitizeSmoothedValue(rawMix, 0.0f, 0.0f, 1.0f);
        const auto currentHiCut = sanitizeCutoffValue(rawHiCut, 20000.0f);
        const auto currentLoCut = sanitizeCutoffValue(rawLoCut, 20.0f);
        if (filterUpdateCountdown <= 0)
        {
            updateOutputFilters(currentLoCut, currentHiCut);
            filterUpdateCountdown = filterUpdateIntervalSamples;
        }
        --filterUpdateCountdown;

        if (modulationUpdateCountdown <= 0)
        {
            heldLfoValues[0] = std::sin((2 * std::numbers::pi_v<float>) * (wideRateHz>=0?static_cast<float>(wideLfoPhase):lfoPhase));
            heldLfoValues[1] = std::sin((2 * std::numbers::pi_v<float>) * (wideRateHz>=0?static_cast<float>(wideLfoPhase):lfoPhase)
                                        + std::numbers::pi_v<float>);
            modulationUpdateCountdown = modulationUpdateIntervalSamples;
        }
        --modulationUpdateCountdown;

        if(wideRateHz>=0) {
            wideLfoPhase+=static_cast<double>(currentRate)/sampleRate;
            if(wideLfoPhase>=1)wideLfoPhase-=std::floor(wideLfoPhase);
        } else {
            lfoPhase += currentRate / static_cast<float>(sampleRate);
            while (lfoPhase >= 1.0f)lfoPhase -= 1.0f;
        }

        for (int channel = 0; channel < blockChannels; ++channel)
        {
            const auto ch = static_cast<size_t>(channel);
            const auto lfoVal = heldLfoValues[ch];
            const auto delayMs = currentManualMs + currentDepth * currentManualMs * lfoVal;
            const auto delaySamples = std::clamp(delayMs * 0.001f * static_cast<float>(sampleRate), 0.5f, maxDelaySamples);

            const auto drySample = dryBuffer.getSample(channel, sampleIndex);
            const auto input = drySample + feedbackSample[ch] * currentFeedback;
            if (! std::isfinite(drySample) || ! std::isfinite(input))
            {
                delayLines[ch].reset();
                feedbackDcBlockers[ch].reset();
                outputHP[ch].reset();
                outputLP[ch].reset();
                feedbackSample[ch] = 0.0f;
                outputBlock.getChannelPointer(ch)[sampleIndex] =
                    std::isfinite(drySample) ? drySample : 0.0f;
                continue;
            }

            delayLines[ch].pushSample(0, input);
            const auto delayed = delayLines[ch].popSample(0, delaySamples);
            if (! std::isfinite(delayed))
            {
                delayLines[ch].reset();
                feedbackDcBlockers[ch].reset();
                feedbackSample[ch] = 0.0f;
                outputBlock.getChannelPointer(ch)[sampleIndex] = drySample;
                continue;
            }

            feedbackSample[ch] = feedbackDcBlockers[ch].processSample(0, delayed);
            fad::engine::math::snapSmallState(feedbackSample[ch]);
            if (! std::isfinite(feedbackSample[ch]))
            {
                feedbackDcBlockers[ch].reset();
                feedbackSample[ch] = 0.0f;
            }

            // Output EQ
            auto wet = outputHP[ch].processSample(0, delayed);
            wet = outputLP[ch].processSample(0, wet);
            if (! std::isfinite(wet))
            {
                outputHP[ch].reset();
                outputLP[ch].reset();
                wet = 0.0f;
            }

            if (nativeBlend > 0.0f)
            {
                const auto w = serumModulationFxWetGain(currentMix);
                const auto dryGain = (1.0f-currentMix) + nativeBlend*(currentMix-w*w);
                const auto wetGain = currentMix + nativeBlend*(w-currentMix);
                outputBlock.getChannelPointer(ch)[sampleIndex] = drySample*dryGain + wet*wetGain;
            }
            else
                outputBlock.getChannelPointer(ch)[sampleIndex] =
                    drySample * (1.0f - currentMix) + wet * currentMix;
        }
    }
    if (numSamples > 0) hasProcessedSample = true;
}

void FlangerFxCore::reset()
{
    nativeWetModulation.reset(); hasProcessedSample = false;
    nativeMixBlend.setCurrentAndTargetValue(nativeMixEnabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
    lfoPhase = 0.0f; wideLfoPhase=0;
    feedbackSample.fill(0.0f);
    heldLfoValues.fill(0.0f);
    modulationUpdateCountdown = 0;

    rate.setCurrentAndTargetValue(rate.getTargetValue());
    depth.setCurrentAndTargetValue(depth.getTargetValue());
    manualMs.setCurrentAndTargetValue(manualMs.getTargetValue());
    feedback.setCurrentAndTargetValue(feedback.getTargetValue());
    mix.setCurrentAndTargetValue(mix.getTargetValue());
    hiCutHz.setCurrentAndTargetValue(hiCutHz.getTargetValue());
    loCutHz.setCurrentAndTargetValue(loCutHz.getTargetValue());

    for (auto& dl : delayLines)
        dl.reset();

    for (auto& filter : outputHP)
        filter.reset();

    for (auto& filter : outputLP)
        filter.reset();

    for (auto& filter : feedbackDcBlockers)
        filter.reset();

    updateOutputFilters(loCutHz.getCurrentValue(), hiCutHz.getCurrentValue());
    filterUpdateCountdown = filterUpdateIntervalSamples;
}

void FlangerFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        nativeWetModulation.reset(); hasProcessedSample = false;
        nativeMixBlend.setCurrentAndTargetValue(nativeMixEnabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
        for (auto& dl : delayLines)
            dl.reset();
        for (auto& filter : outputHP)
            filter.reset();
        for (auto& filter : outputLP)
            filter.reset();
        for (auto& filter : feedbackDcBlockers)
            filter.reset();
        feedbackSample.fill(0.0f);
        lfoPhase = 0.0f; wideLfoPhase=0;
        heldLfoValues.fill(0.0f);
        modulationUpdateCountdown = 0;
        filterUpdateCountdown = 0;
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void FlangerFxCore::copyRuntimeStateFrom(const BaseFxProcessorCore& other)
{
    const auto* source = dynamic_cast<const FlangerFxCore*>(&other);
    if (source == nullptr)
        return;

    assert(std::abs(sampleRate - source->sampleRate) <= 1.0e-9);
    assert(numChannels == source->numChannels);
    if (std::abs(sampleRate - source->sampleRate) > 1.0e-9
        || numChannels != source->numChannels)
        return;

    delayLines = source->delayLines;
    feedbackDcBlockers = source->feedbackDcBlockers;
    lfoPhase = source->lfoPhase; wideLfoPhase=source->wideLfoPhase;
    feedbackSample = source->feedbackSample;
    heldLfoValues = source->heldLfoValues;
    modulationUpdateCountdown = source->modulationUpdateCountdown;
    filterUpdateCountdown = source->filterUpdateCountdown;
    outputHP = source->outputHP;
    outputLP = source->outputLP;

    const auto copyCurrentPreservingTarget = [](auto& target, const auto& sourceValue)
    {
        const auto targetValue = target.getTargetValue();
        target.setCurrentAndTargetValue(sourceValue.getCurrentValue());
        target.setTargetValue(targetValue);
    };
    copyCurrentPreservingTarget(rate, source->rate);
    copyCurrentPreservingTarget(depth, source->depth);
    copyCurrentPreservingTarget(manualMs, source->manualMs);
    copyCurrentPreservingTarget(feedback, source->feedback);
    copyCurrentPreservingTarget(mix, source->mix);
    copyCurrentPreservingTarget(nativeMixBlend, source->nativeMixBlend);
    nativeWetModulation = source->nativeWetModulation;
    hasProcessedSample = source->hasProcessedSample;
    copyCurrentPreservingTarget(hiCutHz, source->hiCutHz);
    copyCurrentPreservingTarget(loCutHz, source->loCutHz);

    const auto channels = std::min(dryBuffer.getNumChannels(), source->dryBuffer.getNumChannels());
    const auto samples = std::min(dryBuffer.getNumSamples(), source->dryBuffer.getNumSamples());
    for (int channel = 0; channel < channels; ++channel)
        std::copy_n(source->dryBuffer.getReadPointer(channel), samples, dryBuffer.getWritePointer(channel));
}

void FlangerFxCore::setGenericParam(int index, float value01)
{
    if(index==26) {
        const auto next=wideFxDecode(index,value01);
        if(wideRateHz<0 && next>=0)wideLfoPhase=lfoPhase;
        if(wideRateHz>=0 && next<0)lfoPhase=static_cast<float>(wideLfoPhase);
        wideRateHz=next;rate.setTargetValue(wideRateHz>=0?wideRateHz:legacyRateHz);return;
    }
    const auto safeValue01 = utils::sanitizeUnitInterval(value01);

    switch (index)
    {
        case 0: // Rate (0.01-10 Hz, skew 0.3)
            legacyRateHz=fad::engine::math::RuntimeRange(0.01f,10.0f,0.0f,.3f).denormalise(safeValue01);
            rate.setTargetValue(wideRateHz>=0?wideRateHz:legacyRateHz);
            break;
        case 1: // Depth (0-1)
            depth.setTargetValue(safeValue01);
            break;
        case 2: // Manual (0.1-10 ms, skew 0.3)
            manualMs.setTargetValue(
                fad::engine::math::RuntimeRange(0.1f, 10.0f, 0.0f, 0.3f).denormalise(safeValue01));
            break;
        case 3: // Feedback (-0.95 to +0.95)
            feedback.setTargetValue(fad::engine::math::interpolate(safeValue01, -0.95f, 0.95f));
            break;
        case 4: // Mix (0-1)
            mix.setTargetValue(safeValue01);
            break;
        case 5: // Hi Cut (1000-20000 Hz, skew 0.3)
            hiCutHz.setTargetValue(
                fad::engine::math::RuntimeRange(1000.0f, 20000.0f, 0.0f, 0.3f).denormalise(safeValue01));
            break;
        case 6: // Lo Cut (20-4000 Hz, skew 0.3)
            loCutHz.setTargetValue(
                fad::engine::math::RuntimeRange(20.0f, 4000.0f, 0.0f, 0.3f).denormalise(safeValue01));
            break;
        case 7:
            nativeMixEnabled.store(safeValue01 >= .5f, std::memory_order_relaxed);
            if (hasProcessedSample) nativeMixBlend.setTargetValue(safeValue01 >= .5f ? 1.0f : 0.0f);
            else nativeMixBlend.setCurrentAndTargetValue(safeValue01 >= .5f ? 1.0f : 0.0f);
            break;
        default: break;
    }
}

void FlangerFxCore::updateOutputFilters(float currentLoCutHz, float currentHiCutHz) noexcept
{
    currentLoCutHz = sanitizeCutoffValue(currentLoCutHz, 20.0f);
    currentHiCutHz = sanitizeCutoffValue(currentHiCutHz, 20000.0f);

    const auto nyquistSafe =
        static_cast<float>(std::max(2000.0, sampleRate * 0.49));
    const auto currentHiCut = std::clamp(currentHiCutHz, 100.0f, nyquistSafe);
    const auto currentLoCut = std::clamp(currentLoCutHz, 20.0f, currentHiCut - 20.0f);

    for (auto& filter : outputHP)
    {
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setCutoffFrequency(currentLoCut);
        filter.setResonance(0.707f);
    }

    for (auto& filter : outputLP)
    {
        filter.setType(fad::engine::filters::TptMode::lowpass);
        filter.setCutoffFrequency(currentHiCut);
        filter.setResonance(0.707f);
    }
}
}
