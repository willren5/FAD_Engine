#include <fad/engine/fx/FilterFx.h>
#include <fad/engine/filters/SerumFilterDrive.h>
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
constexpr int coefficientUpdateIntervalSamples = 16;

bool coefficientsNeedUpdate(float previousValue, float nextValue) noexcept
{
    return std::isnan(previousValue) || std::abs(previousValue - nextValue) > 1.0e-6f;
}
}

void FilterFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    nativeHoldIdleLimit = std::max(1, fad::engine::math::roundToNearestInt(sampleRate * (1504.0 / 48000.0)));

    cutoff.reset(sampleRate, smoothingSeconds);
    resonance.reset(sampleRate, smoothingSeconds);
    driveGain.reset(sampleRate, smoothingSeconds);
    hiCut.reset(sampleRate, smoothingSeconds);
    loCut.reset(sampleRate, smoothingSeconds);
    for(auto* control:{&nativeVariation,&nativeStereo,&nativeWet,&nativeLevel,&nativeResonance,&nativeCutoff})
        control->reset(sampleRate,smoothingSeconds);
    nativeVariation.setCurrentAndTargetValue(.5f);nativeStereo.setCurrentAndTargetValue(0);
    nativeWet.setCurrentAndTargetValue(1);nativeLevel.setCurrentAndTargetValue(.5f);
    nativeResonance.setCurrentAndTargetValue(-1);nativeCutoff.setCurrentAndTargetValue(0);
    nativeModel.store(-1,std::memory_order_relaxed);lastNativeModel=-1;
    for(auto& filter:nativeFilters)filter.prepare(sampleRate * 2.0);
    for(auto& filter:nativeScreamFilters)filter.prepare(sampleRate);
    for(auto& filter:nativeCombFilters)filter.prepare(sampleRate);

    modeFadeSamples = std::clamp(fad::engine::math::roundToNearestInt(static_cast<float>(sampleRate) * 0.003f), 16, 256);

    for (auto& f : mainFilter)
        f.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& f : outputHP)
        f.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& f : outputLP)
        f.prepare({sampleRate, spec.maximumBlockSize, 1u});

    cutoff.setCurrentAndTargetValue(20000.0f);
    resonance.setCurrentAndTargetValue(0.7f);
    driveGain.setCurrentAndTargetValue(1.0f);
    hiCut.setCurrentAndTargetValue(20000.0f);
    loCut.setCurrentAndTargetValue(20.0f);
    filterMode.store(0, std::memory_order_relaxed);
    lastAppliedCutoffHz = std::numeric_limits<float>::quiet_NaN();
    lastAppliedResonance = std::numeric_limits<float>::quiet_NaN();
    lastAppliedHiCutHz = std::numeric_limits<float>::quiet_NaN();
    lastAppliedLoCutHz = std::numeric_limits<float>::quiet_NaN();
    lastAppliedFilterMode = -1;
    parametersPrimed = false;
    reset();
}

void FilterFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
{
    if (bypassed.load(std::memory_order_relaxed))
        return;

    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());
    if (blockChannels <= 0 || blockChannels > numChannels || numSamples <= 0)
        return;

    const auto model=nativeModel.load(std::memory_order_relaxed);
    if(model!=lastNativeModel) {
        if(hasProcessedSample) {modeFadeStartSamples=lastOutputSamples;modeFadeRemaining=modeFadeSamples;}
        coefficientUpdateSamplesRemaining=0;
        resetNativeHold();
        lastNativeModel=model;
    }
    parametersPrimed = true;
    if(model>=0) {processNativeBlock(outputBlock);return;}


    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto cutoffHz = utils::sanitizeFinite(cutoff.getNextValue(), 20000.0f);
        const auto resonanceValue = utils::sanitizeFinite(resonance.getNextValue(), 0.7f);
        const auto gain = utils::sanitizeFinite(driveGain.getNextValue(), 1.0f);
        const auto hiCutHz = utils::sanitizeFinite(hiCut.getNextValue(), 20000.0f);
        const auto loCutHz = utils::sanitizeFinite(loCut.getNextValue(), 20.0f);
        const auto currentFilterMode = filterMode.load(std::memory_order_relaxed);
        const auto modeChanged = currentFilterMode != lastAppliedFilterMode;
        if (coefficientUpdateSamplesRemaining <= 0
            || modeChanged)
        {
            const bool shouldUpdateCoefficients =
                currentFilterMode != lastAppliedFilterMode
                || coefficientsNeedUpdate(lastAppliedCutoffHz, cutoffHz)
                || coefficientsNeedUpdate(lastAppliedResonance, resonanceValue)
                || coefficientsNeedUpdate(lastAppliedHiCutHz, hiCutHz)
                || coefficientsNeedUpdate(lastAppliedLoCutHz, loCutHz);

            if (shouldUpdateCoefficients)
            {
                if (modeChanged && lastAppliedFilterMode >= 0 && hasProcessedSample)
                {
                    modeFadeStartSamples = lastOutputSamples;
                    modeFadeRemaining = modeFadeSamples;
                }
                updateFilterParams(currentFilterMode,
                                   cutoffHz,
                                   resonanceValue,
                                   hiCutHz,
                                   loCutHz);
                lastAppliedCutoffHz = cutoffHz;
                lastAppliedResonance = resonanceValue;
                lastAppliedHiCutHz = hiCutHz;
                lastAppliedLoCutHz = loCutHz;
                lastAppliedFilterMode = currentFilterMode;
            }

            coefficientUpdateSamplesRemaining = coefficientUpdateIntervalSamples;
        }
        --coefficientUpdateSamplesRemaining;

        for (int channel = 0; channel < blockChannels; ++channel)
        {
            const auto ch = static_cast<size_t>(channel);
            auto sample = utils::sanitizeFinite(outputBlock.getChannelPointer(ch)[sampleIndex]);

            // Drive stage
            sample = std::tanh(sample * gain);

            // Main filter
            sample = mainFilter[ch].processSample(0, sample);
            mainFilter[ch].snapToZero();

            // Output HP (lo cut)
            sample = outputHP[ch].processSample(0, sample);
            outputHP[ch].snapToZero();

            // Output LP (hi cut)
            sample = outputLP[ch].processSample(0, sample);
            outputLP[ch].snapToZero();
            if (! std::isfinite(sample))
            {
                mainFilter[ch].reset();
                outputHP[ch].reset();
                outputLP[ch].reset();
                sample = 0.0f;
            }

            if (modeFadeRemaining > 0)
            {
                const auto fadeOut = static_cast<float>(modeFadeRemaining)
                    / static_cast<float>(modeFadeSamples);
                sample = modeFadeStartSamples[ch] * fadeOut + sample * (1.0f - fadeOut);
            }
            sample = utils::sanitizeFinite(sample);
            outputBlock.getChannelPointer(ch)[sampleIndex] = sample;
            lastOutputSamples[ch] = sample;
        }

        hasProcessedSample = true;
        if (modeFadeRemaining > 0)
            --modeFadeRemaining;
    }
}

