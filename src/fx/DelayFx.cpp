#include <fad/engine/fx/WideFxControls.h>
#include <fad/engine/fx/DelayFx.h>
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
constexpr auto maxDelaySeconds = 60.0;
constexpr auto parameterSmoothingSeconds = 0.04;
constexpr int filterUpdateIntervalSamples = 16;

using utils::isFinite;

void resetFilters(std::array<fad::engine::filters::TptStateVariable<float>, 2>& filters) noexcept
{
    for (auto& filter : filters)
        filter.reset();
}
}

void DelayFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    delayLine.setMaximumDelayInSamples(static_cast<int>(std::ceil(maxDelaySeconds * sampleRate)));
    delayLine.prepare({sampleRate, spec.maximumBlockSize, static_cast<uint32_t>(numChannels)});
    feedbackAmount.reset(sampleRate, parameterSmoothingSeconds);
    wetMix.reset(sampleRate, parameterSmoothingSeconds);
    delaySamples.reset(sampleRate, parameterSmoothingSeconds);
    rightDelaySamples.reset(sampleRate, parameterSmoothingSeconds);
    pingPongMix.reset(sampleRate, parameterSmoothingSeconds);
    nativeFilterFrequency.reset(sampleRate, parameterSmoothingSeconds);
    nativeFilterBandwidth.reset(sampleRate, parameterSmoothingSeconds);
    nativeProfileMix.reset(sampleRate, .005);
    highCutHz.reset(sampleRate, parameterSmoothingSeconds);
    lowCutHz.reset(sampleRate, parameterSmoothingSeconds);

    for (auto& filter : feedbackHighPass)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& filter : feedbackLowPass)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& filter : outputHighPass)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& filter : outputLowPass)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    initialiseFilterTypes();
    reset();
}

void DelayFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
{
    if (bypassed.load(std::memory_order_relaxed))
        return;

    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());
    if (blockChannels <= 0 || blockChannels > numChannels)
        return;

    const auto maximumDelaySamples =
        static_cast<float>(delayLine.getMaximumDelayInSamples());
    const auto resetSmoothedParameters = [this]() noexcept
    {
        const auto resetToFiniteTarget = [] (auto& smoother, float fallback) noexcept
        {
            const auto target = smoother.getTargetValue();
            smoother.setCurrentAndTargetValue(isFinite(target) ? target : fallback);
        };

        resetToFiniteTarget(feedbackAmount, 0.0f);
        resetToFiniteTarget(wetMix, 0.0f);
        resetToFiniteTarget(delaySamples, 0.0f);
        resetToFiniteTarget(rightDelaySamples, 0.0f);
        resetToFiniteTarget(pingPongMix, 0.0f);
        resetToFiniteTarget(nativeFilterFrequency,0.0f);
        resetToFiniteTarget(nativeFilterBandwidth,0.0f);
        resetToFiniteTarget(nativeProfileMix,0.0f);
        resetToFiniteTarget(lowCutHz, 20.0f);
        resetToFiniteTarget(highCutHz, 20000.0f);
    };

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        if (filterUpdateCountdown <= 0) appliedNativeWetOffset = nativeWetOffset;
        const auto rawDelaySamples = delaySamples.getNextValue();
        const auto rawRightDelaySamples = rightDelaySamples.getNextValue();
        const auto pingPong = pingPongMix.getNextValue();
        const auto currentFeedback = feedbackAmount.getNextValue();
        const auto currentMix = std::clamp(wetMix.getNextValue() + appliedNativeWetOffset, 0.0f, 1.0f);
        const auto currentLowCut = lowCutHz.getNextValue();
        const auto currentHighCut = highCutHz.getNextValue();
        const auto nativeBlend = nativeProfileMix.getNextValue();
        const auto nativeFrequency = nativeFilterFrequency.getNextValue();
        const auto nativeBandwidth = nativeFilterBandwidth.getNextValue();
        if (! isFinite(rawDelaySamples) || ! isFinite(rawRightDelaySamples)
            || ! isFinite(pingPong)
            || ! isFinite(currentFeedback)
            || ! isFinite(currentMix)
            || ! isFinite(currentLowCut)
            || ! isFinite(currentHighCut) || !isFinite(nativeBlend)
            || !isFinite(nativeFrequency) || !isFinite(nativeBandwidth))
        {
            resetSmoothedParameters();
            const auto safeLeft = outputBlock.getNumChannels() > 0
                ? outputBlock.getChannelPointer(0)[sampleIndex]
                : 0.0f;
            const auto safeRight = outputBlock.getNumChannels() > 1
                ? outputBlock.getChannelPointer(1)[sampleIndex]
                : 0.0f;
            recoverFromInvalidSample(outputBlock, sampleIndex, safeLeft, safeRight, false);
            continue;
        }

        const auto currentDelaySamples =
            std::clamp(rawDelaySamples, 0.0f, maximumDelaySamples);
        if (filterUpdateCountdown <= 0)
        {
            updateFilters(currentLowCut, currentHighCut);
            if(nativeBlend>0)updateNativeFilter(nativeFrequency,nativeBandwidth);
            filterUpdateCountdown = filterUpdateIntervalSamples;
        }
        --filterUpdateCountdown;
        auto dry = 1.0f - currentMix;
        auto wetGain = currentMix;
        if(nativeBlend>0) {
            if(nativeMixInput!=currentMix) {
                nativeMixInput=currentMix;
                nativeWetGain=.5f-.5f*std::cos(std::numbers::pi_v<float>*currentMix);
            }
            dry += nativeBlend*((1.0f-nativeWetGain*nativeWetGain)-dry);
            wetGain += nativeBlend*(nativeWetGain-wetGain);
        }

        if (blockChannels == 1)
        {
            const auto input = outputBlock.getChannelPointer(0)[sampleIndex];
            const auto delayed = popValidDelaySample(0, currentDelaySamples)
                + (nativeBlend>0 && currentDelaySamples<1.0f ? input*(1.0f-currentDelaySamples) : 0.0f);
            if (! isFinite(input) || ! isFinite(delayed))
            {
                recoverFromInvalidSample(outputBlock,
                                         sampleIndex,
                                         isFinite(input) ? input : 0.0f,
                                         0.0f,
                                         true);
                continue;
            }
            auto wetSample = outputHighPass[0].processSample(0, delayed);
            wetSample = outputLowPass[0].processSample(0, wetSample);
            auto feedbackSample = feedbackHighPass[0].processSample(0, delayed);
            feedbackSample = feedbackLowPass[0].processSample(0, feedbackSample);
            if(nativeBlend>0) {
                const auto nativeWet=processNativeFilter(0,delayed);
                wetSample += nativeBlend*(nativeWet-wetSample);
                feedbackSample += nativeBlend*(nativeWet-feedbackSample);
            }
            fad::engine::math::snapSmallState(wetSample);
            fad::engine::math::snapSmallState(feedbackSample);
            if (! isFinite(wetSample) || ! isFinite(feedbackSample))
            {
                recoverFromInvalidSample(outputBlock,
                                         sampleIndex,
                                         isFinite(input) ? input : 0.0f,
                                         0.0f,
                                         true);
                continue;
            }
            delayLine.pushSample(0, input + feedbackSample * currentFeedback);
            advanceValidDelayHistory();
            outputBlock.getChannelPointer(0)[sampleIndex] = input * dry + wetSample * wetGain;
            continue;
        }

        const auto leftInput = outputBlock.getChannelPointer(0)[sampleIndex];
        const auto rightInput = outputBlock.getChannelPointer(1)[sampleIndex];
        // Feed one side with the stereo average; cross-feedback then alternates
        // echoes even for centred input. Dry stereo and legacy routing stay intact.
        const auto monoInput = 0.5f * leftInput + 0.5f * rightInput;
        const auto delayInputLeft = pingPong > 0.0f
            ? (1.0f - pingPong) * leftInput + pingPong * monoInput : leftInput;
        const auto delayInputRight = pingPong > 0.0f
            ? (1.0f - pingPong) * rightInput : rightInput;
        const auto leftDelayed = popValidDelaySample(0, currentDelaySamples)
            + (nativeBlend>0 && currentDelaySamples<1.0f ? delayInputLeft*(1.0f-currentDelaySamples) : 0.0f);
        const auto rightTime=std::clamp(rawRightDelaySamples, 0.0f, maximumDelaySamples);
        const auto rightDelayed = popValidDelaySample(1,rightTime)
            + (nativeBlend>0 && rightTime<1.0f ? delayInputRight*(1.0f-rightTime) : 0.0f);
        if (! isFinite(leftInput) || ! isFinite(rightInput)
            || ! isFinite(leftDelayed) || ! isFinite(rightDelayed))
        {
            recoverFromInvalidSample(outputBlock,
                                     sampleIndex,
                                     isFinite(leftInput) ? leftInput : 0.0f,
                                     isFinite(rightInput) ? rightInput : 0.0f,
                                     true);
            continue;
        }

        auto wetLeft = outputHighPass[0].processSample(0, leftDelayed);
        wetLeft = outputLowPass[0].processSample(0, wetLeft);
        auto wetRight = outputHighPass[1].processSample(0, rightDelayed);
        wetRight = outputLowPass[1].processSample(0, wetRight);
        const auto sameChannel = sameChannelFeedback ? 1.0f - pingPong : 0.0f;
        const auto routeLeft = [sameChannel](float left, float right) noexcept {
            return sameChannel > 0.0f ? sameChannel * left + (1.0f - sameChannel) * right : right;
        };
        auto feedbackToLeft = feedbackHighPass[0].processSample(0, routeLeft(leftDelayed, rightDelayed));
        feedbackToLeft = feedbackLowPass[0].processSample(0, feedbackToLeft);
        auto feedbackToRight = feedbackHighPass[1].processSample(0, routeLeft(rightDelayed, leftDelayed));
        feedbackToRight = feedbackLowPass[1].processSample(0, feedbackToRight);
        if(nativeBlend>0) {
            const auto nativeLeft=processNativeFilter(0,leftDelayed);
            const auto nativeRight=processNativeFilter(1,rightDelayed);
            wetLeft += nativeBlend*(nativeLeft-wetLeft);
            wetRight += nativeBlend*(nativeRight-wetRight);
            feedbackToLeft += nativeBlend*(routeLeft(nativeLeft,nativeRight)-feedbackToLeft);
            feedbackToRight += nativeBlend*(routeLeft(nativeRight,nativeLeft)-feedbackToRight);
        }
        fad::engine::math::snapSmallState(wetLeft);
        fad::engine::math::snapSmallState(wetRight);
        fad::engine::math::snapSmallState(feedbackToLeft);
        fad::engine::math::snapSmallState(feedbackToRight);
        if (! isFinite(wetLeft) || ! isFinite(wetRight)
            || ! isFinite(feedbackToLeft) || ! isFinite(feedbackToRight))
        {
            recoverFromInvalidSample(outputBlock, sampleIndex, leftInput, rightInput, true);
            continue;
        }

        delayLine.pushSample(0, delayInputLeft + feedbackToLeft * currentFeedback);
        delayLine.pushSample(1, delayInputRight + feedbackToRight * currentFeedback);
        advanceValidDelayHistory();

        outputBlock.getChannelPointer(0)[sampleIndex] = leftInput * dry + wetLeft * wetGain;
        outputBlock.getChannelPointer(1)[sampleIndex] = rightInput * dry + wetRight * wetGain;
    }
}

