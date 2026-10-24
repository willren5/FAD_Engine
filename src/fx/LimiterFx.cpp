#include <fad/engine/fx/LimiterFx.h>
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
constexpr auto mixSmoothingSeconds = 0.02;
constexpr auto maxLookaheadMs = 5.0;

using utils::sanitizeFinite;
using utils::isFinite;
}

void LimiterFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));

    fixedOutputDelaySamples = static_cast<int>(std::lround(maxLookaheadMs * 0.001 * sampleRate));
    const auto maxLookaheadSamples = std::max(1, fixedOutputDelaySamples);
    lookaheadL.setMaximumDelayInSamples(maxLookaheadSamples);
    lookaheadR.setMaximumDelayInSamples(maxLookaheadSamples);
    detectorDelay.setMaximumDelayInSamples(maxLookaheadSamples);
    lookaheadL.prepare({sampleRate, spec.maximumBlockSize, 1u});
    lookaheadR.prepare({sampleRate, spec.maximumBlockSize, 1u});
    detectorDelay.prepare({sampleRate, spec.maximumBlockSize, 1u});

    ceilingDb.reset(sampleRate, paramSmoothingSeconds);
    releaseMs.reset(sampleRate, paramSmoothingSeconds);
    lookaheadMs.reset(sampleRate, paramSmoothingSeconds);
    kneeDb.reset(sampleRate, paramSmoothingSeconds);
    mix.reset(sampleRate, mixSmoothingSeconds);

    reset();
}

void LimiterFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
{
    if (bypassed.load(std::memory_order_relaxed))
        return;

    auto& outputBlock = context.getOutputBlock();
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());

    if (blockChannels <= 0 || blockChannels > numChannels || outputBlock.getNumSamples() == 0)
        return;

    processActiveBlock(context);
}

