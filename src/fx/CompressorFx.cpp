#include <fad/engine/fx/CompressorFx.h>
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
constexpr auto paramSmoothingSeconds = 0.03;
constexpr auto mixSmoothingSeconds = 0.02;

using utils::isFinite;
using utils::sanitizeFinite;
}

void CompressorFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    dryBuffer.setSize(numChannels, static_cast<int>(spec.maximumBlockSize), false, false, true);

    threshold.reset(sampleRate, paramSmoothingSeconds);
    ratio.reset(sampleRate, paramSmoothingSeconds);
    attackMs.reset(sampleRate, paramSmoothingSeconds);
    releaseMs.reset(sampleRate, paramSmoothingSeconds);
    makeupGainDb.reset(sampleRate, paramSmoothingSeconds);
    mix.reset(sampleRate, mixSmoothingSeconds);
    kneeDb.reset(sampleRate, paramSmoothingSeconds);
    constexpr std::array<float,8> defaults{-18.0618f,.75f,90.091f,90.01f,0.0f,1.0f,1.0f,1.0f};
    for (size_t i=0;i<nativeControls.size();++i)
    {
        nativeControls[i].reset(sampleRate,paramSmoothingSeconds);
        nativeControls[i].setCurrentAndTargetValue(defaults[i]);
    }
    nativeMode = 0;
    nativeDcPole = std::exp(-(2 * std::numbers::pi_v<double>) * 12.0 / sampleRate);
    nativeRmsPole = std::exp(-1.0 / (sampleRate * .005));
    const fad::engine::ProcessSpec splitSpec{sampleRate, spec.maximumBlockSize,
        static_cast<uint32_t>(numChannels)};
    for(auto& split:nativeCrossovers)split.prepare(splitSpec);
    nativeCrossovers[0].setCutoffFrequency(std::min(128.3, sampleRate*.45));
    nativeCrossovers[1].setCutoffFrequency(std::min(2500.0, sampleRate*.45));

    reset();
}

void CompressorFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
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