void DelayFxCore::recoverFromInvalidSample(fad::engine::AudioBlock<float>& outputBlock,
                                       int sampleIndex,
                                       float safeLeft,
                                       float safeRight,
                                       bool delayReadAdvanced) noexcept
{
    invalidateDelayHistory();
    resetFilters(feedbackHighPass);
    resetFilters(feedbackLowPass);
    resetFilters(outputHighPass);
    resetFilters(outputLowPass);
    nativeFilterStates={};

    if (delayReadAdvanced && outputBlock.getNumChannels() > 0)
    {
        delayLine.pushSample(0, isFinite(safeLeft) ? safeLeft : 0.0f);
        if (outputBlock.getNumChannels() > 1)
            delayLine.pushSample(1, isFinite(safeRight) ? safeRight : 0.0f);
        advanceValidDelayHistory();
    }

    if (outputBlock.getNumChannels() <= 0)
        return;

    outputBlock.getChannelPointer(0)[sampleIndex] = isFinite(safeLeft) ? safeLeft : 0.0f;
    if (outputBlock.getNumChannels() > 1)
        outputBlock.getChannelPointer(1)[sampleIndex] = isFinite(safeRight) ? safeRight : 0.0f;
}

void DelayFxCore::addNativeModulation(int target, float offset) noexcept
{
    if (target == 124 && std::isfinite(offset))
        nativeWetOffset = std::clamp(nativeWetOffset + offset, -32.0f, 32.0f);
}