void FilterFxCore::processNativeBlock(const fad::engine::AudioBlock<float>& block)
{
    const int channels=static_cast<int>(block.getNumChannels());
    const int count=static_cast<int>(block.getNumSamples());
    const int model=nativeModel.load(std::memory_order_relaxed);
    for(int sample=0;sample<count;++sample)
    {
        const auto hz=cutoff.getNextValue();
        const auto q=std::max(.1f, resonance.getNextValue());
        const auto gain=driveGain.getNextValue();
        hiCut.getNextValue();loCut.getNextValue(); // keep legacy controls current
        const auto variation=nativeVariation.getNextValue();
        const auto stereo=nativeStereo.getNextValue();
        const auto wet=std::clamp(nativeWet.getNextValue(), 0.0f, 1.0f);
        const auto level=std::clamp(nativeLevel.getNextValue(), 0.0f, 1.0f);
        const auto resonanceOverride=nativeResonance.getNextValue();
        const auto cutoffOverride=nativeCutoff.getNextValue();
        if(coefficientUpdateSamplesRemaining<=0)
        {
            parameters::FilterSnapshot state;
            state.nativeModel=model;state.nativeVariation=std::clamp(variation+nativeModulation[3], 0.0f, 1.0f);
            state.mode=model<=7?parameters::FilterMode::lowPass:
                (model<=11||model==24||model==25)?parameters::FilterMode::highPass:parameters::FilterMode::bandPass;
            const auto root=std::clamp(1.0f-1.0f/(2.0f*q), 0.0f, 1.0f);
            const auto r=resonanceOverride>=0?resonanceOverride:root*root*root*root;
            state.resonance=.1f+.87f*std::clamp(r+nativeModulation[1], 0.0f, 1.0f);
            state.driveDb=fad::engine::math::gainToDecibels(std::max(1.0f, gain));
            if (nativeModulation[2] != 0.0f) {
                const auto rawDrive=std::pow(std::max(1.0f, gain),.25f)-1.0f;
                state.driveDb=80.0f*std::log10(1.0f+std::clamp(rawDrive+nativeModulation[2], 0.0f, 1.0f));
            }
            auto centre=cutoffOverride>0?cutoffOverride:hz;
            if (nativeModulation[0] != 0.0f) {
                constexpr auto minimum=8.1757989156f;
                const auto raw=std::log2(std::max(minimum, centre)/minimum)*(12.0f/128.0f);
                centre=minimum*std::exp2(std::clamp(raw+nativeModulation[0], 0.0f, 1.0f)*(128.0f/12.0f));
            }
            const auto ratio=std::exp2(std::clamp(stereo, -32.0f, 32.0f)/12.0f);
            nativeInputGain=fad::engine::math::gainFromDecibels(state.driveDb);
            if (model == 79)
            {
                // Independent native hold boundaries show a 1/64-semitone
                // frequency grid and a clock at twice the displayed frequency.
                // Keep fractional carry: rounding the period drifts in long notes.
                constexpr double midiZeroHz = 8.1757989156;
                const std::array<float, 2> frequencies{centre / ratio, centre * ratio};
                for (size_t channel = 0; channel < frequencies.size(); ++channel)
                {
                    const auto frequency = utils::sanitizeFiniteClamped(frequencies[channel], 440.0f, 1.0f, 22050.0f);
                    const auto midi64 = std::round(768.0 * std::log2(static_cast<double>(frequency) / midiZeroHz));
                    const auto clockHz = 2.0 * midiZeroHz * std::exp2(midi64 / 768.0);
                    nativeHoldIncrement[channel] = std::clamp(clockHz / sampleRate, 0.0, 1.0);
                }
            }
            else if (model == 51 || model == 52)
            {
                const auto rawResonance = std::clamp(r + nativeModulation[1], 0.0f, 1.0f);
                nativeCombFilters[0].setParameters(centre / ratio, rawResonance, state.driveDb, state.nativeVariation, model == 52);
                nativeCombFilters[1].setParameters(centre * ratio, rawResonance, state.driveDb, state.nativeVariation, model == 52);
            }
            else if (model == 94 || model == 95)
            {
                const auto rawDrive = std::pow(static_cast<double>(nativeInputGain), .25) - 1.0;
                const auto rawResonance = std::clamp(r + nativeModulation[1], 0.0f, 1.0f);
                nativeScreamFilters[0].setParameters(centre / ratio, rawResonance,
                    rawDrive, state.nativeVariation, model == 95);
                nativeScreamFilters[1].setParameters(centre * ratio, rawResonance,
                    rawDrive, state.nativeVariation, model == 95);
            }
            else
            {
                for(auto& filter:nativeFilters) filter.setNativeInputPreShaped(model>=4 && model<=11);
                state.cutoffHz=centre/ratio;nativeFilters[0].updateCoefficients(state,coefficientUpdateIntervalSamples);
                state.cutoffHz=centre*ratio;nativeFilters[1].updateCoefficients(state,coefficientUpdateIntervalSamples);
            }
            coefficientUpdateSamplesRemaining=coefficientUpdateIntervalSamples;
        }
        --coefficientUpdateSamplesRemaining;
        const auto outputGain=4.0f*level*level;
        // Independent sweeps across every supported family establish the
        // same raised-cosine wet gain and squared dry attenuation. Scream's
        // dry branch has an additional measured 0.5 gain, even at zero Mix.
        const auto wetAmount = .5f - .5f * std::cos(std::numbers::pi_v<float> * wet);
        const auto dryAmount = (1.0f - wetAmount * wetAmount)
            * ((model == 94 || model == 95) ? .5f : 1.0f);
        bool advanceHold = false;
        if (model == 79)
        {
            bool signal = nativeActivityKnown && nativeVoiceActive;
            for (int channel = 0; channel < channels; ++channel)
                signal = signal || std::abs(utils::sanitizeFinite(block.getChannelPointer(static_cast<size_t>(channel))[sample])) > 1.e-10f;
            advanceHold = signal || nativeHoldIdleSamples < nativeHoldIdleLimit;
            nativeHoldIdleSamples = signal ? 0 : std::min(nativeHoldIdleLimit, nativeHoldIdleSamples + 1);
        }
        for(int channel=0;channel<channels;++channel)
        {
            auto* data=block.getChannelPointer(static_cast<size_t>(channel));
            const auto dry=utils::sanitizeFinite(data[sample]);
            float value;
            if (model == 79)
            {
                const auto ch = static_cast<size_t>(channel);
                const auto shaped = filters::serumFilterDrive(dry * nativeInputGain, 4.0f);
                if (advanceHold)
                {
                    if (nativeHoldPhase[ch] >= 1.0)
                    {
                        nativeHoldPhase[ch] -= 1.0;
                        nativeHeldSample[ch] = shaped;
                    }
                    nativeHoldPhase[ch] += nativeHoldIncrement[ch];
                }
                // SampHold- is the driven signal MINUS its held value, not the
                // positive hold/bitcrusher.
                const auto filtered = shaped - nativeHeldSample[ch];
                value = (dry * dryAmount + filtered * wetAmount) * outputGain;
            }
            else if (model == 51 || model == 52)
            {
                const auto filtered = nativeCombFilters[static_cast<size_t>(channel)].processSample(dry);
                value = (dry * dryAmount + wetAmount * filtered) * outputGain;
            }
            else if (model == 94 || model == 95)
            {
                // This model contains its own 2x SVF but its feedback delay and
                // all-pass circuit operate once per host sample.
                const auto filtered = nativeScreamFilters[static_cast<size_t>(channel)].processSample(dry);
                value = (dry * dryAmount + wetAmount * filtered) * outputGain;
            }
            else
            {
                // Native complementary Low 6 / High 6 probes sum to
                // 0.75*x[n] + 0.25*x[n-1]: linear 2x interpolation and averaging.
                auto& filter = nativeFilters[static_cast<size_t>(channel)];
                auto& previous = nativePreviousInput[static_cast<size_t>(channel)];
                const auto input = model>=4 && model<=11 ? filters::serumFilterDrive(dry*nativeInputGain,4.0f) : dry;
                const auto first = filter.processSample(.5f * (previous + input));
                const auto filtered = .5f * (first + filter.processSample(input));
                previous = input;
                value = (dry * dryAmount + wetAmount * filtered) * outputGain;
            }
            if(modeFadeRemaining>0) {
                const auto fade=static_cast<float>(modeFadeRemaining)/static_cast<float>(modeFadeSamples);
                value=modeFadeStartSamples[static_cast<size_t>(channel)]*fade+value*(1-fade);
            }
            data[sample]=utils::sanitizeFinite(value);
            lastOutputSamples[static_cast<size_t>(channel)]=data[sample];
        }
        hasProcessedSample=true;
        if(modeFadeRemaining>0)--modeFadeRemaining;
    }
}

