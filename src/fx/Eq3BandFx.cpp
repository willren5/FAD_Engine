#include <fad/engine/fx/Eq3BandFx.h>
#include <fad/engine/fx/FxCatalog.h>
#include <fad/engine/math/DspSafetyUtils.h>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/Numeric.h>
#include <fad/engine/rt/ScopedNoDenormals.h>
#include <algorithm>
#include <cassert>
#include <complex>

namespace fadmodel1::dsp::fx
{
namespace
{
constexpr auto eqSmoothingSeconds = 0.03;
constexpr auto filterUpdateInterval = 16;
// Independent Serum curve probes resolve native HP/LP modulation on the audio
// sample timeline. Static jumps remain exact because the coefficient target is
// unchanged between samples; moving LFO routes no longer quantize to 4 samples.
constexpr auto nativeFilterUpdateInterval = 1;
constexpr auto butterworthQ = 0.707f;
}

void Eq3BandFxCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    // Original-plugin cutoff/retrigger probes require a short coefficient ramp.
    nativeCoefficientStep = 1.0 - std::exp(-1.0 / 8.0);
    numChannels = static_cast<int>(std::clamp(spec.numChannels, 1u, 2u));

    fad::engine::ProcessSpec monoSpec { sampleRate, spec.maximumBlockSize, 1u };

    for (auto& filter : lowBand)
        filter.prepare(monoSpec);
    for (auto& filter : midBand)
        filter.prepare(monoSpec);
    for (auto& filter : highBand)
        filter.prepare(monoSpec);

    lowFreq.reset(sampleRate, eqSmoothingSeconds);
    lowGainDb.reset(sampleRate, eqSmoothingSeconds);
    midFreq.reset(sampleRate, eqSmoothingSeconds);
    midGainDb.reset(sampleRate, eqSmoothingSeconds);
    midQ.reset(sampleRate, eqSmoothingSeconds);
    highFreq.reset(sampleRate, eqSmoothingSeconds);
    highGainDb.reset(sampleRate, eqSmoothingSeconds);
    constexpr std::array<float, 6> nativeDefaults{656.25f, 656.25f, .5f, .5f, 0.0f, 0.0f};
    for (size_t i = 0; i < nativeControls.size(); ++i)
    {
        nativeControls[i].reset(sampleRate, eqSmoothingSeconds);
        nativeControls[i].setCurrentAndTargetValue(nativeDefaults[i]);
    }
    nativeMode = false;
    nativeTypes.fill(0);

    lowFreq.setCurrentAndTargetValue(120.0f);
    lowGainDb.setCurrentAndTargetValue(0.0f);
    midFreq.setCurrentAndTargetValue(1000.0f);
    midGainDb.setCurrentAndTargetValue(0.0f);
    midQ.setCurrentAndTargetValue(1.0f);
    highFreq.setCurrentAndTargetValue(8000.0f);
    highGainDb.setCurrentAndTargetValue(0.0f);
    parametersPrimed = false;
    reset();
}