void DelayFxCore::reset()
{
    nativeWetOffset = appliedNativeWetOffset = 0.0f;
    invalidateDelayHistory();
    feedbackAmount.setCurrentAndTargetValue(feedbackAmount.getTargetValue());
    wetMix.setCurrentAndTargetValue(wetMix.getTargetValue());
    delaySamples.setCurrentAndTargetValue(delaySamples.getTargetValue());
    rightDelaySamples.setCurrentAndTargetValue(rightDelaySamples.getTargetValue());
    pingPongMix.setCurrentAndTargetValue(pingPongMix.getTargetValue());
    nativeFilterFrequency.setCurrentAndTargetValue(nativeFilterFrequency.getTargetValue());
    nativeFilterBandwidth.setCurrentAndTargetValue(nativeFilterBandwidth.getTargetValue());
    nativeProfileMix.setCurrentAndTargetValue(nativeProfileMix.getTargetValue());
    highCutHz.setCurrentAndTargetValue(highCutHz.getTargetValue());
    lowCutHz.setCurrentAndTargetValue(lowCutHz.getTargetValue());

    resetFilters(feedbackHighPass);
    resetFilters(feedbackLowPass);
    resetFilters(outputHighPass);
    resetFilters(outputLowPass);
    nativeFilterStates={};

    initialiseFilterTypes();
    updateFilters(lowCutHz.getTargetValue(), highCutHz.getTargetValue());
    filterUpdateCountdown = 0;
}

void DelayFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        invalidateDelayHistory();
        resetFilters(feedbackHighPass);
        resetFilters(feedbackLowPass);
        resetFilters(outputHighPass);
        resetFilters(outputLowPass);
        nativeFilterStates={};
        filterUpdateCountdown = 0;
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void DelayFxCore::copyRuntimeStateFrom(const BaseFxProcessorCore& other)
{
    const auto* source = dynamic_cast<const DelayFxCore*>(&other);
    if (source == nullptr)
        return;

    assert(std::abs(sampleRate - source->sampleRate) <= 1.0e-9);
    assert(numChannels == source->numChannels);
    if (std::abs(sampleRate - source->sampleRate) > 1.0e-9
        || numChannels != source->numChannels)
    {
        return;
    }

    delayLine = source->delayLine;
    validDelaySamples = source->validDelaySamples;
    feedbackHighPass = source->feedbackHighPass;
    feedbackLowPass = source->feedbackLowPass;
    outputHighPass = source->outputHighPass;
    outputLowPass = source->outputLowPass;
    filterUpdateCountdown = source->filterUpdateCountdown;
    nativeFilterStates=source->nativeFilterStates;
    nativeB0=source->nativeB0;nativeA1=source->nativeA1;nativeA2=source->nativeA2;
    nativeLastFrequency=source->nativeLastFrequency;nativeLastBandwidth=source->nativeLastBandwidth;
    nativeMixInput=source->nativeMixInput;nativeWetGain=source->nativeWetGain;
    nativeWetOffset = source->nativeWetOffset;
    appliedNativeWetOffset = source->appliedNativeWetOffset;

    const auto copyCurrentPreservingTarget = [](auto& target, const auto& sourceValue)
    {
        const auto targetValue = target.getTargetValue();
        target.setCurrentAndTargetValue(sourceValue.getCurrentValue());
        target.setTargetValue(targetValue);
    };
    copyCurrentPreservingTarget(feedbackAmount, source->feedbackAmount);
    copyCurrentPreservingTarget(wetMix, source->wetMix);
    copyCurrentPreservingTarget(delaySamples, source->delaySamples);
    copyCurrentPreservingTarget(rightDelaySamples, source->rightDelaySamples);
    // Preserve the remaining ramp duration when the published target is unchanged.
    const auto pingPongTarget = pingPongMix.getTargetValue();
    pingPongMix = source->pingPongMix;
    pingPongMix.setTargetValue(pingPongTarget);
    copyCurrentPreservingTarget(nativeFilterFrequency,source->nativeFilterFrequency);
    copyCurrentPreservingTarget(nativeFilterBandwidth,source->nativeFilterBandwidth);
    copyCurrentPreservingTarget(nativeProfileMix,source->nativeProfileMix);
    copyCurrentPreservingTarget(highCutHz, source->highCutHz);
    copyCurrentPreservingTarget(lowCutHz, source->lowCutHz);
    tempoBpm = source->tempoBpm;
}

void DelayFxCore::invalidateDelayHistory() noexcept
{
    // Clearing a 60-second stereo DelayLine can miss a small host deadline.
    // Keep the ring positions aligned and treat old samples as zero until the
    // requested delay lies entirely inside post-reset history.
    validDelaySamples = 0;
}

