#include <fad/engine/fx/StereoFx.h>
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
constexpr auto smoothingSeconds = 0.02;

std::array<float, 2> makeBalanceGains(float pan) noexcept
{
    const auto clampedPan = std::clamp(pan, -1.0f, 1.0f);
    const auto left = clampedPan > 0.0f ? 1.0f - clampedPan : 1.0f;
    const auto right = clampedPan < 0.0f ? 1.0f + clampedPan : 1.0f;
    return {left, right};
}

float makeStereoNormalisation(float midGain, float sideGain, float width) noexcept
{
    const auto sideTerm = sideGain * width;
    const auto bound = 0.5f * (std::abs(midGain + sideTerm) + std::abs(midGain - sideTerm));
    return 1.0f / std::max(1.0f, bound);
}
}

void StereoFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    hyperIdleLimit = static_cast<int>(std::llround(sampleRate * (1504.0 / 48000.0)));
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));
    dryBuffer.setSize(numChannels, static_cast<int>(spec.maximumBlockSize), false, false, true);
    dimensionHistory.setSize(1, static_cast<int>(std::ceil(sampleRate * .2)) + 8, false, false, true);
    // Native smoothers store the two coefficients separately as floats.
    // Their small sum error is audible with fractional-delay interpolation.
    const auto dimensionStep = static_cast<float>(1.0 / (sampleRate * .02));
    dimensionInputGain = dimensionStep;
    dimensionSmoothing = 1.0f - dimensionStep;
    dimensionMix.reset(sampleRate, smoothingSeconds);
    hyperHistory.setSize(2, static_cast<int>(std::ceil(sampleRate * .025)) + 8, false, false, true);
    for (auto* control : {&hyperWet, &hyperRate, &hyperDetune}) control->reset(sampleRate, smoothingSeconds);
    nativeBlend.reset(sampleRate, .005);
    for (auto& filter : hyperHighPass) {
        filter.prepare({sampleRate, spec.maximumBlockSize, 1});
        filter.setType(fad::engine::filters::TptMode::highpass);
        filter.setCutoffFrequency(242.43f);
        filter.setResonance(.728f);
    }
    width.reset(sampleRate, smoothingSeconds);
    pan.reset(sampleRate, smoothingSeconds);
    midGainDb.reset(sampleRate, smoothingSeconds);
    sideGainDb.reset(sampleRate, smoothingSeconds);
    mix.reset(sampleRate, smoothingSeconds);

    width.setCurrentAndTargetValue(1.0f);
    pan.setCurrentAndTargetValue(0.0f);
    midGainDb.setCurrentAndTargetValue(0.0f);
    sideGainDb.setCurrentAndTargetValue(0.0f);
    mix.setCurrentAndTargetValue(1.0f);
    parametersPrimed = false;
    reset();
}

void StereoFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
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