void Eq3BandFxCore::process(fad::engine::ProcessContextReplacing<float>& context)
{
    if (bypassed.load(std::memory_order_relaxed))
        return;

    fad::engine::rt::ScopedNoDenormals noDenormals;

    auto& outputBlock = context.getOutputBlock();
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());
    if (blockChannels <= 0 || blockChannels > numChannels || numSamples <= 0)
        return;

    parametersPrimed = true;

    int sampleIndex = 0;
    const auto updateInterval = nativeMode ? nativeFilterUpdateInterval : filterUpdateInterval;
    while (sampleIndex < numSamples)
    {
        if (filterUpdateSamplesRemaining <= 0)
        {
            const bool nativeSmoothing = std::any_of(nativeControls.begin(), nativeControls.end(),
                [](const auto& value) { return value.isSmoothing(); });
            const bool anySmoothing = nativeSmoothing || lowFreq.isSmoothing() || lowGainDb.isSmoothing()
                                      || midFreq.isSmoothing() || midGainDb.isSmoothing()
                                      || midQ.isSmoothing() || highFreq.isSmoothing()
                                      || highGainDb.isSmoothing();

            if (anySmoothing)
            {
                advanceSmoothers(updateInterval);
                needsFilterUpdate.store(true, std::memory_order_relaxed);
            }

            updateFilters();
            filterUpdateSamplesRemaining = updateInterval;
        }

        const auto chunkSize = std::min(filterUpdateSamplesRemaining, numSamples - sampleIndex);
        const auto chunkEnd = sampleIndex + chunkSize;

        // Process the chunk sample-by-sample, per channel
        for (int s = sampleIndex; s < chunkEnd; ++s)
        {
            if (nativeMode)
                for (size_t band = 0; band < 2; ++band)
                    if (nativeTypes[band] != 2)
                        for (size_t coefficient = 0; coefficient < 5; ++coefficient)
                            nativeCoefficients[band][coefficient] += nativeCoefficientStep
                                * (nativeCoefficientTargets[band][coefficient] - nativeCoefficients[band][coefficient]);
            for (int channel = 0; channel < blockChannels; ++channel)
            {
                const auto ch = static_cast<size_t>(channel);
                auto sample = utils::sanitizeFinite(outputBlock.getChannelPointer(ch)[s]);

                if (nativeMode)
                {
                    // Direct form I retains physical sample history when coefficients change.
                    double value = sample;
                    for (size_t band = 0; band < 2; ++band)
                    {
                        if (nativeTypes[band] == 2)
                        {
                            const auto& c = nativeCutCoefficients[band];
                            auto& state = nativeCutStates[band][ch];
                            const auto v3 = value - state[1];
                            const auto v1 = c[0] * state[0] + c[1] * v3;
                            const auto v2 = state[1] + c[1] * state[0] + c[2] * v3;
                            state[0] = 2.0 * v1 - state[0];
                            state[1] = 2.0 * v2 - state[1];
                            fad::engine::math::snapSmallState(state[0]);
                            fad::engine::math::snapSmallState(state[1]);
                            value = band == 0 ? value - c[3] * v1 - v2 : v2;
                            if (!std::isfinite(state[0]) || !std::isfinite(state[1]))
                            {
                                state = {};
                                value = 0.0;
                            }
                            continue;
                        }
                        const auto& c = nativeCoefficients[band];
                        auto& h = nativeHistory[band][ch];
                        const auto output = c[0]*value + c[1]*h[0] + c[2]*h[1] - c[3]*h[2] - c[4]*h[3];
                        h[1] = h[0]; h[0] = value; h[3] = h[2]; h[2] = output;
                        value = output;
                    }
                    sample = static_cast<float>(value);
                }
                else
                {
                    sample = lowBand[ch].processSample(sample);
                    lowBand[ch].snapToZero();
                    sample = midBand[ch].processSample(sample);
                    midBand[ch].snapToZero();
                    sample = highBand[ch].processSample(sample);
                    highBand[ch].snapToZero();
                }
                sample = nodeFilters[ch].process(sample);
                if (! std::isfinite(sample))
                {
                    lowBand[ch].reset();
                    midBand[ch].reset();
                    highBand[ch].reset();
                    for (auto& band : nativeHistory) band[ch] = {};
                    for (auto& band : nativeCutStates) band[ch] = {};
                    sample = 0.0f;
                }

                outputBlock.getChannelPointer(ch)[s] = sample;
            }
        }

        filterUpdateSamplesRemaining -= chunkSize;
        nodeUpdateSamplesRemaining = std::max(0, nodeUpdateSamplesRemaining-chunkSize);
        sampleIndex = chunkEnd;
    }
}

void Eq3BandFxCore::reset()
{
    for(auto& filter:nodeFilters)filter.reset();
    nodeUpdateSamplesRemaining=0;
    nodeLayerEnabled=false;
    nativeHistory = {};
    nativeCutStates = {};
    nativeCoefficientsPrimed = false;
    nativeModulation.fill(0);
    parametersPrimed = false;
    lowFreq.setCurrentAndTargetValue(lowFreq.getTargetValue());
    lowGainDb.setCurrentAndTargetValue(lowGainDb.getTargetValue());
    midFreq.setCurrentAndTargetValue(midFreq.getTargetValue());
    midGainDb.setCurrentAndTargetValue(midGainDb.getTargetValue());
    midQ.setCurrentAndTargetValue(midQ.getTargetValue());
    highFreq.setCurrentAndTargetValue(highFreq.getTargetValue());
    highGainDb.setCurrentAndTargetValue(highGainDb.getTargetValue());
    for (auto& value : nativeControls)
        value.setCurrentAndTargetValue(value.getTargetValue());

    for (auto& filter : lowBand)
        filter.reset();
    for (auto& filter : midBand)
        filter.reset();
    for (auto& filter : highBand)
        filter.reset();

    needsFilterUpdate.store(true, std::memory_order_relaxed);
    updateFilters();
    filterUpdateSamplesRemaining = 0;
}

