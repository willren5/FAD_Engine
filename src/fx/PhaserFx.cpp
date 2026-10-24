#include <fad/engine/fx/WideFxControls.h>
#include <fad/engine/fx/PhaserFx.h>

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
constexpr auto stageTransitionSeconds = 0.005;
constexpr int modulationUpdateIntervalSamples = 16;
}

void PhaserFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = fadmodel1::dsp::utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    dryBuffer.setSize(numChannels, static_cast<int>(spec.maximumBlockSize), false, false, true);

    rate.reset(sampleRate, paramSmoothingSeconds);
    depth.reset(sampleRate, paramSmoothingSeconds);
    centerFreq.reset(sampleRate, paramSmoothingSeconds);
    feedback.reset(sampleRate, paramSmoothingSeconds);
    mix.reset(sampleRate, paramSmoothingSeconds);
    nativeMixBlend.reset(sampleRate, .005);
    stereoOffset.reset(sampleRate, paramSmoothingSeconds);
    nativeCore.prepare(sampleRate);
    constexpr std::array<float, 5> nativeDefaults{0.0f, .5f, 0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < nativeControls.size(); ++i)
    {
        nativeControls[i].reset(sampleRate, paramSmoothingSeconds);
        nativeControls[i].setCurrentAndTargetValue(nativeDefaults[i]);
    }
    profileFadeLength = std::max(1, fad::engine::math::roundToNearestInt(sampleRate * .005));
    stageTransitionLengthSamples = std::max(1, fad::engine::math::roundToNearestInt(sampleRate * stageTransitionSeconds));

    for (auto& filter : feedbackDcBlockers)
    {
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setCutoffFrequency(5.0f);
        filter.setResonance(0.707f);
    }

    reset();
}

void PhaserFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
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

void PhaserFxCore::processActiveBlock(fad::engine::ProcessContextReplacing<float>& context)
{
    auto& block = context.getOutputBlock();
    if (block.getNumSamples() == 0 || block.getNumChannels() == 0
        || block.getNumChannels() > static_cast<size_t>(numChannels)) return;
    const bool requested = nativeProfile.load(std::memory_order_relaxed);
    if (requested != activeProfile)
    {
        transitionFrom = lastOutput;
        profileFadeRemaining = hasProcessedSample ? profileFadeLength : 0;
        activeProfile = requested;
        nativeCore.reset();
        nativeRemaining = 0;
        for (auto& channel : allPasses) for (auto& stage : channel) stage.z1 = 0.0f;
        for (auto& filter : feedbackDcBlockers) filter.reset();
        feedbackSample.fill(0.0f);
        modulationUpdateCountdown = 0;
    }
    if (activeProfile) processNativeBlock(context);
    else processLegacyBlock(context);
    for (size_t sample = 0; sample < block.getNumSamples(); ++sample)
    {
        const auto fade = profileFadeRemaining > 0
            ? static_cast<float>(profileFadeRemaining) / static_cast<float>(profileFadeLength) : 0.0f;
        for (size_t channel = 0; channel < block.getNumChannels(); ++channel)
        {
            auto& value = block.getChannelPointer(channel)[sample];
            if (profileFadeRemaining > 0) value = transitionFrom[channel] * fade + value * (1.0f - fade);
            lastOutput[channel] = fadmodel1::dsp::utils::sanitizeFinite(value);
            value = lastOutput[channel];
        }
        if (profileFadeRemaining > 0) --profileFadeRemaining;
    }
    hasProcessedSample = true;
}

void PhaserFxCore::processNativeBlock(fad::engine::ProcessContextReplacing<float>& context)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;
    auto& block = context.getOutputBlock();
    for (size_t sample = 0; sample < block.getNumSamples(); ++sample)
    {
        for (auto& control : nativeControls) control.getNextValue();
        const auto d = depth.getNextValue();
        const auto m = mix.getNextValue();
        // Keep legacy smoothers on the same sample timeline during profile changes.
        rate.getNextValue(); centerFreq.getNextValue(); feedback.getNextValue();
        stereoOffset.getNextValue(); nativeMixBlend.getNextValue(); nativeWetModulation.next();
        if (nativeRemaining <= 0)
        {
            const auto beats = nativeControls[4].getCurrentValue();
            const auto hz = wideRateHz>=0 ? wideRateHz : beats > 0.0f ? static_cast<float>(transportBpm / (60.0 * beats))
                                        : nativeControls[0].getCurrentValue();
            nativeCore.setParameters(nativeControls[1].getCurrentValue() + nativeOffsets[1], hz, d,
                nativeControls[3].getCurrentValue(), nativeControls[2].getCurrentValue(), m + nativeOffsets[0], wideRateHz>=0);
            nativeRemaining = modulationUpdateIntervalSamples;
        }
        --nativeRemaining;
        const auto left = block.getChannelPointer(0)[sample];
        const auto right = block.getNumChannels() > 1 ? block.getChannelPointer(1)[sample] : left;
        const auto output = nativeCore.process({left, right}, activityKnown, voiceActive);
        for (size_t channel = 0; channel < block.getNumChannels(); ++channel)
            block.getChannelPointer(channel)[sample] = output[channel];
    }
}