void CompressorFxCore::processActiveBlock(fad::engine::ProcessContextReplacing<float>& context)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());

    if (blockChannels <= 0 || blockChannels > dryBuffer.getNumChannels() || numSamples > dryBuffer.getNumSamples())
        return;

    if (nativeMode == 1)
    {
        processNativeNormalBlock(context);
        return;
    }
    if (nativeMode == 2)
    {
        processNativeMultibandBlock(context);
        return;
    }

    for (int channel = 0; channel < blockChannels; ++channel)
        std::copy_n(outputBlock.getChannelPointer(static_cast<size_t>(channel)), numSamples, dryBuffer.getWritePointer(channel));

    // Pre-compute exp() coefficients outside the loop when smoothers are stable.
    // exp() only needs recalculation when attack/release params are actively changing.
    const bool smoothersActive = attackMs.isSmoothing() || releaseMs.isSmoothing();
    const auto srFloat = static_cast<float>(sampleRate);
    const auto initialAttackMs = std::max(0.1f, sanitizeFinite(attackMs.getTargetValue(), 10.0f));
    const auto initialReleaseMs = std::max(0.1f, sanitizeFinite(releaseMs.getTargetValue(), 100.0f));
    float cachedAttackCoeff  = std::exp(-1.0f / (initialAttackMs * 0.001f * srFloat));
    float cachedReleaseCoeff = std::exp(-1.0f / (initialReleaseMs * 0.001f * srFloat));

    const auto resetSmoothedParameters = [this]() noexcept
    {
        threshold.setCurrentAndTargetValue(sanitizeFinite(threshold.getTargetValue(), -18.0f));
        ratio.setCurrentAndTargetValue(std::max(1.0f, sanitizeFinite(ratio.getTargetValue(), 4.0f)));
        attackMs.setCurrentAndTargetValue(std::max(0.1f, sanitizeFinite(attackMs.getTargetValue(), 10.0f)));
        releaseMs.setCurrentAndTargetValue(std::max(0.1f, sanitizeFinite(releaseMs.getTargetValue(), 100.0f)));
        makeupGainDb.setCurrentAndTargetValue(sanitizeFinite(makeupGainDb.getTargetValue(), 0.0f));
        mix.setCurrentAndTargetValue(std::clamp(sanitizeFinite(mix.getTargetValue(), 1.0f), 0.0f, 1.0f));
        kneeDb.setCurrentAndTargetValue(std::max(0.0f, sanitizeFinite(kneeDb.getTargetValue(), 6.0f)));
        envelopeL = 1.0f;
        envelopeR = 1.0f;
    };

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        auto currentThreshold = threshold.getNextValue();
        auto currentRatio     = ratio.getNextValue();
        auto currentAttack    = attackMs.getNextValue();
        auto currentRelease   = releaseMs.getNextValue();
        auto currentMakeup    = makeupGainDb.getNextValue();
        auto currentMix       = mix.getNextValue();
        auto currentKnee      = kneeDb.getNextValue();
        if (! isFinite(currentThreshold)
            || ! isFinite(currentRatio)
            || ! isFinite(currentAttack)
            || ! isFinite(currentRelease)
            || ! isFinite(currentMakeup)
            || ! isFinite(currentMix)
            || ! isFinite(currentKnee))
        {
            resetSmoothedParameters();
            currentThreshold = threshold.getCurrentValue();
            currentRatio = ratio.getCurrentValue();
            currentAttack = attackMs.getCurrentValue();
            currentRelease = releaseMs.getCurrentValue();
            currentMakeup = makeupGainDb.getCurrentValue();
            currentMix = mix.getCurrentValue();
            currentKnee = kneeDb.getCurrentValue();
        }

        currentMix = std::clamp(sanitizeFinite(currentMix, 1.0f), 0.0f, 1.0f);
        currentKnee = std::max(0.0f, sanitizeFinite(currentKnee, 6.0f));
        const auto dry              = 1.0f - currentMix;
        const auto safeRatio        = std::max(1.0f, currentRatio);
        const auto safeAttackMs     = std::max(0.1f, currentAttack);
        const auto safeReleaseMs    = std::max(0.1f, currentRelease);

        if (smoothersActive)
        {
            cachedAttackCoeff  = std::exp(-1.0f / (safeAttackMs * 0.001f * srFloat));
            cachedReleaseCoeff = std::exp(-1.0f / (safeReleaseMs * 0.001f * srFloat));
        }
        const auto attackCoeff  = sanitizeFinite(cachedAttackCoeff, 0.99f);
        const auto releaseCoeff = sanitizeFinite(cachedReleaseCoeff, 0.99f);
        const auto makeupLinear = sanitizeFinite(fad::engine::math::gainFromDecibels(currentMakeup), 1.0f);
        const auto halfKnee     = currentKnee * 0.5f;
        const auto invRatio     = 1.0f - 1.0f / safeRatio;

        float detectorPeak = 0.0f;
        for (int channel = 0; channel < blockChannels; ++channel)
        {
            const auto inputSample =
                sanitizeFinite(outputBlock.getChannelPointer(static_cast<size_t>(channel))[sampleIndex]);
            detectorPeak = std::max(detectorPeak, std::abs(inputSample));
        }

        const auto inputDb = fad::engine::math::gainToDecibels(detectorPeak + 1e-10f);
        const auto overshoot = inputDb - currentThreshold;

        float gainReductionDb;
        if (currentKnee <= 1.0e-6f)
        {
            gainReductionDb = overshoot > 0.0f ? overshoot * invRatio : 0.0f;
        }
        else if (overshoot <= -halfKnee)
        {
            gainReductionDb = 0.0f;
        }
        else if (overshoot >= halfKnee)
        {
            gainReductionDb = overshoot * invRatio;
        }
        else
        {
            const auto t = overshoot + halfKnee;
            gainReductionDb = (t * t) / (2.0f * currentKnee) * invRatio;
        }

        const auto targetGainLinear =
            sanitizeFinite(fad::engine::math::gainFromDecibels(-gainReductionDb), 1.0f);

        auto& envelope = envelopeL;
        const auto coeff = (targetGainLinear < envelope) ? attackCoeff : releaseCoeff;
        if (! isFinite(envelope))
            envelope = 1.0f;
        envelope = targetGainLinear + coeff * (envelope - targetGainLinear);
        fad::engine::math::snapSmallState(envelope);
        envelopeR = envelope;

        const auto gain = sanitizeFinite(envelope * makeupLinear, 1.0f);
        for (int channel = 0; channel < blockChannels; ++channel)
        {
            const auto inputSample =
                sanitizeFinite(outputBlock.getChannelPointer(static_cast<size_t>(channel))[sampleIndex]);
            const auto drySample = sanitizeFinite(dryBuffer.getSample(channel, sampleIndex));

            outputBlock.getChannelPointer(static_cast<size_t>(channel))[sampleIndex] =
                sanitizeFinite(drySample * dry + inputSample * gain * currentMix, drySample);
        }
    }
    publishedGainReductionDb.store(envelopeL < 1.0f
        ? -20.0f * std::log10(std::max(1.0e-9f, envelopeL)) : 0.0f,
        std::memory_order_relaxed);
}

