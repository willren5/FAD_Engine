#include <fad/engine/filters/FilterBlock.h>
#include <fad/engine/filters/SerumFilterDrive.h>
#include <fad/engine/math/DspSafetyUtils.h>

namespace fadmodel1::dsp::filters
{
namespace
{
int filterOrderForNativeModel(int nativeModel) noexcept
{
    if (nativeModel < 0)
        return 2;

    if (nativeModel == 59) return 6;
    if (nativeModel == 61) return 8;

    if (nativeModel == 14 || nativeModel == 16) return 2;
    if (nativeModel == 24 || nativeModel == 25 || nativeModel == 27 || nativeModel == 32) return 4;
    if (nativeModel == 12)
        return 2;
    if (nativeModel == 13)
        return 4;

    const auto familyIndex = nativeModel % 4;
    if (nativeModel <= 11 && familyIndex >= 0)
        return familyIndex + 1;

    // Special Serum models keep the legacy second-order path until their
    // topology is measured and implemented in the special-filter stage.
    return 2;
}
}

void FilterBlock::prepare(double sampleRate)
{
    currentSampleRate = fadmodel1::dsp::utils::sanitizeSampleRate(sampleRate);
    nativePhase.prepare(currentSampleRate * 2.0);
    nativeScream.prepare(currentSampleRate);
    nativeComb.prepare(currentSampleRate);
    modeFadeSamples = std::clamp(fad::engine::math::roundToNearestInt(static_cast<float>(currentSampleRate) * 0.003f), 16, 256);
    reset();
}

void FilterBlock::reset()
{
    nodeFilter.reset();
    s1 = 0.0f;
    s2 = 0.0f;
    onePoleStates.fill(0.0f);
    nativeBasicPreviousInput = 0.0f;
    stageS1.fill(0.0f);
    stageS2.fill(0.0f);
    nativePhase.reset();
    nativeScream.reset();
    nativeComb.reset();
    nativePhasePreviousInput = 0.0f;
    targetCutoffHz = 18000.0f;
    targetResonance = 0.1f;
    targetDriveDb = 0.0f;
    targetVariation = currentVariation = 0.5f;
    currentCutoffHz = targetCutoffHz;
    currentResonance = targetResonance;
    currentDriveDb = targetDriveDb;
    driveAmount = 0.0f;
    nativePassbandGain = 1.0f;
    nativeFeedback = 0.0f;
    filterMode = parameters::FilterMode::lowPass;
    nativeFilterModel = -1;
    filterOrder = 2;
    modeFadeSourceMode = filterMode;
    coefficientTargetsValid = false;
    cachedSmoothingSampleCount = 0;
    cachedSmoothingAlpha = 1.0f;
    modeFadeRemaining = 0;
    modeFadeSourceCorrection = 0.0f;
    modeFadeNeedsCorrection = false;
    lastOutputSample = 0.0f;
    hasProcessedSample = false;

    // Initialize coefficients for default values
    g = std::tan(std::numbers::pi_v<float> * 18000.0f
                 / static_cast<float>(currentSampleRate));
    k = 2.0f;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
    onePoleG = g / (1.0f + g);
    variationA1 = variationA2 = variationA3 = 0.0f;
    variationK = 2.0f;
}

void FilterBlock::updateCoefficients(const parameters::FilterSnapshot& snapshot,
                                     int smoothingSamples) noexcept
{
    nodeFilter.prepare(snapshot.curve,currentSampleRate);
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    ++coefficientUpdateCount;
#endif
    const auto nextMode = snapshot.mode;
    const auto minimumCutoff = snapshot.nativeModel >= 0 ? 1.0f : 20.0f;
    const auto maximumCutoff = snapshot.nativeModel >= 0 ? static_cast<float>(currentSampleRate) * .49f : 20000.0f;
    const auto nextCutoffHz = std::clamp(utils::isFinite(snapshot.cutoffHz) ? snapshot.cutoffHz : 18000.0f, minimumCutoff, maximumCutoff);
    const auto nextResonance = std::clamp(utils::isFinite(snapshot.resonance) ? snapshot.resonance : 0.1f, 0.1f, 0.97f);
    const auto nextDriveDb = std::clamp(utils::isFinite(snapshot.driveDb) ? snapshot.driveDb : 0.0f, 0.0f, 36.0f);
    const auto firstUpdate = ! coefficientTargetsValid;
    const auto nextNativeFilterModel = fadmodel1::dsp::utils::roundToIntSafe(
        static_cast<float>(snapshot.nativeModel), -1, -1, 95);
    const auto nextFilterOrder = filterOrderForNativeModel(nextNativeFilterModel);
    const auto modeChanged = ! firstUpdate && nextMode != filterMode;
    const auto modelChanged = ! firstUpdate
        && (nextNativeFilterModel != nativeFilterModel || nextFilterOrder != filterOrder);

    targetCutoffHz = nextCutoffHz;
    targetResonance = nextResonance;
    targetDriveDb = nextDriveDb;
    targetVariation = fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.nativeVariation, 0.5f, 0.0f, 1.0f);