void PhaserFxCore::addNativeModulation(int target, float offset) noexcept
{
    nativeWetModulation.add(target, offset);
    const int index = target == 109 ? 0 : target == 113 ? 1 : -1;
    if (index >= 0 && std::isfinite(offset))
    {
        auto& value = nativeOffsets[static_cast<size_t>(index)];
        value = std::clamp(value + offset, -32.0f, 32.0f);
    }
}

void PhaserFxCore::setTransportBpm(double bpm)
{
    transportBpm = std::isfinite(bpm) ? std::clamp(bpm, 20.0, 400.0) : 120.0;
}

void PhaserFxCore::processLegacyBlock(fad::engine::ProcessContextReplacing<float>& context)
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

    const auto resetSmoothedParameters = [this]() noexcept
    {
        const auto resetToFiniteTarget = [] (auto& smoother, float fallback) noexcept
        {
            const auto target = smoother.getTargetValue();
            smoother.setCurrentAndTargetValue(std::isfinite(target) ? target : fallback);
        };

        resetToFiniteTarget(rate, 0.1f);
        resetToFiniteTarget(depth, 0.0f);
        resetToFiniteTarget(centerFreq, 1000.0f);
        resetToFiniteTarget(feedback, 0.0f);
        resetToFiniteTarget(mix, 0.0f);
        resetToFiniteTarget(stereoOffset, 0.0f);
    };

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto currentRate = rate.getNextValue();
        const auto currentDepth = depth.getNextValue();
        const auto currentCenterFreq = centerFreq.getNextValue();
        const auto currentFeedback = feedback.getNextValue();
        const auto nativeBlend = nativeMixBlend.getNextValue();
        const auto currentMix = std::clamp(mix.getNextValue() + nativeBlend * nativeWetModulation.next(), 0.0f, 1.0f);
        const auto currentStereoOffset = stereoOffset.getNextValue();
        const auto resetChannelState = [this](size_t ch) noexcept
        {
            for (auto& stage : allPasses[ch])
                stage.z1 = 0.0f;
            feedbackDcBlockers[ch].reset();
            feedbackSample[ch] = 0.0f;
        };

        if (! std::isfinite(currentRate)
            || ! std::isfinite(currentDepth)
            || ! std::isfinite(currentCenterFreq)
            || ! std::isfinite(currentFeedback)
            || ! std::isfinite(currentMix)
            || ! std::isfinite(currentStereoOffset))
        {
            resetSmoothedParameters();
            for (int channel = 0; channel < blockChannels; ++channel)
            {
                const auto ch = static_cast<size_t>(channel);
                const auto drySample = dryBuffer.getSample(channel, sampleIndex);
                resetChannelState(ch);
                outputBlock.getChannelPointer(ch)[sampleIndex] =
                    std::isfinite(drySample) ? drySample : 0.0f;
            }
            continue;
        }

        if (stageTransitionSamplesRemaining <= 0)
        {
            const auto requestedStages = std::clamp(numStages.load(std::memory_order_relaxed), 2, 8);
            if (requestedStages != activeNumStages)
            {
                transitionFromStages = activeNumStages;
                transitionToStages = requestedStages;
                stageTransitionSamplesRemaining = stageTransitionLengthSamples;
            }
        }

        if (modulationUpdateCountdown <= 0)
        {
            const auto stereoOffsetRadians = currentStereoOffset
                                             * (std::numbers::pi_v<float> / 180.0f);
            const auto lfoValues = std::array<float, 2>{
                std::sin((2 * std::numbers::pi_v<float>) * (wideRateHz>=0?static_cast<float>(wideLfoPhase):lfoPhase)),
                std::sin((2 * std::numbers::pi_v<float>) * (wideRateHz>=0?static_cast<float>(wideLfoPhase):lfoPhase) + stereoOffsetRadians)};

            for (int channel = 0; channel < blockChannels; ++channel)
            {
                const auto ch = static_cast<size_t>(channel);
                auto fc = currentCenterFreq * std::pow(2.0f, currentDepth * lfoValues[ch]);
                fc = std::clamp(fc, 20.0f, static_cast<float>(sampleRate * 0.49));
                const auto tanVal = std::tan(std::numbers::pi_v<float> * fc
                                             / static_cast<float>(sampleRate));
                allPassCoefficients[ch] = (tanVal - 1.0f) / (tanVal + 1.0f);
            }

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
            const auto a = allPassCoefficients[ch];

            // Add feedback
            const auto drySample = dryBuffer.getSample(channel, sampleIndex);
            auto input = drySample + feedbackSample[ch] * currentFeedback;
            if (! std::isfinite(drySample) || ! std::isfinite(input))
            {
                resetChannelState(ch);
                outputBlock.getChannelPointer(ch)[sampleIndex] =
                    std::isfinite(drySample) ? drySample : 0.0f;
                continue;
            }

            // Keep all topology taps warm so changing the audible tap never exposes stale state.
            bool cascadeInterrupted = false;
            std::array<float, 4> stageOutputs{};
            for (int stage = 0; stage < 8; ++stage)
            {
                const auto s = static_cast<size_t>(stage);
                const auto x = input;
                const auto y = -a * x + allPasses[ch][s].z1;
                allPasses[ch][s].z1 = x + a * y;
                fad::engine::math::snapSmallState(allPasses[ch][s].z1);
                if (! std::isfinite(y) || ! std::isfinite(allPasses[ch][s].z1))
                {
                    resetChannelState(ch);
                    cascadeInterrupted = true;
                    break;
                }
                input = y;
                if ((stage & 1) != 0)
                    stageOutputs[static_cast<size_t>(stage / 2)] = input;
            }

            if (cascadeInterrupted)
            {
                outputBlock.getChannelPointer(ch)[sampleIndex] = drySample;
                continue;
            }

            const auto tapForStages = [&stageOutputs](int stages) noexcept
            {
                return stageOutputs[static_cast<size_t>(std::clamp(stages, 2, 8) / 2 - 1)];
            };
            input = tapForStages(activeNumStages);
            if (stageTransitionSamplesRemaining > 0)
            {
                const auto transitionMix = 1.0f
                    - static_cast<float>(stageTransitionSamplesRemaining)
                        / static_cast<float>(stageTransitionLengthSamples);
                const auto from = tapForStages(transitionFromStages);
                input = from + (tapForStages(transitionToStages) - from) * transitionMix;
            }

            feedbackSample[ch] = feedbackDcBlockers[ch].processSample(0, input);
            if (! std::isfinite(feedbackSample[ch]))
            {
                resetChannelState(ch);
                outputBlock.getChannelPointer(ch)[sampleIndex] = drySample;
                continue;
            }
            fad::engine::math::snapSmallState(feedbackSample[ch]);

            // Soft-limit the wet signal to prevent extreme peaks at high
            // feedback + many stages (theoretical max gain ≈ 1/(1-|fb|))
            const auto limited = std::tanh(input);
            if (nativeBlend > 0.0f)
            {
                const auto w = serumModulationFxWetGain(currentMix);
                const auto dryGain = (1.0f-currentMix) + nativeBlend*(currentMix-w*w);
                const auto wetGain = currentMix + nativeBlend*(w-currentMix);
                outputBlock.getChannelPointer(ch)[sampleIndex] = drySample*dryGain + limited*wetGain;
            }
            else
                outputBlock.getChannelPointer(ch)[sampleIndex] =
                    drySample * (1.0f - currentMix) + limited * currentMix;
        }

        if (stageTransitionSamplesRemaining > 0
            && --stageTransitionSamplesRemaining == 0)
        {
            activeNumStages = transitionToStages;
        }
    }
    if (numSamples > 0) hasProcessedSample = true;
}

