#include <fad/engine/fx/WideFxControls.h>
#include <fad/engine/fx/ChorusFx.h>
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
constexpr auto parameterSmoothingSeconds = 0.03;
constexpr int parameterUpdateBlockSize = 16;

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

void ChorusFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    modeFadeLength = std::clamp(static_cast<int>(sampleRate*.003), 16, 256);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    dryBuffer.setSize(numChannels, static_cast<int>(spec.maximumBlockSize), false, false, true);
    const fad::engine::ProcessSpec monoSpec{sampleRate, spec.maximumBlockSize, 1u};

    for (auto& chorus : choruses)
        chorus.prepare(monoSpec.sampleRate);
    for(auto& chorus:wideChoruses)chorus.prepare(monoSpec.sampleRate);
    wideScratch.fill(0.0);

    for (auto& filter : outputHighPass)
        filter.prepare(monoSpec);
    for (auto& filter : outputLowPass)
        filter.prepare(monoSpec);

    nativeOversampling = std::make_unique<fad::engine::filters::Oversample2x>();
    nativeOversampling->prepare(static_cast<uint32_t>(numChannels), fad::engine::filters::preparedAllpass2x(true));
    for (auto& delay : nativeDelays) {
        delay.setMaximumDelayInSamples(static_cast<int>(sampleRate * .2) + 32);
        delay.prepare({sampleRate * 2, 1, 1});
    }
    for (auto& filter : nativeFilters) {
        filter.prepare({sampleRate * 2, 1, 1});
        filter.setType(fad::engine::filters::TptMode::lowpass);
        filter.setResonance(1.41421356f);
    }
    nativeDelay2Ms.reset(sampleRate, parameterSmoothingSeconds);
    nativeDepthMs.reset(sampleRate, parameterSmoothingSeconds);
    rateHz.reset(sampleRate, parameterSmoothingSeconds);
    depth.reset(sampleRate, parameterSmoothingSeconds);
    centreDelayMs.reset(sampleRate, parameterSmoothingSeconds);
    feedback.reset(sampleRate, parameterSmoothingSeconds);
    mix.reset(sampleRate, parameterSmoothingSeconds);
    nativeMixBlend.reset(sampleRate, parameterSmoothingSeconds);
    highCutHz.reset(sampleRate, parameterSmoothingSeconds);
    lowCutHz.reset(sampleRate, parameterSmoothingSeconds);
    reset();
}

void ChorusFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
{
    if (bypassed.load(std::memory_order_relaxed))
        return;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto scratchCapacity = dryBuffer.getNumSamples();

    if (numSamples > scratchCapacity)
    {
        if (scratchCapacity <= 0)
            return;

        for (int offset = 0; offset < numSamples; offset += scratchCapacity)
        {
            auto chunkBlock = outputBlock.getSubBlock(static_cast<size_t>(offset),
                                                      static_cast<size_t>(std::min(scratchCapacity, numSamples - offset)));
            auto chunkContext = fad::engine::ProcessContextReplacing<float>(chunkBlock);
            processBlockInternal(chunkContext);
        }
        return;
    }

    processBlockInternal(context);
}