void Eq3BandFxCore::setNodeCurve(const parameters::CurveNodes& curve) noexcept
{
    if(!curve.enabled && curve.historySlot==0)
    {
        if(nodeLayerEnabled)for(auto& filter:nodeFilters)filter.prepare(parameters::CurveNodes{},sampleRate);
        nodeLayerEnabled=false;nodeUpdateSamplesRemaining=0;return;
    }
    if(nodeUpdateSamplesRemaining>0)return;
    auto resolved=curve;
    resolved.history=nullptr;
    for(auto& filter:nodeFilters)filter.prepare(resolved,sampleRate);
    nodeLayerEnabled=resolved.enabled;
    nodeUpdateSamplesRemaining=64;
}

void Eq3BandFxCore::clearNativeModulation() noexcept
{
    for(auto& offset:nativeModulation) {
        if(offset!=0)needsFilterUpdate.store(true,std::memory_order_relaxed);
        offset=0;
    }
}

void Eq3BandFxCore::addNativeModulation(int target,float offset) noexcept
{
    if(target<88 || target>93 || !std::isfinite(offset))return;
    nativeModulation[static_cast<size_t>(target-88)]+=offset;
    needsFilterUpdate.store(true,std::memory_order_relaxed);
}

void Eq3BandFxCore::setBypassed(bool shouldBeBypassed)
{
    if (shouldBeBypassed && !bypassed.load(std::memory_order_relaxed))
    {
        for(auto& filter:nodeFilters)filter.reset();
        nodeLayerEnabled=false;nodeUpdateSamplesRemaining=0;
        nativeHistory = {};
        nativeCutStates = {};
        for (auto& filter : lowBand)
            filter.reset();
        for (auto& filter : midBand)
            filter.reset();
        for (auto& filter : highBand)
            filter.reset();
        filterUpdateSamplesRemaining = 0;
    }
    else if (!shouldBeBypassed && bypassed.load(std::memory_order_relaxed))
    {
        // Re-entering active state: force coefficient update
        // to prevent stale coefficients from previous session
        needsFilterUpdate.store(true, std::memory_order_relaxed);
        filterUpdateSamplesRemaining = 0;
    }
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

void Eq3BandFxCore::setGenericParam(int index, float value01)
{
    const auto safeValue01 = utils::sanitizeUnitInterval(value01);

    const auto setSmoothedValue = [this](auto& smoother, float value)
    {
        if (! parametersPrimed)
            smoother.setCurrentAndTargetValue(value);
        else
            smoother.setTargetValue(value);
    };

    if (index >= 7 && index <= 15)
    {
        if (index == 7)
            nativeMode = safeValue01 >= .5f;
        else if (index <= 13)
            setSmoothedValue(nativeControls[static_cast<size_t>(index - 8)],
                FxCatalog::denormalizeParamValue(FxType::eq3Band, index, safeValue01));
        else
        {
            const auto band = static_cast<size_t>(index - 14);
            const auto nextType = fad::engine::math::roundToNearestInt(safeValue01 * 2.0f);
            if (nextType != nativeTypes[band])
            {
                nativeHistory[band] = {};
                nativeCutStates[band] = {};
            }
            nativeTypes[band] = nextType;
        }
        needsFilterUpdate.store(true, std::memory_order_relaxed);
        return;
    }

    switch (index)
    {
        case 0:
            setSmoothedValue(lowFreq,
                fad::engine::math::RuntimeRange(20.0f, 500.0f, 0.0f, 0.3f).denormalise(safeValue01));
            needsFilterUpdate.store(true, std::memory_order_relaxed);
            break;
        case 1:
            setSmoothedValue(lowGainDb, fad::engine::math::interpolate(safeValue01, -12.0f, 12.0f));
            needsFilterUpdate.store(true, std::memory_order_relaxed);
            break;
        case 2:
            setSmoothedValue(midFreq,
                fad::engine::math::RuntimeRange(200.0f, 8000.0f, 0.0f, 0.3f).denormalise(safeValue01));
            needsFilterUpdate.store(true, std::memory_order_relaxed);
            break;
        case 3:
            setSmoothedValue(midGainDb, fad::engine::math::interpolate(safeValue01, -12.0f, 12.0f));
            needsFilterUpdate.store(true, std::memory_order_relaxed);
            break;
        case 4:
            setSmoothedValue(midQ,
                fad::engine::math::RuntimeRange(0.1f, 10.0f, 0.0f, 0.5f).denormalise(safeValue01));
            needsFilterUpdate.store(true, std::memory_order_relaxed);
            break;
        case 5:
            setSmoothedValue(highFreq,
                fad::engine::math::RuntimeRange(2000.0f, 20000.0f, 0.0f, 0.3f).denormalise(safeValue01));
            needsFilterUpdate.store(true, std::memory_order_relaxed);
            break;
        case 6:
            setSmoothedValue(highGainDb, fad::engine::math::interpolate(safeValue01, -12.0f, 12.0f));
            needsFilterUpdate.store(true, std::memory_order_relaxed);
            break;
        default: break;
    }
}

void Eq3BandFxCore::updateFilters() noexcept
{
    if (!needsFilterUpdate.load(std::memory_order_relaxed))
        return;

    if (sampleRate <= 0.0)
        return;

    // Clamp frequencies to safe Nyquist range
    const auto safeNyquist = static_cast<float>(sampleRate * 0.49);
    if (safeNyquist <= 20.0f)
        return;

    needsFilterUpdate.store(false, std::memory_order_relaxed);

    if (nativeMode)
    {
        for (size_t band = 0; band < 2; ++band)
        {
            auto frequency = utils::sanitizeFiniteClamped(
                nativeControls[band].getCurrentValue(), 656.25f, 1.0f, safeNyquist);
            auto q = utils::sanitizeUnitInterval(nativeControls[band + 2].getCurrentValue());
            auto gainDb = utils::sanitizeFiniteClamped(
                nativeControls[band + 4].getCurrentValue(), 0.0f, -24.0f, 24.0f);
            constexpr auto minimum = 44100.0f/2048.0f;
            if(nativeModulation[band]!=0) {
                const auto position=std::log(std::max(minimum, frequency)/minimum)/std::log(20000.0f/minimum);
                frequency=minimum*std::pow(20000.0f/minimum,
                    utils::sanitizeUnitInterval(position+nativeModulation[band]));
                frequency=std::min(frequency, safeNyquist);
            }
            q=utils::sanitizeUnitInterval(q+nativeModulation[band+2]);
            gainDb=48.0f*(utils::sanitizeUnitInterval((gainDb+24.0f)/48.0f+nativeModulation[band+4])-.5f);
            const auto gain = fad::engine::math::gainFromDecibels(gainDb);
            const auto peakQ = .1f + 18.9f * q * q * q * q;
            // Independent native impulse/white-noise coefficient fits establish
            // different Q laws for shelves and peak/cut filters.
            const auto shelfQ = .4f + (std::numbers::pi_v<float> - 1.4f) * q * q * q;
            if (nativeTypes[band] == 2)
            {
                const auto g = std::tan(std::numbers::pi_v<double>
                    * static_cast<double>(frequency) / sampleRate);
                const auto damping = 1.0 / static_cast<double>(peakQ);
                const auto a1 = 1.0 / (1.0 + g * (g + damping));
                nativeCutCoefficients[band] = {a1, g * a1, g * g * a1, damping};
            }

            const auto coefficients = nativeTypes[band] == 2
                ? (band == 0 ? fad::engine::filters::eqPassCoefficients<float>(true, sampleRate, frequency, peakQ)
                             : fad::engine::filters::eqPassCoefficients<float>(false, sampleRate, frequency, peakQ))
                : nativeTypes[band] == 1
                    ? fad::engine::filters::eqCoefficients<float>(fad::engine::filters::EqShape::peak, sampleRate, frequency, peakQ, gain)
                    : (band == 0 ? fad::engine::filters::eqCoefficients<float>(fad::engine::filters::EqShape::lowShelf, sampleRate, frequency, shelfQ, gain)
                                 : fad::engine::filters::eqCoefficients<float>(fad::engine::filters::EqShape::highShelf, sampleRate, frequency, shelfQ, gain));
            nativeCoefficientTargets[band] = {coefficients[0]/coefficients[3], coefficients[1]/coefficients[3], coefficients[2]/coefficients[3], coefficients[4]/coefficients[3], coefficients[5]/coefficients[3]};
            if (!nativeCoefficientsPrimed) nativeCoefficients[band] = nativeCoefficientTargets[band];
            for (auto& filter : band == 0 ? lowBand : highBand)
                filter.setRawCoefficients(coefficients);
        }
        nativeCoefficientsPrimed = true;
        return;
    }

    const auto currentLowFreq = utils::sanitizeFiniteClamped(lowFreq.getCurrentValue(), 120.0f, 20.0f, safeNyquist);
    const auto currentLowGain = fad::engine::math::gainFromDecibels(
        utils::sanitizeFiniteClamped(lowGainDb.getCurrentValue(), 0.0f, -24.0f, 24.0f));
    const auto currentMidFreq = utils::sanitizeFiniteClamped(midFreq.getCurrentValue(), 1000.0f, 20.0f, safeNyquist);
    const auto currentMidGain = fad::engine::math::gainFromDecibels(
        utils::sanitizeFiniteClamped(midGainDb.getCurrentValue(), 0.0f, -24.0f, 24.0f));
    const auto currentMidQ = utils::sanitizeFiniteClamped(midQ.getCurrentValue(), 1.0f, 0.1f, 10.0f);
    const auto currentHighFreq = utils::sanitizeFiniteClamped(highFreq.getCurrentValue(), 8000.0f, 20.0f, safeNyquist);
    const auto currentHighGain = fad::engine::math::gainFromDecibels(
        utils::sanitizeFiniteClamped(highGainDb.getCurrentValue(), 0.0f, -24.0f, 24.0f));

    const auto lowCoeffs = fad::engine::filters::eqCoefficients<float>(fad::engine::filters::EqShape::lowShelf,
        sampleRate,
        std::clamp(currentLowFreq, 20.0f, safeNyquist),
        butterworthQ,
        currentLowGain);

    const auto midCoeffs = fad::engine::filters::eqCoefficients<float>(fad::engine::filters::EqShape::peak,
        sampleRate,
        std::clamp(currentMidFreq, 20.0f, safeNyquist),
        std::clamp(currentMidQ, 0.1f, 10.0f),
        currentMidGain);

    const auto highCoeffs = fad::engine::filters::eqCoefficients<float>(fad::engine::filters::EqShape::highShelf,
        sampleRate,
        std::clamp(currentHighFreq, 20.0f, safeNyquist),
        butterworthQ,
        currentHighGain);

    for (auto& filter : lowBand)
        filter.setRawCoefficients(lowCoeffs);
    for (auto& filter : midBand)
        filter.setRawCoefficients(midCoeffs);
    for (auto& filter : highBand)
        filter.setRawCoefficients(highCoeffs);
}

double Eq3BandFxCore::getStaticMagnitudeForFrequency(double frequencyHz) const noexcept
{
    if (sampleRate <= 0.0 || !std::isfinite(frequencyHz)
        || bypassed.load(std::memory_order_relaxed)) return 1.0;
    const auto hz = std::clamp(frequencyHz, 0.0001, sampleRate * 0.4999);
    if (!nativeMode)
        return lowBand[0].getMagnitudeForFrequency(hz, sampleRate)
            * midBand[0].getMagnitudeForFrequency(hz, sampleRate)
            * highBand[0].getMagnitudeForFrequency(hz, sampleRate)
            * nodeFilters[0].getStaticMagnitudeForFrequency(hz);

    const auto z = std::polar(1.0, (2 * std::numbers::pi_v<double>) * hz / sampleRate);
    const auto inverseZ = std::conj(z);
    std::complex<double> response{1.0, 0.0};
    for (size_t band = 0; band < 2; ++band)
    {
        if (nativeTypes[band] == 2)
        {
            // Exact state-space transfer of the existing TPT integrators.
            const auto& c = nativeCutCoefficients[band];
            const auto a = z - (2.0 * c[0] - 1.0);
            const auto b = 2.0 * c[1];
            const auto cc = -2.0 * c[1];
            const auto d = z - (1.0 - 2.0 * c[2]);
            const auto determinant = a * d - b * cc;
            const auto state0 = (d * (2.0 * c[1]) - b * (2.0 * c[2])) / determinant;
            const auto state1 = (a * (2.0 * c[2]) - cc * (2.0 * c[1])) / determinant;
            const auto v1 = c[0] * state0 - c[1] * state1 + c[1];
            const auto v2 = c[1] * state0 + (1.0 - c[2]) * state1 + c[2];
            response *= band == 0 ? 1.0 - c[3] * v1 - v2 : v2;
        }
        else
        {
            const auto& c = nativeCoefficients[band];
            response *= (c[0] + c[1] * inverseZ + c[2] * inverseZ * inverseZ)
                / (1.0 + c[3] * inverseZ + c[4] * inverseZ * inverseZ);
        }
    }
    const auto magnitude = std::abs(response)*nodeFilters[0].getStaticMagnitudeForFrequency(hz);
    return std::isfinite(magnitude) ? magnitude : 0.0;
}

void Eq3BandFxCore::advanceSmoothers(int numSamples) noexcept
{
    for (auto& value : nativeControls)
        value.skip(numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        lowFreq.getNextValue();
        lowGainDb.getNextValue();
        midFreq.getNextValue();
        midGainDb.getNextValue();
        midQ.getNextValue();
        highFreq.getNextValue();
        highGainDb.getNextValue();
    }
}
}
