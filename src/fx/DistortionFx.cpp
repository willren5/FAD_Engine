#include <fad/engine/fx/DistortionFx.h>
#include <fad/engine/fx/SerumZeroSquare.h>
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
constexpr auto mixSmoothingSeconds = 0.03;
constexpr int toneFilterUpdateIntervalSamples = 16;
// Independently measured Tube rational curve; Filter Off uses the shared
// native 2x resampling path, without the legacy tone filters or DC blocker.
float nativeTube(float input, float drive) noexcept
{
    const auto depth = .989f * drive;
    const auto half = .5f * input;
    return half / (1.0f - depth + depth * std::abs(half));
}

float nativeDiode2(float input, float drive) noexcept
{
    // Independent slow-sine/DC transfers collapse in input*Drive. This bounded
    // ninth-order fit preserves the asymmetric interior, including values below
    // -1. The native plateau transition is not a conventional soft clip.
    // Held-out Drive=.7857 verifies the transfer; high-band resampling remains
    // approximate and is measured separately from the static curve.
    const auto z = static_cast<double>(input) * drive;
    if (z > .23032) return .569880426f;
    if (z < -.23032) return -.679857314f;
    constexpr std::array<double, 10> coefficients{
        0.0, 2.4880517853283064, -.24805939286530246,
        -2.584399770556038, .12873310073047597, .8048933008984374,
        -.026423959248776736, -.11825520220931753,
        .0025385406826328185, .008947811477988472};
    const auto t = z / .23;
    auto value = coefficients.back();
    for (int i = 8; i >= 0; --i)
        value = value * t + coefficients[static_cast<size_t>(i)];
    return static_cast<float>(value);
}
// Rational approximation of independently measured native Asym DC transfers.
// Evaluate reciprocal powers at large magnitudes to avoid polynomial overflow.
float nativeAsym(float input, float drive) noexcept
{
    const double z = static_cast<double>(input) * (1.0 + 20.0 * drive * drive);
    if (z < -6.0) return -1.0f;
    constexpr double a1=.6307887926248819, a2=-.004930232287877824,
        a3=.0691301959720909, a4=-.002269137402477469, a5=.0006007097783775488;
    constexpr double b2=.3129128965173957, b4=.00863715072573058, b6=.000028242362235831942;
    const auto magnitude = std::abs(z);
    double value;
    if (magnitude <= 1.0)
    {
        const auto square = z*z;
        value = z*(a1+z*(a2+z*(a3+z*(a4+z*a5)))) / (1.0+square*(b2+square*(b4+square*b6)));
    }
    else
    {
        const auto u=1.0/magnitude, square=u*u, sign=std::copysign(1.0,z);
        value = u*(sign*a5+u*(a4+u*(sign*a3+u*(a2+u*sign*a1)))) / (b6+square*(b4+square*(b2+square)));
    }
    // The native negative branch reaches a measured -1 plateau.
    return static_cast<float>(std::max(-1.0, value));
}
float nativeSoftClip(float input, float drive) noexcept
{
    const auto z = input * (.5f + 1.47455f * drive * drive * drive);
    const auto magnitude = std::abs(z);
    return magnitude <= .5f ? z : std::copysign(1.5f - 1.0f / (.5f + magnitude), z);
}
}

void DistortionFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = fadmodel1::dsp::utils::sanitizeSampleRate(spec.sampleRate);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    dryBuffer.setSize(numChannels, static_cast<int>(spec.maximumBlockSize), false, false, true);
    amount.reset(sampleRate, smoothingSeconds);
    nativeDrive.reset(sampleRate, smoothingSeconds);
    nativePostCutoff.reset(sampleRate, smoothingSeconds);
    nativePostCutoff.setCurrentAndTargetValue(71.0f);
    nativeFilterBandwidth.reset(sampleRate, smoothingSeconds);
    nativeFilterMorphTarget.reset(sampleRate, smoothingSeconds);
    for (auto& filter : nativePlacementFilters) filter.prepare(sampleRate);
    for(auto& filter:nativeHP) {
        filter.prepare({sampleRate*2,1,1});
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setResonance(.70710678f);
    }
    for (auto& filter : nativeLP) {
        filter.prepare({sampleRate * 2, 1, 1});
        filter.setType(fad::engine::filters::TptMode::lowpass);
        filter.setResonance(.70710678f);
    }
    nativePostMorphCoefficient = static_cast<float>(1.0 - std::exp(-1.0 / (.032 * sampleRate)));
    nativeFilterIdleLimit = static_cast<int>(std::llround(sampleRate * (1504.0 / 48000.0)));
    tone.reset(sampleRate, smoothingSeconds);
    nativeOversampling = std::make_unique<fad::engine::filters::Oversample2x>();
    nativeOversampling->prepare(static_cast<uint32_t>(numChannels), fad::engine::filters::preparedAllpass2x(false));
    mix.reset(sampleRate, mixSmoothingSeconds);
    bias.reset(sampleRate, smoothingSeconds);
    modeFadeSamples = std::clamp(fad::engine::math::roundToNearestInt(static_cast<float>(sampleRate) * 0.003f), 16, 256);

    for (auto& filter : preFilters)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& filter : postFilters)
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});

    for (auto& filter : dcBlockers)
    {
        filter.prepare({sampleRate, spec.maximumBlockSize, 1u});
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setCutoffFrequency(5.0f);
        filter.setResonance(0.707f);
    }

    reset();
}

void DistortionFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
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

void DistortionFxCore::processActiveBlock(fad::engine::ProcessContextReplacing<float>& context)
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

    for (int channel = 0; channel < blockChannels; ++channel)
        std::copy_n(outputBlock.getChannelPointer(static_cast<size_t>(channel)), numSamples, dryBuffer.getWritePointer(channel));

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto nextMode = std::clamp(distortionMode.load(std::memory_order_relaxed), 0, 5);
        const auto nextNative = serumSoftClip.load(std::memory_order_relaxed) && nextMode == 0;
        const auto nextZeroSquare = serumZeroSquareEnabled.load(std::memory_order_relaxed);
        const auto nextDownsample = serumDownsample.load(std::memory_order_relaxed);
        const auto nextAsym = serumAsym.load(std::memory_order_relaxed);
        const auto nextTube = serumTube.load(std::memory_order_relaxed);
        const auto nextDiode = serumDiode2.load(std::memory_order_relaxed);
        const auto nextPlacement = nativeFilterPlacement.load(std::memory_order_relaxed);
        const auto nextPost = nativePostHP.load(std::memory_order_relaxed);
        if (nextMode != activeDistortionMode || nextNative != activeSerumSoftClip
            || nextZeroSquare != activeSerumZeroSquare || nextPost != activePostHP || nextTube != activeSerumTube || nextAsym != activeSerumAsym || nextDownsample != activeSerumDownsample
            || nextDiode != activeSerumDiode2 || nextPlacement != activeNativeFilterPlacement)
        {
            activeDistortionMode = nextMode;
            activeSerumSoftClip = nextNative;
            activeSerumTube = nextTube;
            activeSerumDiode2 = nextDiode;
            activeNativeFilterPlacement = nextPlacement;
            for (auto& filter : nativePlacementFilters) filter.reset();
            nativeFilterMorph = 0.0f;
            nativePlacementIdleSamples = 0;
            activeSerumAsym = nextAsym;
            activeSerumDownsample = nextDownsample;
            nativeHoldCounter = nativeHoldIdleSamples = 0; nativeHeldSamples = {};
            nativeOversampling->reset();
            activeSerumZeroSquare = nextZeroSquare;
            activePostHP = nextPost;
            nativePrevious.fill(0);
            nativeAlignment = {}; nativeAlignmentIndex = 0;
            nativeDryAlignment = {}; nativeDryAlignmentIndex = 0;
            for(auto& filter:nativeHP) filter.reset();
            for(auto& filter:nativeLP) filter.reset();
            nativePostMorph = 0.0f; nativeFilterIdleSamples = nativeFilterIdleLimit;
            if (hasProcessedSample)
            {
                modeFadeStartSamples = lastOutputSamples;
                modeFadeRemaining = modeFadeSamples;
            }
        }

        if (nativeModulationRemaining <= 0) {
            appliedNativeModulation = nativeModulation;
            // Native high-rate triangular controls resolve at four host samples.
            // Preserve the legacy/non-Downsample cadence and continuous phase
            // across arbitrary host partitions.
            nativeModulationRemaining = activeSerumDownsample ? 4 : 16;
        }
        --nativeModulationRemaining;
        const auto postHz = nativePostCutoff.getNextValue();
        const auto bandwidth = nativeFilterBandwidth.getNextValue();
        const auto morphTarget = nativeFilterMorphTarget.getNextValue();
        const bool placementEnabled = (activeSerumTube || activeSerumDiode2 || activeSerumAsym)
            && activeNativeFilterPlacement > 0;
        if (placementEnabled)
        {
            bool signal = nativeActivityKnown && nativeVoiceActive;
            for (int channel = 0; channel < blockChannels; ++channel)
                signal = signal || std::abs(dryBuffer.getSample(channel, sampleIndex)) > 1.e-10f;
            if (signal || nativePlacementIdleSamples < nativeFilterIdleLimit)
                nativeFilterMorph += nativePostMorphCoefficient * (morphTarget - nativeFilterMorph);
            nativePlacementIdleSamples = signal ? 0 : std::min(nativeFilterIdleLimit, nativePlacementIdleSamples + 1);
        }
        const bool postHP = activePostHP;
        const auto currentTone =
            fadmodel1::dsp::utils::sanitizeUnitInterval(tone.getNextValue(), 0.5f);
        if (toneFilterUpdateSamplesRemaining <= 0)
        {
            const auto cutoff = std::clamp(postHz, 1.0f, static_cast<float>(sampleRate * .98));
            for (auto& filter : nativeHP) filter.setCutoffFrequency(cutoff);
            for (auto& filter : nativeLP) filter.setCutoffFrequency(cutoff);
            if (placementEnabled)
            {
                auto filterHz = postHz;
                if (appliedNativeModulation[2] != 0.0f)
                {
                    constexpr auto midiZeroHz = 8.1757989156f;
                    const auto position = std::log2(std::max(midiZeroHz, postHz) / midiZeroHz) * (12.0f / 128.0f);
                    filterHz = midiZeroHz * std::exp2(std::clamp(position + appliedNativeModulation[2], 0.0f, 1.0f) * (128.0f / 12.0f));
                }
                for (auto& filter : nativePlacementFilters) filter.setParameters(filterHz, bandwidth);
            }
            updatePreFilters(currentTone);
            updatePostFilters(currentTone);
            toneFilterUpdateSamplesRemaining = toneFilterUpdateIntervalSamples;
        }
        --toneFilterUpdateSamplesRemaining;

        // The native Post morph starts at LP, then approaches its saved HP
        // position. Its control clock suspends after a short silent interval.
        if (activeSerumZeroSquare && postHP) {
            if (nativeActivityKnown && nativeVoiceActive)
                nativeFilterIdleSamples = nativeFilterIdleLimit;
            for (int channel = 0; channel < blockChannels; ++channel)
                if (std::abs(dryBuffer.getSample(channel, sampleIndex)) > 1.e-10f)
                    nativeFilterIdleSamples = nativeFilterIdleLimit;
            if (nativeFilterIdleSamples > 0) {
                nativePostMorph += nativePostMorphCoefficient * (1.0f - nativePostMorph);
                --nativeFilterIdleSamples;
            }
        }
        const auto gain = amount.getNextValue();
        const auto drive = std::clamp(nativeDrive.getNextValue()+appliedNativeModulation[1], 0.0f, 1.0f);
        const auto wetMix = mix.getNextValue()+appliedNativeModulation[0];
        const auto biasOffset = bias.getNextValue();

        if (! std::isfinite(gain)
            || ! std::isfinite(wetMix)
            || ! std::isfinite(biasOffset))
        {
            for (int channel = 0; channel < blockChannels; ++channel)
            {
                preFilters[static_cast<size_t>(channel)].reset();
                postFilters[static_cast<size_t>(channel)].reset();
                dcBlockers[static_cast<size_t>(channel)].reset();
                outputBlock.getChannelPointer(static_cast<size_t>(channel))[sampleIndex] = 0.0f;
            }
            continue;
        }

        const auto safeWetMix = std::clamp(wetMix, 0.0f, 1.0f);
        // Native noise probes confirm the same wet law for SoftClip and Zero-Square.
        const bool nativeMix = activeSerumZeroSquare || activeSerumSoftClip || activeSerumTube || activeSerumAsym || activeSerumDownsample || activeSerumDiode2;
        const auto nativeWet = nativeMix
            ? .5f - .5f * std::cos(std::numbers::pi_v<float> * safeWetMix) : safeWetMix;
        const auto dryMix = nativeMix ? 1.0f - nativeWet * nativeWet : 1.0f - safeWetMix;
        const auto wetGain = nativeMix ? nativeWet : safeWetMix;
        std::array<float, 2> nativeSamples{};
        std::array<float*, 2> nativePointers{&nativeSamples[0], &nativeSamples[1]};
        if (activeSerumDownsample) {
            const auto period = 1 + fad::engine::math::roundToNearestInt(96.0f * drive * drive);
            bool signal = nativeActivityKnown && nativeVoiceActive;
            for (int channel = 0; channel < blockChannels; ++channel)
                signal = signal || dryBuffer.getSample(channel, sampleIndex) != 0.0f;
            const bool advanceClock = signal || nativeHoldIdleSamples < nativeFilterIdleLimit;
            nativeHoldIdleSamples = signal ? 0 : std::min(nativeFilterIdleLimit, nativeHoldIdleSamples + 1);
            if (advanceClock) {
                if (nativeHoldCounter >= period) {
                    for (int channel = 0; channel < blockChannels; ++channel)
                        nativeHeldSamples[static_cast<size_t>(channel)] = utils::sanitizeFinite(dryBuffer.getSample(channel, sampleIndex));
                    nativeHoldCounter = 0;
                }
                ++nativeHoldCounter;
            }
            nativeSamples = nativeHeldSamples;
        }
        else if (nativeMix) {
            for (int channel = 0; channel < blockChannels; ++channel)
            {
                const auto ch = static_cast<size_t>(channel);
                const auto input = utils::sanitizeFinite(dryBuffer.getSample(channel, sampleIndex));
                nativeSamples[ch] = placementEnabled && activeNativeFilterPlacement == 1
                    ? nativePlacementFilters[ch].processSample(input, nativeFilterMorph) : input;
            }
            fad::engine::AudioBlock<float> frame(nativePointers.data(), static_cast<size_t>(numChannels), 1);
            auto up = nativeOversampling->processSamplesUp(frame);
            for (int channel = 0; channel < blockChannels; ++channel) {
                auto* samples = up.getChannelPointer(static_cast<size_t>(channel));
                for (size_t i = 0; i < up.getNumSamples(); ++i)
                    samples[i] = activeSerumDiode2 ? nativeDiode2(samples[i], drive)
                                                     : activeSerumZeroSquare ? serumZeroSquare(samples[i], drive)
                                                     : activeSerumAsym ? nativeAsym(samples[i], drive) : activeSerumTube ? nativeTube(samples[i], drive) : nativeSoftClip(samples[i], drive);
            }
            nativeOversampling->processSamplesDown(frame);
            // Held-out native noise probes have the same decimator response
            // with four additional host samples of internal delay.
            for(int ch=0;ch<blockChannels;++ch) {
                const auto index=static_cast<size_t>(ch);
                std::swap(nativeSamples[index],nativeAlignment[index][nativeAlignmentIndex]);
            }
            nativeAlignmentIndex=(nativeAlignmentIndex+1)%4;
        }

        for (int channel = 0; channel < blockChannels; ++channel)
        {
            const auto inputSample = dryBuffer.getSample(channel, sampleIndex);
            auto* output = outputBlock.getChannelPointer(static_cast<size_t>(channel));
            if (! std::isfinite(inputSample))
            {
                preFilters[static_cast<size_t>(channel)].reset();
                postFilters[static_cast<size_t>(channel)].reset();
                dcBlockers[static_cast<size_t>(channel)].reset();
                output[sampleIndex] = 0.0f;
                continue;
            }
            const auto safeInput = inputSample;

            float processed = 0.0f;
            if (activeSerumDownsample)
            {
                processed = nativeSamples[static_cast<size_t>(channel)];
            }
            else if (activeSerumZeroSquare)
            {
                processed = nativeSamples[static_cast<size_t>(channel)];
                if (postHP) {
                    const auto ch=static_cast<size_t>(channel);
                    // The native Post path has a soft limiting input stage and
                    // a separately interpolated 2x filter, after distortion decimation.
                    processed = filters::serumFilterDrive(processed, 1.0f);
                    const auto interpolated = .5f * (nativePrevious[ch] + processed);
                    const auto first = nativeHP[ch].processSample(0, interpolated);
                    const auto second = nativeHP[ch].processSample(0, processed);
                    const auto lowFirst = nativeLP[ch].processSample(0, interpolated);
                    const auto lowSecond = nativeLP[ch].processSample(0, processed);
                    nativePrevious[ch] = processed;
                    const auto high = .5f * (first + second);
                    const auto low = .5f * (lowFirst + lowSecond);
                    processed = low + nativePostMorph * (high - low);
                }
            }
            else if (activeSerumSoftClip || activeSerumTube || activeSerumAsym || activeSerumDiode2)
            {
                processed = nativeSamples[static_cast<size_t>(channel)];
                if (placementEnabled && activeNativeFilterPlacement == 2)
                    processed = nativePlacementFilters[static_cast<size_t>(channel)].processSample(processed, nativeFilterMorph);
            }
            else
            {
                // Pre-filter (lowpass)
                processed = preFilters[static_cast<size_t>(channel)].processSample(0, safeInput);
                if (! std::isfinite(processed))
                {
                    preFilters[static_cast<size_t>(channel)].reset();
                    postFilters[static_cast<size_t>(channel)].reset();
                    dcBlockers[static_cast<size_t>(channel)].reset();
                    output[sampleIndex] = 0.0f;
                    continue;
                }

                // Apply drive gain and bias offset
                processed = processed * gain + biasOffset;

                // Nonlinear distortion
                processed = processNonlinearity(processed, activeDistortionMode);

                // Remove bias (use nonlinearity response to bias for correct DC removal)
                processed -= processNonlinearity(biasOffset, activeDistortionMode);

                // Post-filter (lowpass)
                processed = postFilters[static_cast<size_t>(channel)].processSample(0, processed);
                if (! std::isfinite(processed))
                {
                    postFilters[static_cast<size_t>(channel)].reset();
                    dcBlockers[static_cast<size_t>(channel)].reset();
                    output[sampleIndex] = 0.0f;
                    continue;
                }

                // DC blocker (removes residual DC from rectify/asymmetric modes)
                processed = dcBlockers[static_cast<size_t>(channel)].processSample(0, processed);
                if (! std::isfinite(processed))
                {
                    dcBlockers[static_cast<size_t>(channel)].reset();
                    output[sampleIndex] = 0.0f;
                    continue;
                }

            }

            // Dry/wet mix
            auto drySample = safeInput;
            if (nativeMix && !activeSerumDownsample)
                std::swap(drySample, nativeDryAlignment[static_cast<size_t>(channel)][nativeDryAlignmentIndex]);
            const auto mixed = drySample * dryMix + processed * wetGain;
            if (! std::isfinite(mixed))
            {
                preFilters[static_cast<size_t>(channel)].reset();
                postFilters[static_cast<size_t>(channel)].reset();
                dcBlockers[static_cast<size_t>(channel)].reset();
                output[sampleIndex] = 0.0f;
                continue;
            }

            auto transitioned = mixed;
            if (modeFadeRemaining > 0)
            {
                const auto fadeOut = static_cast<float>(modeFadeRemaining)
                    / static_cast<float>(modeFadeSamples);
                transitioned = modeFadeStartSamples[static_cast<size_t>(channel)] * fadeOut
                    + transitioned * (1.0f - fadeOut);
            }
            transitioned = fadmodel1::dsp::utils::sanitizeFinite(transitioned);
            output[sampleIndex] = transitioned;
            lastOutputSamples[static_cast<size_t>(channel)] = transitioned;
        }

        hasProcessedSample = true;
        if (nativeMix) nativeDryAlignmentIndex = (nativeDryAlignmentIndex + 1) % 7;
        if (modeFadeRemaining > 0)
            --modeFadeRemaining;
    }
}