float CompressorFxCore::modulatedNativeThreshold() const noexcept
{
    const auto db=utils::sanitizeFiniteClamped(nativeControls[0].getCurrentValue(),-18.0618f,-120.0f,0.0f);
    if (appliedNativeModulation[0] == 0.0f) return db;
    const auto raw=std::clamp(1.0f-std::pow(10.0f,db/60.0f)+appliedNativeModulation[0], 0.0f, 1.0f);
    return 60.0f*std::log10(std::max(.01f, 1.0f-raw));
}

float CompressorFxCore::modulatedNativeMakeup() const noexcept
{
    const auto db=utils::sanitizeFiniteClamped(nativeControls[4].getCurrentValue(),0.0f,0.0f,30.103f);
    // Keep the stored makeup contract unchanged: it encodes the normal 31x law.
    // Native Limit mode uses 63x instead (independent gain and ratio sweeps).
    // Limit's 63x makeup also applies to Multiband. Native sine gain sweeps
    // confirm both modes; limiting this branch to Normal loses up to ~6 dB.
    const bool limiter = nativeMode > 0 && nativeControls[1].getCurrentValue() >= .9811765f;
    if (appliedNativeModulation[1] == 0.0f && !limiter) return db;
    const auto raw=std::clamp(std::sqrt(std::max(0.0f, (std::pow(10.0f,db/20.0f)-1.0f)/31.0f))+appliedNativeModulation[1], 0.0f, 1.0f);
    return 20.0f*std::log10(1.0f+(limiter ? 63.0f : 31.0f)*raw*raw);
}

float CompressorFxCore::modulatedNativeReleaseMs() const noexcept
{
    const auto milliseconds = utils::sanitizeFiniteClamped(nativeControls[3].getCurrentValue(), 90.01f, .1f, 1000.0f);
    if (appliedNativeModulation[3] == 0.0f)
        return milliseconds;
    const auto basePosition = std::sqrt((milliseconds - .1f) / 999.0f);
    const auto position = std::clamp(basePosition + appliedNativeModulation[3], 0.0f, 1.0f);
    return .1f + 999.0f * position * position;
}