void FilterFxCore::reset()
{
    nativeModulation.fill(0);
    nativeActivityKnown = false;
    nativeVoiceActive = false;
    nativePreviousInput.fill(0);
    resetNativeHold();
    lastNativeModel = -1;
    for(auto& filter:nativeFilters)filter.reset();
    for(auto* control:{&nativeVariation,&nativeStereo,&nativeWet,&nativeLevel,&nativeResonance,&nativeCutoff})
        control->setCurrentAndTargetValue(control->getTargetValue());
    parametersPrimed = false;
    cutoff.setCurrentAndTargetValue(cutoff.getTargetValue());
    resonance.setCurrentAndTargetValue(resonance.getTargetValue());
    driveGain.setCurrentAndTargetValue(driveGain.getTargetValue());
    hiCut.setCurrentAndTargetValue(hiCut.getTargetValue());
    loCut.setCurrentAndTargetValue(loCut.getTargetValue());

    for (auto& f : mainFilter)
        f.reset();

    for (auto& f : outputHP)
        f.reset();

    for (auto& f : outputLP)
        f.reset();

    lastAppliedCutoffHz = std::numeric_limits<float>::quiet_NaN();
    lastAppliedResonance = std::numeric_limits<float>::quiet_NaN();
    lastAppliedHiCutHz = std::numeric_limits<float>::quiet_NaN();
    lastAppliedLoCutHz = std::numeric_limits<float>::quiet_NaN();
    lastAppliedFilterMode = -1;
    const auto currentFilterMode = filterMode.load(std::memory_order_relaxed);
    const auto cutoffTarget = cutoff.getTargetValue();
    const auto resonanceTarget = resonance.getTargetValue();
    const auto hiCutTarget = hiCut.getTargetValue();
    const auto loCutTarget = loCut.getTargetValue();
    updateFilterParams(currentFilterMode,
                       cutoffTarget,
                       resonanceTarget,
                       hiCutTarget,
                       loCutTarget);
    lastAppliedCutoffHz = cutoffTarget;
    lastAppliedResonance = resonanceTarget;
    lastAppliedHiCutHz = hiCutTarget;
    lastAppliedLoCutHz = loCutTarget;
    lastAppliedFilterMode = currentFilterMode;
    coefficientUpdateSamplesRemaining = coefficientUpdateIntervalSamples;
    modeFadeRemaining = 0;
    modeFadeStartSamples = {};
    lastOutputSamples = {};
    hasProcessedSample = false;
}

void FilterFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        nativeActivityKnown = false;
        nativeVoiceActive = false;
        for(auto& filter:nativeFilters)filter.reset();
        nativePreviousInput.fill(0);
        resetNativeHold();
        lastNativeModel=-1;
        for (auto& f : mainFilter)
            f.reset();
        for (auto& f : outputHP)
            f.reset();
        for (auto& f : outputLP)
            f.reset();

        lastAppliedCutoffHz = std::numeric_limits<float>::quiet_NaN();
        lastAppliedResonance = std::numeric_limits<float>::quiet_NaN();
        lastAppliedHiCutHz = std::numeric_limits<float>::quiet_NaN();
        lastAppliedLoCutHz = std::numeric_limits<float>::quiet_NaN();
        lastAppliedFilterMode = -1;
        coefficientUpdateSamplesRemaining = 0;
        modeFadeRemaining = 0;
        hasProcessedSample = false;
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void FilterFxCore::resetNativeHold() noexcept
{
    for(auto& filter:nativeScreamFilters)filter.reset();
    for(auto& filter:nativeCombFilters)filter.reset();
    nativeHoldPhase.fill(0.0);
    nativeHoldIncrement.fill(0.0);
    nativeHeldSample.fill(0.0f);
    nativeHoldIdleSamples = 0;
}