    if (firstUpdate)
    {
        currentCutoffHz = targetCutoffHz;
        currentResonance = targetResonance;
        currentDriveDb = targetDriveDb;
        currentVariation = targetVariation;
        filterMode = nextMode;
        nativeFilterModel = nextNativeFilterModel;
        filterOrder = nextFilterOrder;
        modeFadeSourceMode = nextMode;
    }
    else
    {
        const auto previousCutoffHz = currentCutoffHz;
        const auto previousResonance = currentResonance;
        const auto previousDriveDb = currentDriveDb;
        const auto previousVariation = currentVariation;
        if (modeChanged || modelChanged)
        {
            if (modelChanged)
            {
                nativePhase.reset();
                nativeScream.reset();
                nativeComb.reset();
                nativePhasePreviousInput = 0.0f;
                nativeBasicPreviousInput = 0.0f;
            }
            modeFadeSourceMode = filterMode;
            filterMode = nextMode;
            if (hasProcessedSample)
            {
                modeFadeRemaining = modeFadeSamples;
                modeFadeSourceCorrection = 0.0f;
                modeFadeNeedsCorrection = true;
            }
        }
        nativeFilterModel = nextNativeFilterModel;
        filterOrder = nextFilterOrder;

        const auto safeSmoothingSamples = std::max(1, smoothingSamples);
        if (cachedSmoothingSampleCount != safeSmoothingSamples)
        {
            cachedSmoothingSampleCount = safeSmoothingSamples;
            cachedSmoothingAlpha = 1.0f - std::exp(
                -static_cast<float>(safeSmoothingSamples)
                / (0.005f * static_cast<float>(currentSampleRate)));
        }

        const auto smooth = [this] (float& current, float target, float snapThreshold) noexcept
        {
            current = fadmodel1::dsp::utils::sanitizeFinite(current, target);
            const auto difference = target - current;
            current = std::abs(difference) <= snapThreshold
                ? target
                : current + cachedSmoothingAlpha * difference;
        };
        smooth(currentCutoffHz, targetCutoffHz, 0.001f);
        smooth(currentResonance, targetResonance, 1.0e-6f);
        smooth(currentDriveDb, targetDriveDb, 1.0e-6f);
        smooth(currentVariation, targetVariation, 1.0e-6f);

        const auto changed = [] (float current, float previous) noexcept
        {
            return current < previous || current > previous;
        };
        const auto parametersChanged = changed(currentCutoffHz, previousCutoffHz)
            || changed(currentResonance, previousResonance)
            || changed(currentDriveDb, previousDriveDb)
            || changed(currentVariation, previousVariation);
        if (! modeChanged && ! modelChanged && ! parametersChanged)
            return;
    }

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    ++coefficientRecalculationCount;
#endif

    // Coefficient calculation (contains trig — called per block, not per sample)
    const auto fs = static_cast<float>(currentSampleRate);
    const auto clampedCutoff = std::clamp(currentCutoffHz, nativeFilterModel >= 0 ? 1.0f : 20.0f, fs * 0.49f);
    g = std::tan(std::numbers::pi_v<float> * clampedCutoff / fs);