void CompressorFxCore::processNativeNormalBlock(fad::engine::ProcessContextReplacing<float>& context) noexcept
{
    auto& block = context.getOutputBlock();
    const auto channels = static_cast<int>(block.getNumChannels());
    const auto samples = static_cast<int>(block.getNumSamples());
    nativeParametersPrimed = true;
    const bool smoothing = std::any_of(nativeControls.begin(),nativeControls.end(),
        [](const auto& value) { return value.isSmoothing(); });
    double thresholdDb=0, ratioAmount=0, attack=0, release=0, makeupDb=0;
    const auto update = [&]() noexcept
    {
        thresholdDb=modulatedNativeThreshold();
        ratioAmount=utils::sanitizeUnitInterval(nativeControls[1].getCurrentValue());
        const auto attackTime=utils::sanitizeFiniteClamped(nativeControls[2].getCurrentValue(),90.091f,.1f,1000.0f);
        const auto releaseTime=modulatedNativeReleaseMs();
        attack=ratioAmount >= .9811765 ? 0.0 : std::exp(-2.0/(sampleRate*attackTime*.001));
        release=std::exp(-1.0/(sampleRate*releaseTime*.001));
        makeupDb=modulatedNativeMakeup();
    };
    update();
    for(int i=0;i<samples;++i)
    {
        if (nativeModulationRemaining <= 0) {
            if (appliedNativeModulation != nativeModulation) {
                appliedNativeModulation = nativeModulation;
                update();
            }
            nativeModulationRemaining = 16;
        }
        --nativeModulationRemaining;
        if(smoothing) {
            for(auto& value:nativeControls)value.getNextValue();
            update();
        }
        std::array<double,2> original{}, wet{};
        double peak=0.0;
        for(int ch=0;ch<channels;++ch) {
            original[static_cast<size_t>(ch)]=sanitizeFinite(block.getChannelPointer(static_cast<size_t>(ch))[i]);
            wet[static_cast<size_t>(ch)]=original[static_cast<size_t>(ch)]*.25;
            peak=std::max(peak, std::abs(wet[static_cast<size_t>(ch)]));
        }
        const auto target=std::max(0.0, 20.0*std::log10(std::max(peak, 1.0e-12))-thresholdDb);
        const auto coefficient=target>nativeReductionDb ? attack : release;
        nativeReductionDb=target+coefficient*(nativeReductionDb-target);
        const auto gain=std::pow(10.0,(makeupDb-ratioAmount*nativeReductionDb)/20.0);
        const auto amount=utils::sanitizeUnitInterval(mix.getNextValue()+appliedNativeModulation[2]);
        for(int ch=0;ch<channels;++ch) {
            const auto index=static_cast<size_t>(ch);
            // Large-input compression and makeup probes place this bound
            // after gain processing, before dry/wet mixing and DC removal.
            const auto processed=std::clamp(wet[index]*gain, -1.0, 1.0)*4.0;
            const auto input=original[index]*(1.0-amount)+processed*amount;
            auto output=input-nativeDcInput[index]+nativeDcPole*nativeDcOutput[index];
            nativeDcInput[index]=input;
            if(!std::isfinite(static_cast<float>(output))) {
                output=0;nativeDcInput[index]=0;nativeReductionDb=0;
            }
            fad::engine::math::snapSmallState(output);
            nativeDcOutput[index]=output;
            block.getChannelPointer(index)[i]=sanitizeFinite(static_cast<float>(output));
        }
    }
    publishedGainReductionDb.store(static_cast<float>(std::max(0.0, ratioAmount * nativeReductionDb)), std::memory_order_relaxed);
}