void StereoFxCore::processActiveBlock(fad::engine::ProcessContextReplacing<float>& context)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());

    assert(blockChannels <= dryBuffer.getNumChannels() && numSamples <= dryBuffer.getNumSamples());
    if (blockChannels <= 0
        || blockChannels > dryBuffer.getNumChannels()
        || numSamples <= 0
        || numSamples > dryBuffer.getNumSamples())
        return;

    parametersPrimed = true;


    for (int channel = 0; channel < blockChannels; ++channel)
        std::copy_n(outputBlock.getChannelPointer(static_cast<size_t>(channel)), numSamples, dryBuffer.getWritePointer(channel));

    if (nativeHyper && !nativeBlend.isSmoothing()) { processNativeHyper(outputBlock); return; }

    if (blockChannels == 1)
    {
        // Mono has no stereo image to rebalance, so keep pan/width transparent.
        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            (void) (pan.getNextValue(), width.getNextValue(), sideGainDb.getNextValue());
            const auto currentMix = std::clamp(utils::sanitizeFinite(mix.getNextValue(), 1.0f), 0.0f, 1.0f);
            const auto currentMidGainDb = utils::sanitizeFinite(midGainDb.getNextValue());
            const auto dry = 1.0f - currentMix;
            const auto midGain = utils::sanitizeFinite(fad::engine::math::gainFromDecibels(currentMidGainDb), 1.0f);

            const auto inputSample = utils::sanitizeFinite(dryBuffer.getSample(0, sampleIndex));
            const auto processed = utils::sanitizeFinite(inputSample * midGain);
            outputBlock.getChannelPointer(0)[sampleIndex] =
                utils::sanitizeFinite(inputSample * dry + processed * currentMix);
        }
        if (nativeBlend.isSmoothing()) processNativeHyper(outputBlock);
        return;
    }

    // Stereo path: full M/S processing
    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto currentWidth = utils::sanitizeFinite(width.getNextValue(), 1.0f);
        const auto currentPan = utils::sanitizeFinite(pan.getNextValue());
        const auto currentMidGainDb = utils::sanitizeFinite(midGainDb.getNextValue());
        const auto currentSideGainDb = utils::sanitizeFinite(sideGainDb.getNextValue());
        const auto currentMix = std::clamp(utils::sanitizeFinite(mix.getNextValue(), 1.0f), 0.0f, 1.0f);
        const auto dry = 1.0f - currentMix;
        const auto midGainLinear = utils::sanitizeFinite(fad::engine::math::gainFromDecibels(currentMidGainDb), 1.0f);
        const auto sideGainLinear = utils::sanitizeFinite(fad::engine::math::gainFromDecibels(currentSideGainDb), 1.0f);
        const auto stereoNormalisation = makeStereoNormalisation(midGainLinear, sideGainLinear, currentWidth);

        const auto inL = utils::sanitizeFinite(dryBuffer.getSample(0, sampleIndex));
        const auto inR = utils::sanitizeFinite(dryBuffer.getSample(1, sampleIndex));

        // 1. M/S encode
        auto mid = (inL + inR) * 0.5f;
        auto side = (inL - inR) * 0.5f;

        // 2. Apply gains
        mid *= midGainLinear;
        side *= sideGainLinear;

        // 3. Width
        side *= currentWidth;

        // 4. M/S decode
        auto outL = mid + side;
        auto outR = mid - side;
        outL *= stereoNormalisation;
        outR *= stereoNormalisation;

        // 5. Pan as stereo balance so center remains transparent.
        const auto [leftGain, rightGain] = makeBalanceGains(currentPan);
        outL *= leftGain;
        outR *= rightGain;

        // 6. Mix
        outputBlock.getChannelPointer(0)[sampleIndex] =
            utils::sanitizeFinite(inL * dry + outL * currentMix);
        outputBlock.getChannelPointer(1)[sampleIndex] =
            utils::sanitizeFinite(inR * dry + outR * currentMix);
    }
    if (nativeBlend.isSmoothing()) processNativeHyper(outputBlock);
}

void StereoFxCore::reset()
{
    nativeModulation.fill(0); appliedNativeModulation.fill(0); nativeModulationRemaining = 0;
    nativeModulationStarted = false;
    dimensionHistory.clear(); dimensionWrite = 0; dimensionPhase = 0.0;
    dimensionDelays.fill(512.0);
    dimensionMix.setCurrentAndTargetValue(dimensionMix.getTargetValue());
    hyperHistory.clear(); hyperWrite = 0;
    for (auto& filter : hyperHighPass) filter.reset();
    // Native rate sweeps reveal 31⅓ ms of silent phase advancement before
    // the initially silent heads suspend (48 kHz calibration).
    hyperWarmupRemaining = hyperIdleLimit;
    nativeBlend.setCurrentAndTargetValue(nativeBlend.getTargetValue());
    constexpr std::array<std::array<double, 7>, 2> phases{{
        {{0.828347198, 0.187632363, 0.278154296, 0.179781161, 0.813003082, 0.935003108, 0.527999055}},
        {{0.173965528, 0.015817880, 0.810437344, 0.802096779, 0.078007496, 0.574001256, 0.894999253}}
    }};
    hyperPhase = phases;
    for (size_t i = 0; i < savedHyperPhases.size(); ++i)
        if (savedHyperPhases[i] >= .5f) {
            const auto phase = 2.0 * savedHyperPhases[i] - .5;
            hyperPhase[i / 7][i % 7] = phase - std::floor(phase);
        }
    for (auto* control : {&hyperWet, &hyperRate, &hyperDetune})
        control->setCurrentAndTargetValue(control->getTargetValue());
    parametersPrimed = false;
    width.setCurrentAndTargetValue(width.getTargetValue());
    pan.setCurrentAndTargetValue(pan.getTargetValue());
    midGainDb.setCurrentAndTargetValue(midGainDb.getTargetValue());
    sideGainDb.setCurrentAndTargetValue(sideGainDb.getTargetValue());
    mix.setCurrentAndTargetValue(mix.getTargetValue());
}