void DistortionFxCore::reset()
{
    for (auto& filter : nativePlacementFilters) filter.reset();
    nativeFilterMorph = 0.0f;
    nativePlacementIdleSamples = 0;
    nativeActivityKnown = nativeVoiceActive = false;
    nativeFilterBandwidth.setCurrentAndTargetValue(nativeFilterBandwidth.getTargetValue());
    nativeFilterMorphTarget.setCurrentAndTargetValue(nativeFilterMorphTarget.getTargetValue());
    nativeModulation.fill(0); appliedNativeModulation.fill(0); nativeModulationRemaining = 0;
    nativePrevious.fill(0);
    nativeAlignment = {}; nativeAlignmentIndex = 0;
    nativeDryAlignment = {}; nativeDryAlignmentIndex = 0;
    for(auto& filter:nativeHP) filter.reset();
    for(auto& filter:nativeLP) filter.reset();
    nativePostMorph = 0.0f; nativeFilterIdleSamples = nativeFilterIdleLimit;
    nativePostCutoff.setCurrentAndTargetValue(nativePostCutoff.getTargetValue());
    amount.setCurrentAndTargetValue(amount.getTargetValue());
    nativeDrive.setCurrentAndTargetValue(nativeDrive.getTargetValue());
    if (nativeOversampling) nativeOversampling->reset();
    tone.setCurrentAndTargetValue(tone.getTargetValue());
    mix.setCurrentAndTargetValue(mix.getTargetValue());
    bias.setCurrentAndTargetValue(bias.getTargetValue());

    for (auto& filter : preFilters)
        filter.reset();

    for (auto& filter : postFilters)
        filter.reset();

    for (auto& filter : dcBlockers)
        filter.reset();

    activeDistortionMode = std::clamp(distortionMode.load(std::memory_order_relaxed), 0, 5);
    activeSerumSoftClip = serumSoftClip.load(std::memory_order_relaxed) && activeDistortionMode == 0;
    activeSerumTube = serumTube.load(std::memory_order_relaxed);
    activeSerumDiode2 = serumDiode2.load(std::memory_order_relaxed);
    activeNativeFilterPlacement = nativeFilterPlacement.load(std::memory_order_relaxed);
    activeSerumAsym = serumAsym.load(std::memory_order_relaxed);
    activeSerumDownsample = serumDownsample.load(std::memory_order_relaxed);
    nativeHoldCounter = nativeHoldIdleSamples = 0; nativeHeldSamples = {};
    activePostHP = nativePostHP.load(std::memory_order_relaxed);
    activeSerumZeroSquare = serumZeroSquareEnabled.load(std::memory_order_relaxed);
    modeFadeRemaining = 0;
    modeFadeStartSamples = {};
    lastOutputSamples = {};
    hasProcessedSample = false;
    updatePreFilters(tone.getCurrentValue());
    updatePostFilters(tone.getCurrentValue());
    toneFilterUpdateSamplesRemaining = 0;
}

void DistortionFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        for (auto& filter : nativePlacementFilters) filter.reset();
        nativeFilterMorph = 0.0f;
        nativePlacementIdleSamples = 0;
        nativeActivityKnown = nativeVoiceActive = false;
        nativeHoldCounter = nativeHoldIdleSamples = 0; nativeHeldSamples = {};
        nativePrevious.fill(0);
        nativeAlignment = {}; nativeAlignmentIndex = 0;
        nativeDryAlignment = {}; nativeDryAlignmentIndex = 0;
        for(auto& filter:nativeHP) filter.reset();
            for(auto& filter:nativeLP) filter.reset();
            nativePostMorph = 0.0f; nativeFilterIdleSamples = nativeFilterIdleLimit;
        if(nativeOversampling) nativeOversampling->reset();
        for (auto& filter : preFilters)
            filter.reset();
        for (auto& filter : postFilters)
            filter.reset();
        for (auto& filter : dcBlockers)
            filter.reset();
        toneFilterUpdateSamplesRemaining = 0;
        modeFadeRemaining = 0;
        hasProcessedSample = false;
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void DistortionFxCore::setGenericParam(int index, float value01)
{
    const auto safeValue01 = fadmodel1::dsp::utils::sanitizeUnitInterval(value01);

    switch (index)
    {
        case 0: // Amount
            nativeDrive.setTargetValue(safeValue01);
            amount.setTargetValue(
                fad::engine::math::gainFromDecibels(fad::engine::math::interpolate(safeValue01, 0.0f, 30.0f)));
            break;
        case 1: // Tone
            tone.setTargetValue(safeValue01);
            break;
        case 2: // Mix
            mix.setTargetValue(safeValue01);
            break;
        case 3: // Mode (0-5 discrete)
            distortionMode.store(std::clamp(fad::engine::math::roundToNearestInt(safeValue01 * 5.0f), 0, 5), std::memory_order_relaxed);
            break;
        case 4: // Pre Filter freq (200-8000 Hz, skew 0.3)
            preFilterFreq = fad::engine::math::RuntimeRange(200.0f, 8000.0f, 0.0f, 0.3f)
                                .denormalise(safeValue01);
            updatePreFilters(tone.getCurrentValue());
            break;
        case 5: // Post Filter freq (1000-20000 Hz, skew 0.3)
            postFilterFreq = fad::engine::math::RuntimeRange(1000.0f, 20000.0f, 0.0f, 0.3f)
                                 .denormalise(safeValue01);
            updatePostFilters(tone.getCurrentValue());
            break;
        case 6: // Bias (-1 to +1)
            bias.setTargetValue(fad::engine::math::interpolate(safeValue01, -1.0f, 1.0f));
            break;
        case 13: serumDownsample.store(safeValue01 >= .5f, std::memory_order_relaxed); break;
        case 14: nativeFilterPlacement.store(std::clamp(fad::engine::math::roundToNearestInt(safeValue01*2), 0, 2),std::memory_order_relaxed); break;
        case 15: nativeFilterMorphTarget.setTargetValue(safeValue01); break;
        case 16: nativeFilterBandwidth.setTargetValue(safeValue01); break;
        case 17: serumDiode2.store(safeValue01 >= .5f,std::memory_order_relaxed); break;
        case 12: serumAsym.store(safeValue01 >= .5f, std::memory_order_relaxed); break;
        case 11: serumTube.store(safeValue01 >= .5f, std::memory_order_relaxed); break;
        case 7: serumSoftClip.store(safeValue01 >= .5f, std::memory_order_relaxed); break;
        case 8: serumZeroSquareEnabled.store(safeValue01 >= .5f, std::memory_order_relaxed); break;
        case 9: nativePostHP.store(safeValue01 >= .5f, std::memory_order_relaxed); break;
        case 10: nativePostCutoff.setTargetValue(8.1757989156f*std::exp2(safeValue01*(128.0f/12.0f))); break;
        default: break;
    }
}