void FilterFxCore::setGenericParam(int index, float value01)
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
        case 0: // Cutoff (20-20000 Hz, skew 0.3)
        {
            const auto freq = fad::engine::math::RuntimeRange(20.0f, 20000.0f, 0.0f, 0.3f)
                                  .denormalise(safeValue01);
            setSmoothedValue(cutoff, freq);
            break;
        }
        case 1: // Resonance (0.1-10.0, skew 0.5)
        {
            const auto res = fad::engine::math::RuntimeRange(0.1f, 10.0f, 0.0f, 0.5f)
                                 .denormalise(safeValue01);
            setSmoothedValue(resonance, res);
            break;
        }
        case 2: // Mode (0-2 discrete: LP/BP/HP)
            filterMode.store(std::clamp(fad::engine::math::roundToNearestInt(safeValue01 * 2.0f), 0, 2), std::memory_order_relaxed);
            break;
        case 3: // Drive (0-30 dB)
        {
            const auto driveDb = fad::engine::math::interpolate(safeValue01, 0.0f, 30.0f);
            setSmoothedValue(driveGain, fad::engine::math::gainFromDecibels(driveDb));
            break;
        }
        case 4: // Hi Cut (1000-20000 Hz, skew 0.3)
        {
            const auto freq = fad::engine::math::RuntimeRange(1000.0f, 20000.0f, 0.0f, 0.3f)
                                  .denormalise(safeValue01);
            setSmoothedValue(hiCut, freq);
            break;
        }
        case 5: // Lo Cut (20-4000 Hz, skew 0.3)
        {
            const auto freq = fad::engine::math::RuntimeRange(20.0f, 4000.0f, 0.0f, 0.3f)
                                  .denormalise(safeValue01);
            setSmoothedValue(loCut, freq);
            break;
        }
        case 6: nativeModel.store(std::clamp(fad::engine::math::roundToNearestInt(safeValue01*96)-1, -1, 95),std::memory_order_relaxed);break;
        case 7: setSmoothedValue(nativeVariation,safeValue01);break;
        case 8: setSmoothedValue(nativeStereo,(safeValue01-.5f)*64.0f);break;
        case 9: setSmoothedValue(nativeWet,safeValue01);break;
        case 10: setSmoothedValue(nativeLevel,safeValue01);break;
        case 11: setSmoothedValue(nativeResonance,safeValue01*2.0f-1.0f);break;
        case 12: setSmoothedValue(nativeCutoff,fad::engine::math::RuntimeRange(0,22050,0,.3f).denormalise(safeValue01));break;
        default: break;
    }
}

void FilterFxCore::updateFilterParams(int filterModeValue,
                                  float cutoffValue,
                                  float resonanceValue,
                                  float hiCutValue,
                                  float loCutValue) noexcept
{
    const auto nyquistSafe =
        static_cast<float>(std::max(100.0, sampleRate * 0.49));

    const auto cutoffHz = std::clamp(cutoffValue, 20.0f, nyquistSafe);
    // Cap resonance at 5.0 to prevent self-oscillation and ear-piercing squeals
    const auto res = std::clamp(resonanceValue, 0.1f, 5.0f);
    auto hiCutHz = std::clamp(hiCutValue, 100.0f, nyquistSafe);
    auto loCutHz = std::clamp(loCutValue, 20.0f, std::min(4000.0f, nyquistSafe - 20.0f));
    if (loCutHz >= hiCutHz)
    {
        loCutHz = 20.0f;
        hiCutHz = std::min(8000.0f, nyquistSafe);
    }
    else
    {
        loCutHz = std::clamp(loCutHz, 20.0f, std::min(4000.0f, hiCutHz - 20.0f));
        hiCutHz = std::clamp(hiCutHz, loCutHz + 20.0f, nyquistSafe);
    }

    fad::engine::filters::TptMode mainType;
    switch (filterModeValue)
    {
        case 1:  mainType = fad::engine::filters::TptMode::bandpass; break;
        case 2:  mainType = fad::engine::filters::TptMode::highpass; break;
        case 0:
        default: mainType = fad::engine::filters::TptMode::lowpass;  break;
    }

    for (auto& f : mainFilter)
    {
        f.setType(mainType);
        f.setCutoffFrequency(cutoffHz);
        f.setResonance(res);
    }

    for (auto& f : outputHP)
    {
        f.setType(fad::engine::filters::TptMode::highpass);
        f.setCutoffFrequency(loCutHz);
        f.setResonance(0.707f);
    }

    for (auto& f : outputLP)
    {
        f.setType(fad::engine::filters::TptMode::lowpass);
        f.setCutoffFrequency(hiCutHz);
        f.setResonance(0.707f);
    }
}
}