void ChorusFxCore::processBlockInternal(fad::engine::ProcessContextReplacing<float>& context)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());
    const auto activeChannels = std::min(numChannels, blockChannels);
    if (numSamples <= 0 || activeChannels <= 0)
        return;

    if (nativeDual != activeNativeDual || (!nativeDual && activeWideRate != (wideRateHz>=0))) {
        activeNativeDual = nativeDual;
        activeWideRate = wideRateHz>=0;
        parameterQuantumSamplesRemaining = 0;
        if (hasProcessed) {modeFadeStart=lastOutput; modeFadeRemaining=modeFadeLength;}
    }
    if (nativeDual) {
        processNative(outputBlock);
        applyModeTransition(outputBlock);
        return;
    }

    assert(activeChannels <= dryBuffer.getNumChannels());
    assert(numSamples <= dryBuffer.getNumSamples());

    for (int channel = 0; channel < activeChannels; ++channel)
        std::copy_n(outputBlock.getChannelPointer(static_cast<size_t>(channel)), numSamples, dryBuffer.getWritePointer(channel));

    for (int offset = 0; offset < numSamples;)
    {
        if (parameterQuantumSamplesRemaining <= 0)
        {
            appliedMix = applyParameters();
            parameterQuantumSamplesRemaining = parameterUpdateBlockSize;
        }

        const auto chunkSize = std::min(parameterQuantumSamplesRemaining, numSamples - offset);
        const auto currentMix = appliedWetGain;

        auto chunkBlock = outputBlock.getSubBlock(static_cast<size_t>(offset),
                                                  static_cast<size_t>(chunkSize));

        if (activeChannels == 1)
        {
            auto monoBlock = chunkBlock.getSingleChannelBlock(0);
            auto monoContext = fad::engine::ProcessContextReplacing<float>(monoBlock);
            processChorus(0,monoContext);
            outputHighPass[0].process(monoContext);
            outputHighPass[0].snapToZero();
            outputLowPass[0].process(monoContext);
            outputLowPass[0].snapToZero();

            auto* wet = chunkBlock.getChannelPointer(0);
            const auto* dry = dryBuffer.getReadPointer(0, offset);
            const auto dryMix = appliedDryGain;
            for (int i = 0; i < chunkSize; ++i)
            {
                if (! std::isfinite(wet[i]))
                {
                    choruses[0].reset();
                    wideChoruses[0].reset();
                    outputHighPass[0].reset();
                    outputLowPass[0].reset();
                    wet[i] = 0.0f;
                }
                wet[i] = utils::sanitizeFinite(utils::sanitizeFinite(dry[i]) * dryMix
                                               + wet[i] * currentMix);
            }
            parameterQuantumSamplesRemaining -= chunkSize;
            offset += chunkSize;
            continue;
        }

        auto leftBlock = chunkBlock.getSingleChannelBlock(0);
        auto rightBlock = chunkBlock.getSingleChannelBlock(1);
        auto leftContext = fad::engine::ProcessContextReplacing<float>(leftBlock);
        auto rightContext = fad::engine::ProcessContextReplacing<float>(rightBlock);
        processChorus(0,leftContext);
        processChorus(1,rightContext);
        outputHighPass[0].process(leftContext);
        outputHighPass[1].process(rightContext);
        outputHighPass[0].snapToZero();
        outputHighPass[1].snapToZero();
        outputLowPass[0].process(leftContext);
        outputLowPass[1].process(rightContext);
        outputLowPass[0].snapToZero();
        outputLowPass[1].snapToZero();

        const auto dryMix = appliedDryGain;
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            auto* wet = chunkBlock.getChannelPointer(static_cast<size_t>(channel));
            const auto* dry = dryBuffer.getReadPointer(channel, offset);
            for (int i = 0; i < chunkSize; ++i)
            {
                if (! std::isfinite(wet[i]))
                {
                    choruses[static_cast<size_t>(channel)].reset();
                    wideChoruses[static_cast<size_t>(channel)].reset();
                    outputHighPass[static_cast<size_t>(channel)].reset();
                    outputLowPass[static_cast<size_t>(channel)].reset();
                    wet[i] = 0.0f;
                }
                wet[i] = utils::sanitizeFinite(utils::sanitizeFinite(dry[i]) * dryMix
                                               + wet[i] * currentMix);
            }
        }

        parameterQuantumSamplesRemaining -= chunkSize;
        offset += chunkSize;
    }
    applyModeTransition(outputBlock);
}

void ChorusFxCore::reset()
{
    nativeWetModulation.reset();
    activeNativeDual = nativeDual; activeWideRate = wideRateHz>=0; hasProcessed = false; modeFadeRemaining = 0; lastOutput = {}; modeFadeStart = {};
    nativePhase = nativeInitialPhase;
    nativeTailSamples = 0;
    nativeFeedback.fill(0);
    nativeWetPrevious = {}; nativeAlignment = {}; nativeAlignmentIndex = 0;
    for (auto& delay : nativeDelays) delay.reset();
    for (auto& filter : nativeFilters) filter.reset();
    if (nativeOversampling) nativeOversampling->reset();
    nativeDelay2Ms.setCurrentAndTargetValue(nativeDelay2Ms.getTargetValue());
    nativeDepthMs.setCurrentAndTargetValue(nativeDepthMs.getTargetValue());
    rateHz.setCurrentAndTargetValue(rateHz.getTargetValue());
    depth.setCurrentAndTargetValue(depth.getTargetValue());
    centreDelayMs.setCurrentAndTargetValue(centreDelayMs.getTargetValue());
    feedback.setCurrentAndTargetValue(feedback.getTargetValue());
    mix.setCurrentAndTargetValue(mix.getTargetValue());
    nativeMixBlend.setCurrentAndTargetValue(nativeMixBlend.getTargetValue());
    highCutHz.setCurrentAndTargetValue(highCutHz.getTargetValue());
    lowCutHz.setCurrentAndTargetValue(lowCutHz.getTargetValue());

    // Set the inner wet-only mixer before resetting its smoothing state.
    // Otherwise a freshly reset chorus briefly leaks dry audio into the wet path.
    appliedMix = applyParameters();
    for (auto& chorus : choruses)
        chorus.reset();
    for(auto& chorus:wideChoruses)chorus.reset();
    for (auto& filter : outputHighPass)
        filter.reset();
    for (auto& filter : outputLowPass)
        filter.reset();

    updateOutputFilters(highCutHz.getCurrentValue(), lowCutHz.getCurrentValue());
    parameterQuantumSamplesRemaining = 0;
    appliedMix = std::clamp(mix.getCurrentValue(), 0.0f, 1.0f);
}

void ChorusFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        nativeWetModulation.reset();
        nativePhase = nativeInitialPhase;
        nativeTailSamples = 0;
        nativeFeedback.fill(0);
        nativeWetPrevious = {}; nativeAlignment = {}; nativeAlignmentIndex = 0;
        for (auto& delay : nativeDelays) delay.reset();
        for (auto& filter : nativeFilters) filter.reset();
        if (nativeOversampling) nativeOversampling->reset();
        for (auto& chorus : choruses)
            chorus.reset();
        for (auto& filter : outputHighPass)
            filter.reset();
        for (auto& filter : outputLowPass)
            filter.reset();
        parameterQuantumSamplesRemaining = 0;
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void ChorusFxCore::setGenericParam(int index, float value01)
{
    if(index==26){wideRateHz=wideFxDecode(index,value01);setSnapshot(snapshot);return;}
    const auto safeValue01 = utils::sanitizeUnitInterval(value01);

    switch (index)
    {
        case 0: snapshot.rateHz    = FxCatalog::denormalizeParamValue(FxType::chorus, 0, safeValue01); break;
        case 1: snapshot.depth     = safeValue01; break;
        case 2: snapshot.delayMs   = FxCatalog::denormalizeParamValue(FxType::chorus, 2, safeValue01); break;
        case 3: snapshot.feedback  = FxCatalog::denormalizeParamValue(FxType::chorus, 3, safeValue01); break;
        case 4: snapshot.mix       = safeValue01; break;
        case 5: snapshot.highCutHz = FxCatalog::denormalizeParamValue(FxType::chorus, 5, safeValue01); break;
        case 6: snapshot.lowCutHz  = FxCatalog::denormalizeParamValue(FxType::chorus, 6, safeValue01); break;
        case 7: nativeMixBlend.setTargetValue(safeValue01 >= .5f ? 1.0f : 0.0f); break;
        case 8: nativeDelay2Ms.setTargetValue(20.0f * safeValue01); break;
        case 9: nativeDepthMs.setTargetValue(62.5f * safeValue01); break;
        case 10: nativeDual = safeValue01 >= .5f; break;
        case 11:
            // Zero is the legacy sentinel; explicit imported phases occupy .5..1.
            nativeInitialPhase = safeValue01 < .5f ? .5 : static_cast<double>(2.0f * safeValue01 - 1.0f);
            nativeInitialPhase -= std::floor(nativeInitialPhase);
            if (!hasProcessed) nativePhase = nativeInitialPhase;
            break;
        default: break;
    }

    setSnapshot(snapshot);
}

void ChorusFxCore::setSnapshot(const parameters::ChorusSnapshot& newSnapshot) noexcept
{
    snapshot = newSnapshot;
    rateHz.setTargetValue(wideRateHz>=0?wideRateHz:utils::sanitizeFiniteClamped(snapshot.rateHz, 0.25f, 0.05f, 12.0f));
    depth.setTargetValue(utils::sanitizeFiniteClamped(snapshot.depth, 0.0f, 0.0f, 1.0f));
    centreDelayMs.setTargetValue(utils::sanitizeFiniteClamped(snapshot.delayMs, 8.0f, 1.0f, 40.0f));
    feedback.setTargetValue(utils::sanitizeFiniteClamped(snapshot.feedback, 0.0f, -0.95f, 0.95f));
    mix.setTargetValue(utils::sanitizeFiniteClamped(snapshot.mix, 0.0f, 0.0f, 1.0f));
    highCutHz.setTargetValue(utils::sanitizeFiniteClamped(snapshot.highCutHz, 20000.0f, 1000.0f, 20000.0f));
    lowCutHz.setTargetValue(utils::sanitizeFiniteClamped(snapshot.lowCutHz, 20.0f, 20.0f, 4000.0f));
}

void ChorusFxCore::updateOutputFilters(float currentHiCutHz, float currentLoCutHz) noexcept
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