void LimiterFxCore::processActiveBlock(fad::engine::ProcessContextReplacing<float>& context)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());

    if (blockChannels <= 0
        || blockChannels > numChannels
        || numSamples <= 0)
        return;

    // Pre-compute release coefficient outside the loop when release is stable.
    const bool relSmoothing = releaseMs.isSmoothing();
    const auto srFloat = static_cast<float>(sampleRate);
    const auto fixedOutputDelay = static_cast<float>(fixedOutputDelaySamples);
    const auto initialReleaseMs = std::max(1.0f, sanitizeFinite(releaseMs.getTargetValue(), 100.0f));
    float cachedReleaseCoeff = std::exp(-1.0f / (initialReleaseMs * 0.001f * srFloat));

    const auto resetSmoothedParameters = [this]() noexcept
    {
        ceilingDb.setCurrentAndTargetValue(sanitizeFinite(ceilingDb.getTargetValue(), 0.0f));
        releaseMs.setCurrentAndTargetValue(std::max(1.0f, sanitizeFinite(releaseMs.getTargetValue(), 100.0f)));
        lookaheadMs.setCurrentAndTargetValue(
            std::clamp(sanitizeFinite(lookaheadMs.getTargetValue(), 0.0f), 0.0f, static_cast<float>(maxLookaheadMs)));
        kneeDb.setCurrentAndTargetValue(std::max(0.0f, sanitizeFinite(kneeDb.getTargetValue(), 0.0f)));
        mix.setCurrentAndTargetValue(std::clamp(sanitizeFinite(mix.getTargetValue(), 1.0f), 0.0f, 1.0f));
        envelope = 1.0f;
        holdCounter = 0;
        resetDelayLines();
    };

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        auto currentCeiling    = ceilingDb.getNextValue();
        auto currentRelease    = releaseMs.getNextValue();
        auto currentLookahead  = lookaheadMs.getNextValue();
        auto currentKnee       = kneeDb.getNextValue();
        auto currentMix        = mix.getNextValue();

        if (! isFinite(currentCeiling)
            || ! isFinite(currentRelease)
            || ! isFinite(currentLookahead)
            || ! isFinite(currentKnee)
            || ! isFinite(currentMix))
        {
            resetSmoothedParameters();
            currentCeiling = ceilingDb.getCurrentValue();
            currentRelease = releaseMs.getCurrentValue();
            currentLookahead = lookaheadMs.getCurrentValue();
            currentKnee = kneeDb.getCurrentValue();
            currentMix = mix.getCurrentValue();
        }

        currentCeiling = sanitizeFinite(currentCeiling, 0.0f);
        currentRelease = std::max(1.0f, sanitizeFinite(currentRelease, 100.0f));
        currentLookahead =
            std::clamp(sanitizeFinite(currentLookahead, 0.0f), 0.0f, static_cast<float>(maxLookaheadMs));
        currentKnee = std::max(0.0f, sanitizeFinite(currentKnee, 0.0f));
        currentMix = std::clamp(sanitizeFinite(currentMix, 1.0f), 0.0f, 1.0f);
        const auto dry               = 1.0f - currentMix;

        const auto requestedLookaheadSamples = std::clamp(static_cast<int>(std::lround(currentLookahead * 0.001f * srFloat)), 0, fixedOutputDelaySamples);
        const auto detectorDelaySamples = fixedOutputDelaySamples - requestedLookaheadSamples;
        const auto detectorDelayInSamples = static_cast<float>(detectorDelaySamples);
        const auto halfKnee     = currentKnee * 0.5f;
        if (relSmoothing)
            cachedReleaseCoeff = std::exp(-1.0f / (currentRelease * 0.001f * srFloat));
        const auto releaseCoeff = cachedReleaseCoeff;

        // --- Push input to lookahead delay and pop delayed samples ---
        const auto inputL = sanitizeFinite(outputBlock.getChannelPointer(0)[sampleIndex]);
        lookaheadL.pushSample(0, inputL);
        const auto delayedL = sanitizeFinite(lookaheadL.popSample(0, fixedOutputDelay));

        float inputR = 0.0f;
        float delayedR = 0.0f;

        if (blockChannels >= 2)
        {
            inputR = sanitizeFinite(outputBlock.getChannelPointer(1)[sampleIndex]);
            lookaheadR.pushSample(0, inputR);
            delayedR = sanitizeFinite(lookaheadR.popSample(0, fixedOutputDelay));
        }

        // --- Peak detection (linked stereo) ---
        const auto inputPeak = (blockChannels >= 2) ? std::max(std::abs(inputL), std::abs(inputR))
                                                    : std::abs(inputL);
        detectorDelay.pushSample(0, inputPeak);
        // The lead detector anticipates future program peaks. A later equal
        // or smaller peak may outlive the first peak's hold period, so the
        // sample actually playing must retain its own gain constraint too.
        // This uses the same musical gain law, without clipping the waveform.
        const auto programPeak = blockChannels >= 2
            ? std::max(std::abs(delayedL), std::abs(delayedR)) : std::abs(delayedL);
        const auto peak = std::max(programPeak,
            sanitizeFinite(detectorDelay.popSample(0, detectorDelayInSamples)));
        const auto peakDb = sanitizeFinite(fad::engine::math::gainToDecibels(peak + 1e-10f), currentCeiling);

        // --- Overshoot and soft knee ---
        const auto over = peakDb - currentCeiling;
        float reduction;

        if (over <= -halfKnee)
        {
            reduction = 0.0f;
        }
        else if (over >= halfKnee)
        {
            reduction = over;
        }
        else
        {
            const auto t = over + halfKnee;
            reduction = (t * t) / (2.0f * currentKnee + 1e-10f);
        }

        // --- Target gain ---
        const auto targetGain = sanitizeFinite(fad::engine::math::gainFromDecibels(-reduction), 1.0f);

        envelope = sanitizeFinite(envelope, 1.0f);

        // --- Envelope: instant attack, hold during lookahead, smooth release ---
        // D2 fix: hold envelope for the lookahead duration so gain reduction
        // stays aligned with the delayed signal instead of releasing early
        if (targetGain < envelope)
        {
            envelope = targetGain;
            holdCounter = std::max(1, requestedLookaheadSamples);
        }
        else if (holdCounter > 0)
        {
            --holdCounter;
            // envelope held — no release during lookahead period
        }
        else
        {
            envelope += (targetGain - envelope) * (1.0f - releaseCoeff);
        }
        fad::engine::math::snapSmallState(envelope);

        // --- Apply gain to delayed signal and mix ---
        const auto wetL = sanitizeFinite(delayedL * envelope);
        outputBlock.getChannelPointer(0)[sampleIndex] =
            sanitizeFinite(delayedL * dry + wetL * currentMix);

        if (blockChannels >= 2)
        {
            const auto wetR = sanitizeFinite(delayedR * envelope);
            outputBlock.getChannelPointer(1)[sampleIndex] =
                sanitizeFinite(delayedR * dry + wetR * currentMix);
        }
    }
}