float DelayFxCore::popValidDelaySample(int channel, float delayInSamples) noexcept
{
    if (delayInSamples < 1.0f)
    {
        const auto firstValidTap = delayLine.popSample(channel, 1.0f);
        return validDelaySamples > 0 ? firstValidTap * delayInSamples : 0.0f;
    }

    if (validDelaySamples > 0
        && static_cast<float>(validDelaySamples) >= delayInSamples)
        return delayLine.popSample(channel, delayInSamples);

    const auto integerDelay = static_cast<int>(delayInSamples);
    const auto fractionalDelay = delayInSamples - static_cast<float>(integerDelay);

    if (fractionalDelay > 0.0f && validDelaySamples == integerDelay)
    {
        const auto firstValidTap =
            delayLine.popSample(channel, static_cast<float>(integerDelay));
        return firstValidTap * (1.0f - fractionalDelay);
    }

    (void) (delayLine.popSample(channel, delayInSamples));
    return 0.0f;
}

void DelayFxCore::advanceValidDelayHistory() noexcept
{
    validDelaySamples = std::min(delayLine.getMaximumDelayInSamples(), validDelaySamples + 1);
}

void DelayFxCore::setGenericParam(int index, float value01)
{
    if(index==27 || index==28) {
        (index==27?wideLeftMs:wideRightMs)=wideFxDecode(index,value01);setSnapshot(snapshot);return;
    }
    const auto safeValue01 = utils::sanitizeUnitInterval(value01);

    switch (index)
    {
        case 0: snapshot.timeIndex = std::clamp(fad::engine::math::roundToNearestInt(
                    FxCatalog::denormalizeParamValue(FxType::delay, 0, safeValue01)), 0, 8); break;
        case 1: snapshot.feedback  = FxCatalog::denormalizeParamValue(FxType::delay, 1, safeValue01); break;
        case 2: snapshot.highCutHz = FxCatalog::denormalizeParamValue(FxType::delay, 2, safeValue01); break;
        case 3: snapshot.lowCutHz  = FxCatalog::denormalizeParamValue(FxType::delay, 3, safeValue01); break;
        case 4: snapshot.mix       = safeValue01; break;
        case 5: nativeTimeMode = fad::engine::math::roundToNearestInt(safeValue01 * 2.0f); break;
        case 6: nativeLeftTime = safeValue01 * 16.0f; break;
        case 7: nativeRightTime = safeValue01 * 16.0f; break;
        case 8: sameChannelFeedback = safeValue01 >= .5f; break;
        case 9: nativeFilterFrequency.setTargetValue(safeValue01*18000.0f);filterUpdateCountdown=0;break;
        case 10:nativeFilterBandwidth.setTargetValue(safeValue01);filterUpdateCountdown=0;break;
        case 11:pingPongMix.setTargetValue(safeValue01 >= 0.5f ? 1.0f : 0.0f);break;
        default: break;
    }

    setSnapshot(snapshot);
}

void DelayFxCore::setSnapshot(const parameters::DelaySnapshot& newSnapshot) noexcept
{
    snapshot = newSnapshot;
    const auto beats = delayIndexToBeats(std::clamp(snapshot.timeIndex, 0, 8));
    const auto safeTempoBpm = std::isfinite(tempoBpm) ? std::clamp(tempoBpm, 20.0, 400.0) : 120.0;
    const auto secondsPerBeat = 60.0 / safeTempoBpm;
    const auto legacySeconds = static_cast<float>(beats * secondsPerBeat);
    const auto toSeconds = [&](float time) {
        return nativeTimeMode == 0 ? legacySeconds
            : time * (nativeTimeMode == 2 ? static_cast<float>(secondsPerBeat) : 1.0f);
    };
    const bool nativeProfile=nativeTimeMode!=0 && nativeFilterFrequency.getTargetValue()>0;
    nativeProfileMix.setTargetValue(nativeProfile?1.0f:0.0f);
    const auto delayTarget=[&](float time) {
        const auto frames=std::clamp(toSeconds(time), 0.0f, 12.0f)*static_cast<float>(sampleRate);
        return nativeProfile ? std::max(0.0f, frames-1.0f) : frames;
    };
    delaySamples.setTargetValue(wideLeftMs>=0?wideLeftMs*.001f*static_cast<float>(sampleRate):delayTarget(nativeLeftTime));
    rightDelaySamples.setTargetValue(wideRightMs>=0?wideRightMs*.001f*static_cast<float>(sampleRate):delayTarget(nativeRightTime));
    feedbackAmount.setTargetValue(utils::sanitizeFiniteClamped(snapshot.feedback, 0.0f, 0.0f, 0.95f));
    wetMix.setTargetValue(utils::sanitizeFiniteClamped(snapshot.mix, 0.0f, 0.0f, 1.0f));
    lowCutHz.setTargetValue(utils::sanitizeFiniteClamped(snapshot.lowCutHz, 20.0f, 20.0f, 4000.0f));
    const auto nyquistSafeHighCut =
        static_cast<float>(std::max(1000.0, sampleRate * 0.49));
    highCutHz.setTargetValue(std::clamp(utils::sanitizeFinite(snapshot.highCutHz, 20000.0f), lowCutHz.getTargetValue() + 20.0f, nyquistSafeHighCut));
}