    // k controls damping: k=2 is no resonance, k→0 is self-oscillation.
    // The exposed resonance range is aligned to the stable, non-dead region.
    k = std::max(0.05f, 2.0f * (1.0f - currentResonance));
    nativePassbandGain = 1.0f;
    const auto r = std::clamp((currentResonance - 0.1f) / 0.87f, 0.0f, 1.0f);
    nativeFeedback = std::min(3.9996f, 4.0f * r);
    if (nativeFilterModel == 59 || nativeFilterModel == 61)
        nativePhase.setParameters(clampedCutoff, r, nativeFilterModel);
    if (nativeFilterModel == 41 || nativeFilterModel == 51 || nativeFilterModel == 52)
        nativeComb.setParameters(clampedCutoff, r, currentDriveDb, currentVariation,
            nativeFilterModel == 52, nativeFilterModel == 41);
    if (nativeFilterModel == 94 || nativeFilterModel == 95)
        nativeScream.setParameters(clampedCutoff, r,
            std::pow(10.0, static_cast<double>(currentDriveDb) / 80.0) - 1.0,
            currentVariation, nativeFilterModel == 95);
    if (nativeFilterModel >= 0 && nativeFilterModel <= 3)
        nativePassbandGain = 1.0f + 4.0f * r * currentVariation;
    if ((nativeFilterModel >= 5 && nativeFilterModel <= 11 && nativeFilterModel != 8) || nativeFilterModel == 14
        || nativeFilterModel == 16 || nativeFilterModel == 24 || nativeFilterModel == 25 || nativeFilterModel == 27
        || nativeFilterModel == 31 || nativeFilterModel == 32)
    {
        // Native Low 12 / High 12 sweeps: resonance is stored through the
        // existing host range, then shaped by a fourth root. Resonance also
        // reduces passband gain. MG and composite filters have different laws.
        k = std::max(0.0002f, 2.0f * (1.0f - std::sqrt(std::sqrt(r))));
        if ((nativeFilterModel >= 5 && nativeFilterModel <= 11 && nativeFilterModel != 8) || nativeFilterModel == 14)
            nativePassbandGain = (1.0f + 4.0f * r * currentVariation) / (1.0f + 4.0f * r);
    }


    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
    onePoleG = g / (1.0f + g);
    if (nativeFilterModel == 4 || nativeFilterModel == 8)
    {
        // Serum's 6 dB pair is an explicit one-pole running at 2x. FX Filter
        // callers already provide the two interpolated sub-samples and prepare
        // this block at 2x; the main voice provides host-rate samples, so its
        // coefficient uses the corresponding internal 2x rate here.
        const auto internalRate = nativeInputPreShaped ? static_cast<double>(currentSampleRate)
                                                       : static_cast<double>(currentSampleRate) * 2.0;
        nativeOnePoleF = static_cast<float>(2.0 * std::sin(
            std::numbers::pi_v<double> * static_cast<double>(clampedCutoff) / internalRate));
        nativeOnePoleF = std::clamp(nativeOnePoleF, 0.0f, 1.999f);
    }
    cascadeK = 1.0f + .25f * k * k;
    cascadeA1 = 1.0f / (1.0f + g * (g + cascadeK));
    cascadeA2 = g * cascadeA1;
    cascadeA3 = g * cascadeA2;
    if (nativeFilterModel == 24 || nativeFilterModel == 25 || nativeFilterModel == 27)
    {
        // Variation controls a separate MIDI-frequency curve, independent of
        // the main cutoff and its stereo shift. Compound notches stay broad.
        const auto frequency = 8.1757989156f * std::exp2(currentVariation * (128.0f / 12.0f));
        const auto vg = std::tan(std::numbers::pi_v<float> * std::min(frequency, fs * 0.49f) / fs);
        variationK = nativeFilterModel == 24 ? k : 2.0f;
        variationA1 = 1.0f / (1.0f + vg * (vg + variationK));
        variationA2 = vg * variationA1;
        variationA3 = vg * variationA2;
    }