void LimiterFxCore::reset()
{
    ceilingDb.setCurrentAndTargetValue(ceilingDb.getTargetValue());
    releaseMs.setCurrentAndTargetValue(releaseMs.getTargetValue());
    lookaheadMs.setCurrentAndTargetValue(lookaheadMs.getTargetValue());
    kneeDb.setCurrentAndTargetValue(kneeDb.getTargetValue());
    mix.setCurrentAndTargetValue(mix.getTargetValue());

    envelope = 1.0f;
    holdCounter = 0;
    resetDelayLines();
}

void LimiterFxCore::resetDelayLines() noexcept
{
    lookaheadL.reset();
    lookaheadR.reset();
    detectorDelay.reset();
}

void LimiterFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        envelope = 1.0f;
        holdCounter = 0;
        resetDelayLines();
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void LimiterFxCore::copyRuntimeStateFrom(const BaseFxProcessorCore& other)
{
    const auto* source = dynamic_cast<const LimiterFxCore*>(&other);
    if (source == nullptr)
        return;

    assert(std::abs(sampleRate - source->sampleRate) <= 1.0e-9);
    assert(numChannels == source->numChannels);
    if (std::abs(sampleRate - source->sampleRate) > 1.0e-9
        || numChannels != source->numChannels)
        return;

    resetDelayLines();
    envelope = source->envelope;
    holdCounter = source->holdCounter;
    const auto copyCurrentPreservingTarget = [](auto& target, const auto& sourceValue)
    {
        const auto targetValue = target.getTargetValue();
        target.setCurrentAndTargetValue(sourceValue.getCurrentValue());
        target.setTargetValue(targetValue);
    };
    copyCurrentPreservingTarget(ceilingDb, source->ceilingDb);
    copyCurrentPreservingTarget(releaseMs, source->releaseMs);
    copyCurrentPreservingTarget(lookaheadMs, source->lookaheadMs);
    copyCurrentPreservingTarget(kneeDb, source->kneeDb);
    copyCurrentPreservingTarget(mix, source->mix);
}

void LimiterFxCore::setGenericParam(int index, float value01)
{
    const auto safeValue01 = utils::sanitizeUnitInterval(value01);

    switch (index)
    {
        case 0: // Ceiling (-12 to 0 dB)
            ceilingDb.setTargetValue(fad::engine::math::interpolate(safeValue01, -12.0f, 0.0f));
            break;
        case 1: // Release (10-500 ms, skew 0.3)
            releaseMs.setTargetValue(
                fad::engine::math::RuntimeRange(10.0f, 500.0f, 0.0f, 0.3f).denormalise(safeValue01));
            break;
        case 2: // Lookahead (0-5 ms)
            lookaheadMs.setTargetValue(fad::engine::math::interpolate(safeValue01, 0.0f, 5.0f));
            break;
        case 3: // Knee (0-6 dB)
            kneeDb.setTargetValue(fad::engine::math::interpolate(safeValue01, 0.0f, 6.0f));
            break;
        case 4: // Mix (0-1)
            mix.setTargetValue(safeValue01);
            break;
        default: break;
    }
}
}