float DistortionFxCore::processNonlinearity(float input, int mode) noexcept
{
    switch (mode)
    {
        case 0: // SoftClip (tanh)
            return std::tanh(input);

        case 1: // HardClip
            return std::clamp(input, -1.0f, 1.0f);

        case 2: // Tube (asymmetric tanh)
        {
            const auto asymmetric = input + 0.2f * input * input;
            return 0.9f * std::tanh(asymmetric);
        }

        case 3: // Foldback
        {
            if (! std::isfinite(input))
                return 0.0f;

            auto x = input;
            int iterations = 0;
            while ((x > 1.0f || x < -1.0f) && iterations++ < 64)
            {
                if (x > 1.0f)
                    x = 2.0f - x;
                else if (x < -1.0f)
                    x = -2.0f - x;
            }
            if (x > 1.0f || x < -1.0f)
                return std::clamp(x, -1.0f, 1.0f);
            return x;
        }

        case 4: // Rectify (half-wave)
            return std::max(0.0f, input);

        case 5: // Asymmetric (positive more than negative)
        {
            if (input >= 0.0f)
                return std::tanh(input * 1.5f);
            return std::tanh(input * 0.5f);
        }

        default:
            return std::tanh(input);
    }
}

void DistortionFxCore::updatePreFilters(float toneValue) noexcept
{
    const auto nyquistSafe =
        static_cast<float>(std::max(200.0, sampleRate * 0.49));
    const auto toneScale =
        std::exp2(fad::engine::math::interpolate(std::clamp(toneValue, 0.0f, 1.0f), -1.0f, 1.0f));
    const auto freq = std::clamp(preFilterFreq * toneScale, 200.0f, nyquistSafe);

    for (auto& filter : preFilters)
    {
        filter.setType(fad::engine::filters::TptMode::lowpass);
        filter.setCutoffFrequency(freq);
        filter.setResonance(0.707f);
    }
}

void DistortionFxCore::updatePostFilters(float toneValue) noexcept
{
    const auto nyquistSafe =
        static_cast<float>(std::max(1000.0, sampleRate * 0.49));
    const auto toneScale =
        std::exp2(fad::engine::math::interpolate(std::clamp(toneValue, 0.0f, 1.0f), -1.0f, 1.0f));
    const auto freq = std::clamp(postFilterFreq * toneScale, 1000.0f, nyquistSafe);

    for (auto& filter : postFilters)
    {
        filter.setType(fad::engine::filters::TptMode::lowpass);
        filter.setCutoffFrequency(freq);
        filter.setResonance(0.707f);
    }
}
}