    // Drive sets the input gain before saturation; it must not attenuate the states.
    driveAmount = nativeFilterModel >= 0
        ? fad::engine::math::gainFromDecibels(currentDriveDb)
        : fad::engine::math::gainFromDecibels(currentDriveDb, 0.0f);
    coefficientTargetsValid = true;
}

float FilterBlock::processSample(float inputSample) noexcept
{
    if (! utils::isFinite(inputSample))
        inputSample = 0.0f;

    // Preserve the pre-Serum legacy FAD path byte-for-byte in its structural
    // mode transition semantics. Imported native models use the cascaded path
    // below; legacy automation keeps the old source-mode fade contract.
    if (nativeFilterModel < 0 && filterOrder == 2)
    {
        const float drivenInput = driveAmount > 1.001f ? std::tanh(inputSample * driveAmount) : inputSample;
        const float v3 = drivenInput - s2;
        const float v1 = a1 * s1 + a2 * v3;
        const float v2 = s2 + a2 * s1 + a3 * v3;
        const float newS1 = 2.0f * v1 - s1;
        const float newS2 = 2.0f * v2 - s2;

        s1 = newS1;
        s2 = newS2;

        fad::engine::math::snapSmallState(s1);
        fad::engine::math::snapSmallState(s2);
        if (! utils::isFinite(s1) || ! utils::isFinite(s2))
        {
            s1 = 0.0f;
            s2 = 0.0f;
            return 0.0f;
        }

        const auto outputForMode = [drivenInput, v1, v2, damping = k]
            (parameters::FilterMode mode) noexcept
        {
            switch (mode)
            {
                case parameters::FilterMode::bandPass: return v1;
                case parameters::FilterMode::highPass: return drivenInput - damping * v1 - v2;
                case parameters::FilterMode::lowPass:
                default: return v2;
            }
        };

        float output = outputForMode(filterMode);
        output = utils::isFinite(output) ? output : 0.0f;
        if (modeFadeRemaining > 0)
        {
            const auto sourceOutput = outputForMode(modeFadeSourceMode);
            if (modeFadeNeedsCorrection)
            {
                modeFadeSourceCorrection = lastOutputSample - sourceOutput;
                modeFadeNeedsCorrection = false;
            }
            const auto fadeOut = static_cast<float>(modeFadeRemaining)
                / static_cast<float>(modeFadeSamples);
            output = (sourceOutput + modeFadeSourceCorrection) * fadeOut
                + output * (1.0f - fadeOut);
            --modeFadeRemaining;
            if (modeFadeRemaining == 0)
            {
                modeFadeSourceMode = filterMode;
                modeFadeSourceCorrection = 0.0f;
            }
        }

        lastOutputSample = nodeFilter.process(utils::isFinite(output) ? output : 0.0f);
        hasProcessedSample = true;
        return lastOutputSample;
    }

    const auto processOnePole = [this](float input) noexcept
    {
        const auto v = (input - onePoleStates[0]) * onePoleG;
        const auto low = v + onePoleStates[0];
        onePoleStates[0] = low + v;
        fad::engine::math::snapSmallState(onePoleStates[0]);
        if (! utils::isFinite(onePoleStates[0]))
            onePoleStates[0] = 0.0f;

        if (filterMode == parameters::FilterMode::highPass)
            return input - low;
        return low;
    };

    const auto processNativeSixDb = [this](float input) noexcept
    {
        auto& state = onePoleStates[0];
        const auto low = state;
        const auto high = input - state;
        state += nativeOnePoleF * high;
        fad::engine::math::snapSmallState(state);
        if (! utils::isFinite(state))
        {
            state = 0.0f;
            return 0.0f;
        }
        return nativeFilterModel == 8 ? high : low;
    };

    const auto processSvf = [this](float input, size_t stageIndex, int response = -1, bool variation = false) noexcept
    {
        const auto nativeCascade = stageIndex == 1 && !variation
            && (nativeFilterModel == 7 || nativeFilterModel == 11 || nativeFilterModel == 32);
        const auto ca1 = variation ? variationA1 : nativeCascade ? cascadeA1 : a1;
        const auto ca2 = variation ? variationA2 : nativeCascade ? cascadeA2 : a2;
        const auto ca3 = variation ? variationA3 : nativeCascade ? cascadeA3 : a3;
        const auto damping = variation ? variationK : nativeCascade ? cascadeK : k;
        float& state1 = stageIndex == 0 ? s1 : stageS1[stageIndex];
        float& state2 = stageIndex == 0 ? s2 : stageS2[stageIndex];

        const float v3 = input - state2;
        const float v1 = ca1 * state1 + ca2 * v3;
        const float v2 = state2 + ca2 * state1 + ca3 * v3;
        const float newState1 = 2.0f * v1 - state1;
        const float newState2 = 2.0f * v2 - state2;

        // Native drive is input gain followed by its input bound. Applying
        // the old normalized state tanh here cancels that measured gain.
        state1 = newState1;
        state2 = newState2;

        fad::engine::math::snapSmallState(state1);
        fad::engine::math::snapSmallState(state2);
        if (! utils::isFinite(state1) || ! utils::isFinite(state2))
        {
            state1 = 0.0f;
            state2 = 0.0f;
            return 0.0f;
        }

        if (response == 5)
        {
            // Measured L/B/H sweep: linear LP -> BP -> HP morph. The 24 dB
            // model cascades two morphed stages, rather than mixing their taps.
            const auto position = 2.0f * currentVariation;
            const auto low = std::max(0.0f, 1.0f - position);
            const auto high = std::max(0.0f, position - 1.0f);
            const auto band = 1.0f - std::abs(position - 1.0f);
            return low * v2 + band * v1 + high * (input - damping * v1 - v2);
        }
        if (response == 3) return input + (1.0f - damping) * v1; // HP + BP + LP
        if (response == 4) return input - damping * v1;         // HP + LP
        const auto mode = response < 0 ? filterMode : static_cast<parameters::FilterMode>(response);
        switch (mode)
        {
            case parameters::FilterMode::bandPass:
                return v1;
            case parameters::FilterMode::highPass:
                return input - damping * v1 - v2;
            case parameters::FilterMode::lowPass:
            default:
                return v2;
        }
    };

    // Plain LP/HP models share the measured cubic input stage. FX callers apply
    // it before their separate resampling step; main filters apply it here.
    const auto driven = inputSample * driveAmount;
    const auto shaped = nativeInputPreShaped ? inputSample
        : nativeFilterModel >= 4 && nativeFilterModel <= 11
            ? serumFilterDrive(driven, 4.0f) : std::clamp(driven, -4.0f, 4.0f);
    float output = shaped * nativePassbandGain;
    if (nativeFilterModel == 4 || nativeFilterModel == 8)
    {
        if (nativeInputPreShaped)
        {
            // FilterFx owns the host -> 2x interpolation/averaging contract.
            output = processNativeSixDb(inputSample);
        }
        else
        {
            // Main-filter probes establish the same 2x linear interpolation as
            // FX Filter: midpoint then current sample, averaged back to host
            // rate. Main-filter drive is evaluated at those 2x sub-samples;
            // shaping host samples first incorrectly softens fast transitions.
            const auto midpoint = serumFilterDrive(.5f * (nativeBasicPreviousInput + driven), 4.0f);
            const auto current = serumFilterDrive(driven, 4.0f);
            const auto first = processNativeSixDb(midpoint);
            const auto second = processNativeSixDb(current);
            nativeBasicPreviousInput = driven;
            output = .5f * (first + second);
        }
    }
    else if (nativeFilterModel == 41 || nativeFilterModel == 51 || nativeFilterModel == 52)
        output = nativeComb.processSample(inputSample);
    else if (nativeFilterModel == 94 || nativeFilterModel == 95)
        output = nativeScream.processSample(inputSample);
    else if (nativeFilterModel == 59 || nativeFilterModel == 61)
    {
        // Preserve the measured six/eight all-pass stages and delayed feedback.
        // The main-voice adapter uses bounded linear 2x input interpolation;
        // native whole-voice resampling still has a measurable high-band residual.
        nativePhase.processSample(.5f * (nativePhasePreviousInput + driven));
        output = nativePhase.processSample(driven);
        nativePhasePreviousInput = driven;
    }
    else if (nativeFilterModel >= 0 && nativeFilterModel <= 3)
    {
        // All MG slopes share one four-pole feedback loop. The slope chooses
        // an output tap; four independent cascades lose its resonant response.
        const auto G = onePoleG;
        const auto G2 = G*G, G3 = G2*G, G4 = G3*G;
        const auto history = (1.0f-G)*(G3*onePoleStates[0]+G2*onePoleStates[1]
            +G*onePoleStates[2]+onePoleStates[3]);
        auto stageInput = (output-nativeFeedback*history)/(1.0f+nativeFeedback*G4);
        for (size_t stage=0;stage<onePoleStates.size();++stage)
        {
            const auto delta = (stageInput-onePoleStates[stage])*G;
            const auto value = delta+onePoleStates[stage];
            onePoleStates[stage] = value+delta;
            fad::engine::math::snapSmallState(onePoleStates[stage]);
            if (!utils::isFinite(onePoleStates[stage])) {
                onePoleStates.fill(0);stageInput=0;output=0;break;
            }
            if (stage == static_cast<size_t>(nativeFilterModel)) output=value;
            stageInput=value;
        }
    }
    else if (nativeFilterModel == 31 || nativeFilterModel == 32)
    {
        output = processSvf(output, 0, 5);
        if (nativeFilterModel == 32)
            output = processSvf(output, 1, 5);
    }
    else if (nativeFilterModel == 14 || nativeFilterModel == 16)
    {
        output = processSvf(output, 0, nativeFilterModel == 14 ? 3 : 4);
    }
    else if (nativeFilterModel == 24 || nativeFilterModel == 25 || nativeFilterModel == 27)
    {
        output = processSvf(output, 0, nativeFilterModel == 27
            ? static_cast<int>(parameters::FilterMode::bandPass)
            : static_cast<int>(parameters::FilterMode::highPass));
        output = processSvf(output, 1, nativeFilterModel == 24 ? 3 : 4, true);
    }
    else switch (filterOrder)
    {
        case 1:
            output = processOnePole(output);
            break;
        case 3:
            output = processOnePole(output);
            output = processSvf(output, 0);
            break;
        case 4:
            output = processSvf(output, 0);
            output = processSvf(output, 1);
            break;
        case 2:
        default:
            output = processSvf(output, 0);
            break;
    }

    if (! utils::isFinite(output))
        output = 0.0f;

    if (modeFadeRemaining > 0)
    {
        const auto sourceOutput = output;
        if (modeFadeNeedsCorrection)
        {
            modeFadeSourceCorrection = lastOutputSample - sourceOutput;
            modeFadeNeedsCorrection = false;
        }

        const auto fadeOut = static_cast<float>(modeFadeRemaining)
            / static_cast<float>(modeFadeSamples);
        output = (sourceOutput + modeFadeSourceCorrection) * fadeOut
            + output * (1.0f - fadeOut);
        --modeFadeRemaining;
        if (modeFadeRemaining == 0)
        {
            modeFadeSourceMode = filterMode;
            modeFadeSourceCorrection = 0.0f;
        }
    }

    lastOutputSample = nodeFilter.process(utils::isFinite(output) ? output : 0.0f);
    hasProcessedSample = true;
    return lastOutputSample;
}
float FilterBlock::processSample(float inputSample,
                                 const parameters::FilterSnapshot& snapshot) noexcept
{
    updateCoefficients(snapshot);
    return processSample(inputSample);
}

FilterBlock::RuntimeState FilterBlock::getRuntimeState() const noexcept
{
    return {currentCutoffHz, currentResonance, driveAmount};
}
} // namespace fadmodel1::dsp::filters