void DelayFxCore::setTransportBpm(double newTempoBpm) noexcept
{
    const auto nextTempoBpm = std::isfinite(newTempoBpm)
        ? std::clamp(newTempoBpm, 20.0, 400.0)
        : 120.0;
    if (std::abs(nextTempoBpm - tempoBpm) <= 1.0e-9)
        return;

    tempoBpm = nextTempoBpm;
    setSnapshot(snapshot);
}

float DelayFxCore::delayIndexToBeats(int index) noexcept
{
    static constexpr std::array<float, 9> beatValues{
        0.125f,
        1.0f / 6.0f,
        0.25f,
        1.0f / 3.0f,
        0.5f,
        2.0f / 3.0f,
        1.0f,
        2.0f,
        4.0f};

    return beatValues[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(beatValues.size() - 1)))];
}

void DelayFxCore::updateNativeFilter(float frequency,float bandwidth) noexcept
{
    frequency=std::clamp(frequency, 40.0f, 18000.0f);
    bandwidth=std::clamp(bandwidth, 0.0f, 1.0f);
    if(frequency==nativeLastFrequency && bandwidth==nativeLastBandwidth)return;
    nativeLastFrequency=frequency;nativeLastBandwidth=bandwidth;
    // Native coefficient probes at 44.1/48/96 kHz share this reference-rate
    // coefficient law; delay timing itself follows the host sample rate.
    const double omega=(2 * std::numbers::pi_v<double>)*frequency/44100.0;
    const double alpha=std::sin(omega)*std::sinh(std::log(2.0)*(.65+6.5*bandwidth));
    const double inverse=1.0/(1.0+alpha);
    nativeB0=static_cast<float>(alpha*inverse);
    nativeA1=static_cast<float>(-2.0*std::cos(omega)*inverse);
    nativeA2=static_cast<float>((1.0-alpha)*inverse);
}

float DelayFxCore::processNativeFilter(int channel,float input) noexcept
{
    auto& state=nativeFilterStates[static_cast<size_t>(channel)];
    const auto output=nativeB0*input+state[0];
    const auto next1=state[1]-nativeA1*output;
    const auto next2=-nativeB0*input-nativeA2*output;
    if(!std::isfinite(output)||!std::isfinite(next1)||!std::isfinite(next2)) {state={};return 0.0f;}
    state={next1,next2};
    fad::engine::math::snapSmallState(state[0]);fad::engine::math::snapSmallState(state[1]);
    return output;
}

void DelayFxCore::initialiseFilterTypes() noexcept
{
    for (auto& filter : feedbackHighPass)
    {
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setResonance(0.707f);
    }

    for (auto& filter : feedbackLowPass)
    {
        filter.setType(fad::engine::filters::TptMode::lowpass);
        filter.setResonance(0.707f);
    }

    for (auto& filter : outputHighPass)
    {
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setResonance(0.707f);
    }

    for (auto& filter : outputLowPass)
    {
        filter.setType(fad::engine::filters::TptMode::lowpass);
        filter.setResonance(0.707f);
    }
}

void DelayFxCore::updateFilters(float lowCutValue, float highCutValue) noexcept
{
    const auto lowCut = std::clamp(lowCutValue, 20.0f, 4000.0f);
    const auto nyquistSafeHighCut =
        static_cast<float>(std::max(1000.0, sampleRate * 0.49));
    const auto highCut = std::clamp(highCutValue, lowCut + 20.0f, nyquistSafeHighCut);

    for (auto& filter : feedbackHighPass)
        filter.setCutoffFrequency(lowCut);

    for (auto& filter : feedbackLowPass)
        filter.setCutoffFrequency(highCut);

    for (auto& filter : outputHighPass)
        filter.setCutoffFrequency(lowCut);

    for (auto& filter : outputLowPass)
        filter.setCutoffFrequency(highCut);
}
}