void ChorusFxCore::applyModeTransition(fad::engine::AudioBlock<float>& block)
{
    const auto channels=std::min(static_cast<size_t>(numChannels), block.getNumChannels());
    size_t sample=0;
    for(;sample<block.getNumSamples() && modeFadeRemaining>0;++sample) {
        const auto fade=static_cast<float>(modeFadeRemaining)/static_cast<float>(modeFadeLength);
        for(size_t ch=0;ch<channels;++ch)
            block.setSample(ch,sample,modeFadeStart[ch]*fade+block.getSample(ch,sample)*(1-fade));
        --modeFadeRemaining;
    }
    if(block.getNumSamples()>0) {
        for(size_t ch=0;ch<channels;++ch) lastOutput[ch]=block.getSample(ch,block.getNumSamples()-1);
        hasProcessed=true;
    }
}

void ChorusFxCore::processNative(fad::engine::AudioBlock<float>& block)
{
    // Independent two-line feedback chorus. The measured delay modulation is a
    // unipolar triangle; the second line is inverted and the right pair is 90 degrees apart.
    const int channels = std::min(numChannels, static_cast<int>(block.getNumChannels()));
    for (size_t i = 0; i < block.getNumSamples(); ++i) {
        std::array<float, 2> input{}, wet{};
        for (int ch = 0; ch < channels; ++ch) {
            input[static_cast<size_t>(ch)] = utils::sanitizeFinite(block.getSample(static_cast<size_t>(ch), i));
            wet[static_cast<size_t>(ch)] = input[static_cast<size_t>(ch)];
            if (std::abs(input[static_cast<size_t>(ch)]) > 1.0e-12f)
                nativeTailSamples = static_cast<int>(sampleRate * 2.0);
        }
        const auto rate = rateHz.getNextValue();
        const auto d1 = centreDelayMs.getNextValue();
        const auto d2 = nativeDelay2Ms.getNextValue();
        const auto dep = nativeDepthMs.getNextValue();
        const auto fb = feedback.getNextValue();
        const auto m = std::clamp(mix.getNextValue() + nativeWetModulation.next(), 0.0f, 1.0f);
        const auto cutoff = highCutHz.getNextValue();
        depth.getNextValue(); lowCutHz.getNextValue(); nativeMixBlend.getNextValue();
        if (parameterQuantumSamplesRemaining <= 0) {
            for (auto& filter : nativeFilters)
                filter.setCutoffFrequency(std::clamp(cutoff * .8660254f, 50.0f, static_cast<float>(sampleRate * .98)));
            parameterQuantumSamplesRemaining = parameterUpdateBlockSize;
        }
        --parameterQuantumSamplesRemaining;
        std::array<float*, 2> ptr{&wet[0], &wet[1]};
        fad::engine::AudioBlock<float> frame(ptr.data(), static_cast<size_t>(numChannels), 1);
        auto up = nativeOversampling->processSamplesUp(frame);
        for (size_t sub = 0; sub < up.getNumSamples(); ++sub) {
            for (int ch = 0; ch < channels; ++ch) {
                auto phase = nativePhase + (ch == 0 ? 0.0 : .25);
                phase -= std::floor(phase);
                const auto triangle = static_cast<float>(1.0 - std::abs(2.0 * phase - 1.0));
                const auto x = up.getSample(static_cast<size_t>(ch), sub);
                float sum = 0;
                for (int tap = 0; tap < 2; ++tap) {
                    const auto idx = static_cast<size_t>(2 * ch + tap);
                    const auto ms = (tap == 0 ? d1 : d2) + dep * (tap == 0 ? triangle : 1.0f - triangle);
                    nativeDelays[idx].setDelay(std::max(1.0f, ms * static_cast<float>(sampleRate * .002)));
                    nativeDelays[idx].pushSample(0, x + nativeFeedback[idx] * fb);
                    const auto delayed = nativeDelays[idx].popSample(0);
                    auto filtered = nativeFilters[idx].processSample(0, delayed);
                    if (!std::isfinite(filtered)) {
                        nativeDelays[idx].reset(); nativeFilters[idx].reset(); filtered = 0;
                    }
                    nativeFeedback[idx] = utils::sanitizeFinite(delayed);
                    sum += filtered;
                }
                up.setSample(static_cast<size_t>(ch), sub, .5f * sum);
            }
            if (nativeTailSamples > 0) {
                nativePhase += static_cast<double>(rate) / (sampleRate * 2);
                nativePhase -= std::floor(nativePhase);
            }
        }
        if (nativeTailSamples > 0) --nativeTailSamples;
        nativeOversampling->processSamplesDown(frame);
        // Separate native filter resampling contributes .75 current + .25 previous.
        // Held-out low-cutoff noise probes also establish two host samples of delay.
        for(int ch=0;ch<channels;++ch) {
            const auto c=static_cast<size_t>(ch);
            const auto filtered=.75f*wet[c]+.25f*nativeWetPrevious[c];
            nativeWetPrevious[c]=wet[c];
            wet[c]=nativeAlignment[c][nativeAlignmentIndex];
            nativeAlignment[c][nativeAlignmentIndex]=filtered;
        }
        nativeAlignmentIndex=(nativeAlignmentIndex+1)%2;
        const auto w = .5f - .5f * std::cos(std::numbers::pi_v<float> * m);
        for (int ch = 0; ch < channels; ++ch)
            block.setSample(static_cast<size_t>(ch), i,
                utils::sanitizeFinite(input[static_cast<size_t>(ch)] * (1 - w*w) + wet[static_cast<size_t>(ch)] * w));
    }
    for (auto& filter : nativeFilters) filter.snapToZero();
}