void CompressorFxCore::processNativeMultibandBlock(fad::engine::ProcessContextReplacing<float>& context) noexcept
{
    auto& block=context.getOutputBlock();
    const auto channels=static_cast<int>(block.getNumChannels());
    const auto samples=static_cast<int>(block.getNumSamples());
    nativeParametersPrimed=true;
    const bool smoothing=std::any_of(nativeControls.begin(),nativeControls.end(),
        [](const auto& value){return value.isSmoothing();});
    std::array<double,3> lower{},upper{};
    double ratioAmount=0,releasePole=0,makeupGain=1;
    constexpr std::array<double,3> lowerOffsets{1,0,1};
    constexpr std::array<double,3> upperGaps{7,11.6,5.3};
    constexpr std::array<double,3> thresholdSpans{135.2,120.8,142.0};
    constexpr std::array<double,3> bandMakeup{1.6982436524617444,1.0,1.6982436524617444};
    constexpr double maximumGain=63.09573444801933;
    std::array<double, 3> lastBandGain{1.0, 1.0, 1.0};
    const auto update=[&]() noexcept {
        const auto thresholdDb=modulatedNativeThreshold();
        const auto thresholdControl=1.0-std::pow(10.0,static_cast<double>(thresholdDb)/60.0);
        ratioAmount=utils::sanitizeUnitInterval(nativeControls[1].getCurrentValue());
        const auto releaseMs=modulatedNativeReleaseMs();
        // The measured Serum 1.368 peak-release coefficient uses a fixed
        // 44.1 kHz reference; the preceding RMS window uses the actual rate.
        releasePole=std::exp(-1.0/(44.1*static_cast<double>(releaseMs)));
        makeupGain=std::pow(10.0,modulatedNativeMakeup()/20.0);
        for(size_t band=0;band<3;++band) {
            const auto bandThreshold=utils::sanitizeFiniteClamped(nativeControls[band+5].getCurrentValue(),1.0f,0.0f,2.0f);
            const auto offset=lowerOffsets[band]+thresholdSpans[band]*(.25-thresholdControl*thresholdControl)
                +96.0*thresholdControl*thresholdControl*(static_cast<double>(bandThreshold)-1.0);
            lower[band]=(std::exp(-1.0)/64.0)*std::pow(10.0,offset/20.0);
            upper[band]=lower[band]*std::pow(10.0,upperGaps[band]/20.0);
        }
    };
    update();
    for(int i=0;i<samples;++i) {
        if (nativeModulationRemaining <= 0) {
            if (appliedNativeModulation != nativeModulation) {
                appliedNativeModulation = nativeModulation;
                update();
            }
            nativeModulationRemaining = 16;
        }
        --nativeModulationRemaining;
        if(smoothing) {for(auto& value:nativeControls)value.getNextValue();update();}
        std::array<double,2> original{},wet{};
        std::array<std::array<double,2>,3> bands{};
        bool bandsFinite=true;
        for(int ch=0;ch<channels;++ch) {
            const auto index=static_cast<size_t>(ch);
            original[index]=sanitizeFinite(block.getChannelPointer(index)[i]);
            double remainder=0;
            nativeCrossovers[0].processSample(ch,original[index]*.25,bands[0][index],remainder);
            nativeCrossovers[1].processSample(ch,remainder,bands[1][index],bands[2][index]);
            for(const auto& band:bands)bandsFinite=bandsFinite&&std::isfinite(band[index]);
        }
        if(!bandsFinite) {
            for(auto& split:nativeCrossovers)split.reset();
            nativeRmsPower.fill(0);nativePeakEnvelope.fill(0);bands={};
        }
        for(size_t band=0;band<3;++band) {
            double power=0;
            for(int ch=0;ch<channels;++ch)power+=bands[band][static_cast<size_t>(ch)]*bands[band][static_cast<size_t>(ch)];
            power/=channels;
            nativeRmsPower[band]=nativeRmsPole*nativeRmsPower[band]+(1-nativeRmsPole)*power;
            auto& peak=nativePeakEnvelope[band];
            peak=std::max(std::sqrt(std::max(0.0, nativeRmsPower[band])), peak*releasePole);
            double gain=1;
            if(peak<lower[band])gain=peak>0?std::pow(lower[band]/peak,.75):maximumGain;
            else if(peak>upper[band] && ratioAmount>0)gain=std::pow(upper[band]/peak,ratioAmount);
            lastBandGain[band]=std::clamp(gain, .01, maximumGain);
            gain=lastBandGain[band]*bandMakeup[band];
            for(int ch=0;ch<channels;++ch)wet[static_cast<size_t>(ch)]+=bands[band][static_cast<size_t>(ch)]*gain;
        }
        const auto amount=utils::sanitizeUnitInterval(mix.getNextValue()+appliedNativeModulation[2]);
        for(int ch=0;ch<channels;++ch) {
            const auto index=static_cast<size_t>(ch);
            // The multiband wet sum has its own +/-2 bound AFTER makeup.
            const auto processed=std::clamp(wet[index]*makeupGain, -2.0, 2.0)*4.0;
            const auto input=original[index]*(1.0-amount)+processed*amount;
            auto output=input-nativeDcInput[index]+nativeDcPole*nativeDcOutput[index];
            nativeDcInput[index]=input;
            if(!std::isfinite(static_cast<float>(output))) {output=0;nativeDcInput[index]=0;}
            nativeDcOutput[index]=output;
            block.getChannelPointer(index)[i]=static_cast<float>(output);
        }
    }
    const auto minimumGain = std::min(1.0, std::min(lastBandGain[0], std::min(lastBandGain[1], lastBandGain[2])));
    publishedGainReductionDb.store(static_cast<float>(-20.0 * std::log10(minimumGain)),
        std::memory_order_relaxed);
}