void PhaserFxCore::reset()
{
    nativeCore.reset(); nativeOffsets.fill(0.0f); nativeRemaining = 0;
    activityKnown = voiceActive = false; profileFadeRemaining = 0;
    lastOutput = {}; transitionFrom = {};
    activeProfile = nativeProfile.load(std::memory_order_relaxed);
    for (auto& control : nativeControls) control.setCurrentAndTargetValue(control.getTargetValue());
    nativeWetModulation.reset(); hasProcessedSample = false;
    nativeMixBlend.setCurrentAndTargetValue(nativeMixEnabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
    lfoPhase = 0.0f; wideLfoPhase=0;
    feedbackSample.fill(0.0f);
    allPassCoefficients.fill(0.0f);
    modulationUpdateCountdown = 0;
    activeNumStages = std::clamp(numStages.load(std::memory_order_relaxed), 2, 8);
    transitionFromStages = activeNumStages;
    transitionToStages = activeNumStages;
    stageTransitionSamplesRemaining = 0;

    for (auto& channelStages : allPasses)
        for (auto& stage : channelStages)
            stage.z1 = 0.0f;

    for (auto& filter : feedbackDcBlockers)
        filter.reset();

    rate.setCurrentAndTargetValue(rate.getTargetValue());
    depth.setCurrentAndTargetValue(depth.getTargetValue());
    centerFreq.setCurrentAndTargetValue(centerFreq.getTargetValue());
    feedback.setCurrentAndTargetValue(feedback.getTargetValue());
    mix.setCurrentAndTargetValue(mix.getTargetValue());
    stereoOffset.setCurrentAndTargetValue(stereoOffset.getTargetValue());
}

void PhaserFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        nativeCore.reset(); nativeOffsets.fill(0.0f); nativeRemaining = 0;
        activityKnown = voiceActive = false; profileFadeRemaining = 0;
        lastOutput = {}; transitionFrom = {};
        nativeWetModulation.reset(); hasProcessedSample = false;
        nativeMixBlend.setCurrentAndTargetValue(nativeMixEnabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
        for (auto& channelStages : allPasses)
            for (auto& stage : channelStages)
                stage.z1 = 0.0f;
        for (auto& filter : feedbackDcBlockers)
            filter.reset();
        feedbackSample.fill(0.0f);
        lfoPhase = 0.0f; wideLfoPhase=0;
        allPassCoefficients.fill(0.0f);
        modulationUpdateCountdown = 0;
        activeNumStages = std::clamp(numStages.load(std::memory_order_relaxed), 2, 8);
        transitionFromStages = activeNumStages;
        transitionToStages = activeNumStages;
        stageTransitionSamplesRemaining = 0;
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void PhaserFxCore::setGenericParam(int index, float value01)
{
    if(index==26) {
        const auto next=wideFxDecode(index,value01);
        if(wideRateHz<0 && next>=0)wideLfoPhase=lfoPhase;
        if(wideRateHz>=0 && next<0)lfoPhase=static_cast<float>(wideLfoPhase);
        wideRateHz=next;rate.setTargetValue(wideRateHz>=0?wideRateHz:legacyRateHz);return;
    }
    const auto safeValue01 = fadmodel1::dsp::utils::sanitizeUnitInterval(value01);

    if (index == 8) { nativeProfile.store(safeValue01 >= .5f, std::memory_order_relaxed); return; }
    if (index >= 9 && index <= 13)
    {
        const auto value = safeValue01 * (index == 9 ? 20.0f : index == 13 ? 128.0f : 1.0f);
        auto& control = nativeControls[static_cast<size_t>(index - 9)];
        if (hasProcessedSample) control.setTargetValue(value);
        else control.setCurrentAndTargetValue(value);
        return;
    }

    switch (index)
    {
        case 0: // Rate (0.01-10 Hz, skew 0.3)
            legacyRateHz=fad::engine::math::RuntimeRange(0.01f,10.0f,0.0f,.3f).denormalise(safeValue01);
            rate.setTargetValue(wideRateHz>=0?wideRateHz:legacyRateHz);
            break;
        case 1: // Depth (0-1)
            depth.setTargetValue(safeValue01);
            break;
        case 2: // Center Freq (200-8000 Hz, skew 0.3)
            centerFreq.setTargetValue(
                fad::engine::math::RuntimeRange(200.0f, 8000.0f, 0.0f, 0.3f).denormalise(safeValue01));
            break;
        case 3: // Feedback (-0.95 to +0.95)
            feedback.setTargetValue(fad::engine::math::interpolate(safeValue01, -0.95f, 0.95f));
            break;
        case 4: // Stages (discrete: 2,4,6,8)
        {
            int stages = fad::engine::math::roundToNearestInt(safeValue01 * 3.0f) * 2 + 2;
            numStages.store(std::clamp(stages, 2, 8), std::memory_order_relaxed);
            break;
        }
        case 5: // Mix (0-1)
            mix.setTargetValue(safeValue01);
            break;
        case 6: // Stereo offset (0-180 degrees)
            stereoOffset.setTargetValue(fad::engine::math::interpolate(safeValue01, 0.0f, 180.0f));
            break;
        case 7:
            nativeMixEnabled.store(safeValue01 >= .5f, std::memory_order_relaxed);
            if (hasProcessedSample) nativeMixBlend.setTargetValue(safeValue01 >= .5f ? 1.0f : 0.0f);
            else nativeMixBlend.setCurrentAndTargetValue(safeValue01 >= .5f ? 1.0f : 0.0f);
            break;
        default: break;
    }
}
}