void ChorusFxCore::processChorus(size_t channel,fad::engine::ProcessContextReplacing<float>& context) noexcept
{
    if(wideRateHz<0){choruses[channel].process(context);return;}
    auto& block=context.getOutputBlock();
    auto* samples=block.getChannelPointer(0);
    auto* scratch=wideScratch.data();
    for(size_t i=0;i<block.getNumSamples();++i)scratch[i]=samples[i];
    double* widePlanes[]{wideScratch.data()};
    auto wideBlock=fad::engine::AudioBlock<double>(widePlanes,1,block.getNumSamples());
    auto wideContext=fad::engine::ProcessContextReplacing<double>(wideBlock);
    wideChoruses[channel].process(wideContext);
    for(size_t i=0;i<block.getNumSamples();++i)samples[i]=static_cast<float>(scratch[i]);
}

float ChorusFxCore::applyParameters()
{
    const auto currentRate = utils::sanitizeFinite(consumeSmoothedValue(rateHz, parameterUpdateBlockSize), 0.25f);
    const auto currentDepth = utils::sanitizeFinite(consumeSmoothedValue(depth, parameterUpdateBlockSize), 0.0f);
    const auto currentDelay = utils::sanitizeFinite(consumeSmoothedValue(centreDelayMs, parameterUpdateBlockSize), 8.0f);
    const auto currentFeedback = utils::sanitizeFinite(consumeSmoothedValue(feedback, parameterUpdateBlockSize), 0.0f);
    const auto currentMix =
        std::clamp(utils::sanitizeFinite(consumeSmoothedValue(mix, parameterUpdateBlockSize), 0.0f), 0.0f, 1.0f);
    const auto currentHiCut = utils::sanitizeFinite(consumeSmoothedValue(highCutHz, parameterUpdateBlockSize), 20000.0f);
    const auto currentLoCut = utils::sanitizeFinite(consumeSmoothedValue(lowCutHz, parameterUpdateBlockSize), 20.0f);

    choruses[0].setRate(currentRate);
    choruses[0].setDepth(currentDepth);
    choruses[0].setCentreDelay(currentDelay);
    choruses[0].setFeedback(currentFeedback);
    choruses[0].setMix(1.0f);

    choruses[1].setRate(currentRate * 1.11f);
    choruses[1].setDepth(std::clamp(currentDepth * 0.96f, 0.0f, 1.0f));
    choruses[1].setCentreDelay(std::clamp(currentDelay * 1.18f, 1.0f, 40.0f));
    choruses[1].setFeedback(currentFeedback * 0.9f);
    choruses[1].setMix(1.0f);

    if(wideRateHz>=0) {
        wideChoruses[0].setRate(currentRate);wideChoruses[0].setDepth(currentDepth);
        wideChoruses[0].setCentreDelay(currentDelay);wideChoruses[0].setFeedback(currentFeedback);wideChoruses[0].setMix(1);
        wideChoruses[1].setRate(currentRate*1.11);wideChoruses[1].setDepth(std::clamp(currentDepth*.96, 0.0, 1.0));
        wideChoruses[1].setCentreDelay(std::clamp(currentDelay*1.18, 1.0, 40.0));wideChoruses[1].setFeedback(currentFeedback*.9);wideChoruses[1].setMix(1);
    }
    updateOutputFilters(currentHiCut, currentLoCut);
    const auto blend = consumeSmoothedValue(nativeMixBlend, parameterUpdateBlockSize);
    const auto wet = .5f - .5f * std::cos(std::numbers::pi_v<float> * currentMix);
    appliedDryGain = (1.0f-currentMix) + blend * (currentMix-wet*wet);
    appliedWetGain = currentMix + blend * (wet-currentMix);
    return currentMix;
}
}