void CompressorFxCore::reset()
{
    publishedGainReductionDb.store(0.0f, std::memory_order_relaxed);
    nativeModulation.fill(0); appliedNativeModulation.fill(0); nativeModulationRemaining=0;
    // Only clear DSP history (envelopes), NOT parameter values.
    // Parameters are managed by DynamicFxChain's cached params and
    // will be reapplied via setGenericParam after re-enable.
    // Resetting params here caused "disable→re-enable = dead compressor".
    threshold.setCurrentAndTargetValue(threshold.getTargetValue());
    ratio.setCurrentAndTargetValue(ratio.getTargetValue());
    attackMs.setCurrentAndTargetValue(attackMs.getTargetValue());
    releaseMs.setCurrentAndTargetValue(releaseMs.getTargetValue());
    makeupGainDb.setCurrentAndTargetValue(makeupGainDb.getTargetValue());
    mix.setCurrentAndTargetValue(mix.getTargetValue());
    kneeDb.setCurrentAndTargetValue(kneeDb.getTargetValue());

    envelopeL = 1.0f;
    envelopeR = 1.0f;
    for(auto& value:nativeControls)value.setCurrentAndTargetValue(value.getTargetValue());
    nativeReductionDb=0;
    nativeDcInput.fill(0);nativeDcOutput.fill(0);
    nativeParametersPrimed=false;
    for(auto& split:nativeCrossovers)split.reset();
    nativeRmsPower.fill(0);nativePeakEnvelope.fill(0);
}

void CompressorFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed) publishedGainReductionDb.store(0.0f, std::memory_order_relaxed);
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        envelopeL = 1.0f;
        envelopeR = 1.0f;
        nativeReductionDb=0;nativeDcInput.fill(0);nativeDcOutput.fill(0);
        for(auto& split:nativeCrossovers)split.reset();
        nativeRmsPower.fill(0);nativePeakEnvelope.fill(0);
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void CompressorFxCore::setGenericParam(int index, float value01)
{
    const auto safeValue01 = utils::sanitizeUnitInterval(value01);

    if(index>=7 && index<=15) {
        if(index==7)nativeMode=fad::engine::math::roundToNearestInt(safeValue01*2.0f);
        else {
            auto& parameter=nativeControls[static_cast<size_t>(index-8)];
            const auto value=FxCatalog::denormalizeParamValue(FxType::compressor,index,safeValue01);
            if(nativeParametersPrimed)parameter.setTargetValue(value);
            else parameter.setCurrentAndTargetValue(value);
        }
        return;
    }

    switch (index)
    {
        case 0: // Threshold (-60 to 0 dB)
            threshold.setTargetValue(fad::engine::math::interpolate(safeValue01, -60.0f, 0.0f));
            break;
        case 1: // Ratio (1.0-20.0, skew 0.3)
            ratio.setTargetValue(
                fad::engine::math::RuntimeRange(1.0f, 20.0f, 0.0f, 0.3f).denormalise(safeValue01));
            break;
        case 2: // Attack (0.1-100 ms, skew 0.3)
            attackMs.setTargetValue(
                fad::engine::math::RuntimeRange(0.1f, 100.0f, 0.0f, 0.3f).denormalise(safeValue01));
            break;
        case 3: // Release (10-1000 ms, skew 0.3)
            releaseMs.setTargetValue(
                fad::engine::math::RuntimeRange(10.0f, 1000.0f, 0.0f, 0.3f).denormalise(safeValue01));
            break;
        case 4: // Makeup (0-30 dB)
            makeupGainDb.setTargetValue(fad::engine::math::interpolate(safeValue01, 0.0f, 30.0f));
            break;
        case 5: // Mix (0-1)
            mix.setTargetValue(std::clamp(safeValue01, 0.0f, 1.0f));
            break;
        case 6: // Knee (0-20 dB)
            kneeDb.setTargetValue(fad::engine::math::interpolate(safeValue01, 0.0f, 20.0f));
            break;
        default: break;
    }
}
}