void StereoFxCore::setBypassed(bool shouldBeBypassed)
{
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void StereoFxCore::setGenericParam(int index, float value01)
{
    const auto safeValue01 = utils::sanitizeUnitInterval(value01);

    const auto setSmoothedValue = [this](auto& smoother, float value)
    {
        if (! parametersPrimed)
            smoother.setCurrentAndTargetValue(value);
        else
            smoother.setTargetValue(value);
    };

    if (index >= 10 && index < 24) {
        savedHyperPhases[static_cast<size_t>(index - 10)] = safeValue01;
        if (!parametersPrimed && safeValue01 >= .5f) {
            const auto phase = 2.0 * safeValue01 - .5;
            hyperPhase[static_cast<size_t>((index - 10) / 7)][static_cast<size_t>((index - 10) % 7)] = phase - std::floor(phase);
        }
        return;
    }
    switch (index)
    {
        case 24: dimensionSize = safeValue01; break;
        case 25: setSmoothedValue(dimensionMix, safeValue01); break;
        case 0:
            setSmoothedValue(width, fad::engine::math::interpolate(safeValue01, 0.0f, 2.0f));
            break;
        case 1:
            setSmoothedValue(pan, fad::engine::math::interpolate(safeValue01, -1.0f, 1.0f));
            break;
        case 2:
            setSmoothedValue(midGainDb, fad::engine::math::interpolate(safeValue01, -12.0f, 12.0f));
            break;
        case 3:
            setSmoothedValue(sideGainDb, fad::engine::math::interpolate(safeValue01, -12.0f, 12.0f));
            break;
        case 4:
            setSmoothedValue(mix, safeValue01);
            break;
        case 5: nativeHyper = safeValue01 >= .5f; setSmoothedValue(nativeBlend, nativeHyper ? 1.0f : 0.0f); break;
        case 6: setSmoothedValue(hyperWet, safeValue01); break;
        case 7: setSmoothedValue(hyperRate, safeValue01); break;
        case 8: setSmoothedValue(hyperDetune, safeValue01); break;
        case 9: hyperVoices = std::clamp(fad::engine::math::roundToNearestInt(safeValue01 * 7), 0, 7); break;
        default: break;
    }
}

void StereoFxCore::processNativeHyper(fad::engine::AudioBlock<float>& block) noexcept
{
    // Native noise probes identify seven independent triangular read heads
    // per channel, squared Rate, linear Detune depth, and this wet law.
    // Linear interpolation and the wet high-pass are measured independently;
    // saved initial phases are restored separately from the running phases.
    constexpr std::array<std::array<double, 7>, 2> rateRatios{{
        {{0.980479167, 1.062161667, 0.889967500, 1.296842917, 0.707097917, 1.542199167, 0.594603333}},
        {{1.019907917, 0.937125000, 1.107444583, 0.773780833, 1.414196250, 0.648419583, 1.681797917}}
    }};
    const auto length = hyperHistory.getNumSamples();
    const auto channels = static_cast<int>(block.getNumChannels());
    if (length < 8) return;
    for (size_t sample = 0; sample < block.getNumSamples(); ++sample) {
        const auto blend = nativeBlend.getNextValue();
        if (nativeModulationRemaining <= 0) {
            appliedNativeModulation = nativeModulation; nativeModulationRemaining = 16;
        }
        --nativeModulationRemaining;
        const auto wet = std::clamp(hyperWet.getNextValue() + appliedNativeModulation[0], 0.0f, 1.0f);
        const auto baseRate = hyperRate.getNextValue();
        const auto depth = (.0002 + .020 * std::clamp(hyperDetune.getNextValue() + appliedNativeModulation[1], 0.0f, 1.0f)) * sampleRate;
        const auto b = .5f - .5f * std::cos(std::numbers::pi_v<float> * wet);
        for (int channel = 0; channel < channels; ++channel)
            if (std::abs(dryBuffer.getSample(channel, static_cast<int>(sample))) > 1.e-10f) {
                hyperWarmupRemaining = hyperIdleLimit;
                nativeModulationStarted = true;
            }
        // Native pre-roll advances the stored Rate/Size only. Modulated targets
        // start with the first signal; applying them during silence shifts every
        // read head before the first note (verified with phase-offset probes).
        const auto rate = std::clamp(baseRate + (nativeModulationStarted ? appliedNativeModulation[3] : 0.0f), 0.0f, 1.0f);
        std::array<float, 2> renderedChannels{};
        for (int channel = 0; channel < channels; ++channel) {
            const auto input = utils::sanitizeFinite(dryBuffer.getSample(channel, static_cast<int>(sample)));
            hyperHistory.setSample(channel, hyperWrite, input);
            float sum = 0;
            for (int voice = 0; voice < 7; ++voice) {
                auto& phase = hyperPhase[static_cast<size_t>(channel)][static_cast<size_t>(voice)];
                if (hyperWarmupRemaining > 0) {
                    phase += rate * rate * rateRatios[static_cast<size_t>(channel)][static_cast<size_t>(voice)] / sampleRate;
                    phase -= std::floor(phase);
                }
                if (voice >= hyperVoices) continue;
                auto position = hyperWrite - (1.0 + depth * (1.0 - std::abs(2.0 * phase - 1.0)));
                if (position < 0) position += length;
                const auto index = static_cast<int>(position);
                const auto fraction = static_cast<float>(position - index);
                const auto read = [&](int i) { return hyperHistory.getSample(channel, (i + length) % length); };
                const auto v = read(index);
                sum += v + fraction * (read(index + 1) - v);
            }
            // The native wet branch rejects low frequencies before mixing with dry.
            // Independent constant-delay noise identifies a two-pole high-pass.
            const auto filtered = hyperHighPass[static_cast<size_t>(channel)].processSample(0, sum);
            hyperHighPass[static_cast<size_t>(channel)].snapToZero();
            const auto rendered = hyperVoices > 0
                ? utils::sanitizeFinite(input * (1.0f - b) + b * (.444079f * input + filtered)) : input;
            renderedChannels[static_cast<size_t>(channel)] = rendered;
        }
        // Independent native noise probes identify four mono read heads with
        // geometric delay spacing. Equal/opposite additions preserve the mid.
        const auto dimLength = dimensionHistory.getNumSamples();
        const auto mono = channels == 2 ? .5f * (renderedChannels[0] + renderedChannels[1]) : renderedChannels[0];
        dimensionHistory.setSample(0, dimensionWrite, mono);
        const auto dimWet = std::clamp(dimensionMix.getNextValue() + appliedNativeModulation[2], 0.0f, 1.0f);
        std::array<float, 4> taps{};
        const auto size = std::clamp(dimensionSize + (nativeModulationStarted ? appliedNativeModulation[4] : 0.0f), .00001f, 1.0f);
        auto target = (.015 + .100 * size) * sampleRate;
        for (size_t head = 0; head < taps.size(); ++head) {
            if (hyperWarmupRemaining > 0)
                dimensionDelays[head] = dimensionInputGain * target + dimensionSmoothing * dimensionDelays[head];
            auto position = dimensionWrite - (dimensionDelays[head] + 1.0);
            if (position < 0) position += dimLength;
            const auto index = static_cast<int>(position);
            const auto fraction = static_cast<float>(position - index);
            const auto a = dimensionHistory.getSample(0, index);
            taps[head] = a + fraction * (dimensionHistory.getSample(0, (index + 1) % dimLength) - a);
            target *= 1.18;
        }
        if (hyperWarmupRemaining > 0) {
            dimensionPhase += (2 * std::numbers::pi_v<double>) / (2.2 * sampleRate);
            if (dimensionPhase >= (2 * std::numbers::pi_v<double>)) dimensionPhase -= (2 * std::numbers::pi_v<double>);
        }
        const auto side = .5f * dimWet * static_cast<float>(std::sin(dimensionPhase) * (taps[0] - taps[2])
                                                        - std::cos(dimensionPhase) * (taps[1] - taps[3]));
        if (channels == 2) { renderedChannels[0] += side; renderedChannels[1] -= side; }
        for (int channel = 0; channel < channels; ++channel) {
            auto* data = block.getChannelPointer(static_cast<size_t>(channel));
            const auto rendered = utils::sanitizeFinite(renderedChannels[static_cast<size_t>(channel)]);
            data[sample] = blend >= 1.0f ? rendered : data[sample] + blend * (rendered - data[sample]);
        }
        dimensionWrite = (dimensionWrite + 1) % dimLength;
        hyperWrite = (hyperWrite + 1) % length;
        if (hyperWarmupRemaining > 0) --hyperWarmupRemaining;
    }
}
}
