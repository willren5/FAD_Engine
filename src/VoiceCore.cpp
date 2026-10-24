#include <fad/engine/math/RationalExp.h>
#include <fad/engine/modulation/NativeLfoRate.h>
#include <fad/engine/modulation/NativeLfoBus.h>
#include <fad/engine/parameters/SerumCutoffCurve.h>
// Keep implicit multiply/add rounding aligned with the Windows precise build,
// including inline pitch and phase calculations from headers. Explicit fma remains fused.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

// Copyright (c) 2024-2026 阿勒泰自由意文化服务有限责任公司 (FAD Records). All rights reserved.
// Licensed under the openFAD 0.1 or a separate Commercial License. See LICENSE.

#include <fad/engine/synth/VoiceCore.h>
#include <fad/engine/synth/PerformanceCurve.h>
#include <fad/engine/synth/NativeOctave.h>
#include <fad/engine/oscillators/SerumSubWaveforms.h>
#include <fad/engine/oscillators/SerumUnison.h>

#include <algorithm>
#include <bit>
#if defined(_MSC_VER) && defined(_M_X64)
#include <immintrin.h>
#endif

#include <fad/engine/math/DspSafetyUtils.h>

namespace fadmodel1::dsp::synth
{
namespace
{
bool usesNativePhaseMemory(const parameters::ParameterSnapshot& snapshot, size_t oscillator) noexcept
{
    const auto& osc = snapshot.oscillators[oscillator];
    // Phase=Mem disables ordinary randomization. Rand=1 in modern chunks is
    // the distinct MemTrig setting, not covered by the stopped-phase contract.
    return snapshot.global.serumAmplitude && osc.phaseDegrees == 360.0f && osc.unisonRand < 1.0f;
}

constexpr float maxFilterEnvelopeOctaves = 5.0f;
constexpr float maxFilterResonance = 0.97f;
constexpr int filterKeyTrackReferenceNote = 60;
constexpr double minGlideSmoothingSeconds = 0.005; // minimum ramp to avoid zipper
constexpr float ln2 = 0.6931471805599453f;
constexpr int panTableSize = 256;
constexpr int filterCoefficientUpdateInterval = 32;
// Serum 1.368 Sub probes (sine and triangle) share a fixed fresh-note phase
// anchor roughly fifteen degrees before the calibrated table origin. The table
// itself is an output waveform calibration, so keep the oscillator's note-on
// phase contract separate from the stored samples.
constexpr float serumSubStartPhase01 = 23.0f / 24.0f;
constexpr double fallbackVoiceSampleRate = 44100.0;
constexpr uint32_t fallbackVoiceBlockSize = 512;
constexpr uint32_t fallbackVoiceChannelCount = 2;
static_assert((filterCoefficientUpdateInterval & (filterCoefficientUpdateInterval - 1)) == 0,
              "Filter coefficient update bitmask requires a power-of-two interval");

bool sameFloatBits(float lhs, float rhs) noexcept
{
    return std::bit_cast<uint32_t>(lhs) == std::bit_cast<uint32_t>(rhs);
}

float fastExp2(float exponent) noexcept
{
    if (exponent == 0.0f)
        return 1.0f;
    if (! utils::isFinite(exponent))
        return 1.0f;

    exponent = std::clamp(exponent, -12.0f, 12.0f);
    // The bounded finite range makes truncation safe. Adjust negative
    // fractions to retain floor's exact integer result without a CRT call.
    const auto truncated = static_cast<int>(exponent);
    const auto integerPart = truncated - (exponent < static_cast<float>(truncated) ? 1 : 0);
    const auto fractionalPart = exponent - static_cast<float>(integerPart);
    const auto fractionalRatio =
        fad::engine::math::rationalExp(fractionalPart * ln2);
    // The clamped exponent keeps this exact power of two in the normal float
    // range. Multiplication has the same result as ldexp without a library call
    // for every drifting oscillator sample.
    const auto integerRatio = std::bit_cast<float>(static_cast<uint32_t>(integerPart + 127) << 23);
    return fractionalRatio * integerRatio;
}

void fastExp2Four(float* exponents) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    auto values = _mm_loadu_ps(exponents);
    const auto finite = _mm_cmplt_epi32(_mm_and_si128(_mm_castps_si128(values),
        _mm_set1_epi32(0x7fffffff)), _mm_set1_epi32(0x7f800000));
    values = _mm_and_ps(values, _mm_castsi128_ps(finite));
    values = _mm_max_ps(_mm_set1_ps(-12.0f), _mm_min_ps(_mm_set1_ps(12.0f), values));
    auto integer = _mm_cvttps_epi32(values);
    integer = _mm_add_epi32(integer,
        _mm_castps_si128(_mm_cmplt_ps(values, _mm_cvtepi32_ps(integer))));
    const auto x = _mm_mul_ps(_mm_sub_ps(values, _mm_cvtepi32_ps(integer)), _mm_set1_ps(ln2));
    auto numerator = _mm_add_ps(_mm_set1_ps(20.0f), x);
    numerator = _mm_add_ps(_mm_set1_ps(180.0f), _mm_mul_ps(x, numerator));
    numerator = _mm_add_ps(_mm_set1_ps(840.0f), _mm_mul_ps(x, numerator));
    numerator = _mm_add_ps(_mm_set1_ps(1680.0f), _mm_mul_ps(x, numerator));
    auto denominator = _mm_add_ps(_mm_set1_ps(-20.0f), x);
    denominator = _mm_add_ps(_mm_set1_ps(180.0f), _mm_mul_ps(x, denominator));
    denominator = _mm_add_ps(_mm_set1_ps(-840.0f), _mm_mul_ps(x, denominator));
    denominator = _mm_add_ps(_mm_set1_ps(1680.0f), _mm_mul_ps(x, denominator));
    const auto power = _mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(integer, _mm_set1_epi32(127)), 23));
    _mm_storeu_ps(exponents, _mm_mul_ps(_mm_div_ps(numerator, denominator), power));
#else
    for (int lane = 0; lane < 4; ++lane)
        exponents[lane] = fastExp2(exponents[lane]);
#endif
}

struct PanTable
{
    std::array<float, panTableSize + 1> left{};
    std::array<float, panTableSize + 1> right{};

    explicit PanTable(bool vital = false)
    {
        for (int i = 0; i <= panTableSize; ++i)
        {
            const auto x = static_cast<float>(i) / static_cast<float>(panTableSize);
            left[static_cast<size_t>(i)] = vital ? std::sqrt(2.0f) * std::cos(x * (std::numbers::pi_v<float> / 2.0f)) : std::sqrt(1.0f - x);
            right[static_cast<size_t>(i)] = vital ? std::sqrt(2.0f) * std::sin(x * (std::numbers::pi_v<float> / 2.0f)) : std::sqrt(x);
        }
    }
};

const PanTable equalPowerPanTable{};
const PanTable vitalPanTable{true};

uint32_t advanceUnisonRandomState(uint32_t state) noexcept
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float nextUnisonRandomUnit(uint32_t& state) noexcept
{
    state = advanceUnisonRandomState(state);
    return static_cast<float>(state & 0xFFFFu) / 65536.0f;
}

float nextUnisonRandomBipolar(uint32_t& state) noexcept
{
    state = advanceUnisonRandomState(state);
    return static_cast<float>(static_cast<int32_t>(state)) * (1.0f / 2147483648.0f);
}

float calculateModSmoothCoeff(double safeSampleRate) noexcept
{
    return 1.0f - std::exp(-1.0f / (0.005f * static_cast<float>(safeSampleRate)));
}

bool hasResidualModulation(const modulation::DestinationValues& values) noexcept
{
    const auto isResidual = [](float value) noexcept
    {
        constexpr float residualThreshold = 0.000001f;
        return ! utils::isFinite(value) || std::abs(value) > residualThreshold;
    };

    if (isResidual(values.nativeMasterPitchSemitones)
        || isResidual(values.nativeFilterCutoffOffset)
        || isResidual(values.nativeFilterResonanceOffset)
        || isResidual(values.nativeFilterDriveOffset)
        || isResidual(values.nativeFilterVariationOffset)
        || isResidual(values.nativeFilterMixOffset)
        || isResidual(values.filterCutoffOctaves)
        || isResidual(values.filterResonanceOffset)
        || isResidual(values.filterDriveDb)
        || isResidual(values.globalPitchSemitones)
        || isResidual(values.subLevelOffset)
        || isResidual(values.noiseLevelOffset)
        || isResidual(values.nativeNoisePitchOffset))
    {
        return true;
    }

    for (size_t index = 0; index < values.oscillatorPitchSemitones.size(); ++index)
    {
        if (isResidual(values.nativeUnisonDetuneOffsets[index]) || isResidual(values.nativeUnisonWidthOffsets[index]) || isResidual(values.nativeOctaveOffsets[index]) || isResidual(values.nativeCoarsePitchSemitones[index]) || isResidual(values.nativeFinePitchCents[index])
            || isResidual(values.nativeSemiPitchSemitones[index]) || isResidual(values.nativePanOffsets[index])
            || isResidual(values.oscillatorPitchSemitones[index])
            || isResidual(values.oscillatorLevelOffsets[index])
            || isResidual(values.wavetablePositionOffsets[index])
            || isResidual(values.fmIndexOffsets[index])
            || isResidual(values.oscillatorPanOffsets[index]))
        {
            return true;
        }
    }

    return false;
}



struct ResidualSummary
{
    bool any = false, pitch = false, snapshot = false;
};
ResidualSummary summarizeResiduals(const modulation::DestinationValues& v) noexcept
{
    ResidualSummary r;
    const auto residual = [](float x) noexcept { return !utils::isFinite(x) || std::abs(x) > .000001f; };
    r.pitch = residual(v.nativeMasterPitchSemitones) || residual(v.globalPitchSemitones);
    r.any = r.pitch || residual(v.nativeFilterCutoffOffset) || residual(v.nativeFilterResonanceOffset)
        || residual(v.nativeFilterDriveOffset) || residual(v.nativeFilterVariationOffset)
        || residual(v.nativeFilterMixOffset) || residual(v.filterCutoffOctaves)
        || residual(v.filterResonanceOffset) || residual(v.filterDriveDb)
        || residual(v.subLevelOffset) || residual(v.noiseLevelOffset) || residual(v.nativeNoisePitchOffset);
    for (size_t i = 0; i < v.oscillatorPitchSemitones.size(); ++i)
    {
        const bool pitch = residual(v.nativeUnisonDetuneOffsets[i]) || residual(v.nativeOctaveOffsets[i])
            || residual(v.nativeCoarsePitchSemitones[i]) || residual(v.nativeFinePitchCents[i])
            || residual(v.nativeSemiPitchSemitones[i]) || residual(v.oscillatorPitchSemitones[i]);
        const bool snapshot = residual(v.nativePanOffsets[i]) || residual(v.oscillatorLevelOffsets[i])
            || residual(v.wavetablePositionOffsets[i]) || residual(v.fmIndexOffsets[i])
            || residual(v.oscillatorPanOffsets[i]);
        r.pitch = r.pitch || pitch;
        r.snapshot = r.snapshot || snapshot;
        r.any = r.any || pitch || snapshot || residual(v.nativeUnisonWidthOffsets[i]);
    }
    return r;
}
#if defined(FAD_NOISE_SUMMARY_PROTOTYPE)
extern "C" uint32_t noiseResidualSummaryForTest(const modulation::DestinationValues& v) noexcept
{
    const auto r = summarizeResiduals(v);
    return uint32_t(r.any) | (uint32_t(r.pitch) << 1) | (uint32_t(r.snapshot) << 2);
}
#endif

struct RenderRouteSummary
{
    enum Flag : unsigned
    {
        ampSustain, ampDecay, ampRelease, nativeMasterPitch, nativeNoisePitch,
        nativeSources, nativeNoiseSource, nativeWidth0, nativeWidth1,
        activeModulation, oscillatorPitch, oscillatorSnapshot
    };
    uint32_t flags = 0;
    uint32_t rateMask = 0;
    bool has(Flag flag) const noexcept { return (flags & (uint32_t{1} << flag)) != 0; }
};

RenderRouteSummary summarizeRenderRoutes(const parameters::ModulationSnapshot& snapshot,
                                        bool preserveSmallAmounts) noexcept
{
    RenderRouteSummary result;
    for (const auto& route : snapshot.slots)
    {
        if (! route.hasSource())
            continue;
        if (! route.hasEffectiveAmount(preserveSmallAmounts))
            continue;
        const auto set = [&](RenderRouteSummary::Flag flag)
        {
            result.flags |= uint32_t{1} << flag;
        };
        if (route.nativeTarget == 38)
            set(RenderRouteSummary::ampSustain);
        if (route.nativeTarget == 37)
            set(RenderRouteSummary::ampDecay);
        if (route.nativeTarget == 39)
            set(RenderRouteSummary::ampRelease);
        if (route.nativeTarget == 80)
            set(RenderRouteSummary::nativeMasterPitch);
        if (route.nativeTarget == 28)
            set(RenderRouteSummary::nativeNoisePitch);
        if (route.nativeSource > 0 || route.nativeAuxSource > 0
            || (route.nativeTarget >= 299 && route.nativeTarget <= 314))
            set(RenderRouteSummary::nativeSources);
        if (route.nativeSource == 19 || route.nativeAuxSource == 19)
            set(RenderRouteSummary::nativeNoiseSource);
        const auto index = parameters::nativeLfoRateTargetIndex(route.nativeTarget);
        if (index >= 0)
            result.rateMask |= uint32_t{1} << index;
        if (route.nativeTarget == 172)
            set(RenderRouteSummary::nativeWidth0);
        if (route.nativeTarget == 173)
            set(RenderRouteSummary::nativeWidth1);
        if ((route.nativeTarget >= 0 && route.nativeTarget < 88)
            || route.nativeTarget == 172 || route.nativeTarget == 173
            || (route.nativeTarget < 0
                && route.destination != parameters::ModDestination::none))
            set(RenderRouteSummary::activeModulation);
        const bool nativePitch = route.nativeTarget == 7 || route.nativeTarget == 20
            || route.nativeTarget == 80 || route.nativeTarget == 10 || route.nativeTarget == 23
            || route.nativeTarget == 5 || route.nativeTarget == 18 || route.nativeTarget == 4
            || route.nativeTarget == 17 || route.nativeTarget == 3 || route.nativeTarget == 16;
        const bool legacyPitch = route.destination == parameters::ModDestination::osc1Pitch
            || route.destination == parameters::ModDestination::osc2Pitch
            || route.destination == parameters::ModDestination::globalPitch;
        if (nativePitch || legacyPitch)
            set(RenderRouteSummary::oscillatorPitch);
        // The existing snapshot predicate requires a legacy destination too.
        if (route.destination == parameters::ModDestination::none)
            continue;
        if (route.nativeTarget == 2 || route.nativeTarget == 15)
            set(RenderRouteSummary::oscillatorSnapshot);
        switch (route.destination)
        {
            case parameters::ModDestination::osc1Level:
            case parameters::ModDestination::osc2Level:
            case parameters::ModDestination::osc1WavetablePosition:
            case parameters::ModDestination::osc2WavetablePosition:
            case parameters::ModDestination::osc1FmIndex:
            case parameters::ModDestination::osc2FmIndex:
            case parameters::ModDestination::osc1Pan:
            case parameters::ModDestination::osc2Pan:
                set(RenderRouteSummary::oscillatorSnapshot);
                break;
            default:
                break;
        }
    }
    return result;
}

}


#if defined(FAD_NOISE_SUMMARY_PROTOTYPE)
extern "C" uint64_t noiseRouteSummaryForTest(const parameters::ModulationSnapshot& snapshot, bool vital) noexcept
{
    const auto summary = summarizeRenderRoutes(snapshot, vital);
    return summary.flags | (static_cast<uint64_t>(summary.rateMask) << 32);
}
#endif

namespace
{
const wavetable::WavetableData& nativeSubWavetable()
{
    static const auto data = [] {
        wavetable::WavetableData table;
        // Internal procedural calibration data. Application display metadata
        // remains in the private facade; render never reads a name/path.
        table.frameSize = 2048;
        table.frameCount = serumSubWaveforms.size();
        table.frames.reserve(table.frameCount);
        for (const auto& frame : serumSubWaveforms)
            table.frames.emplace_back(frame.begin(), frame.end());
        table.buildMipmap();
        return table;
    }();
    return data;
}
}

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
float voicePitchRatioForTest(float exponent) noexcept
{
    return fastExp2(exponent);
}

void voicePitchRatiosForTest(float* exponents) noexcept
{
    fastExp2Four(exponents);
}
#endif

float VoiceCore::untrackedTunedBaseHz(const parameters::OscillatorSnapshot& oscillator,
                                const parameters::GlobalSnapshot& global) noexcept
{
    const auto fine = global.serumAmplitude
        ? std::floor(oscillator.fineCents + 100.5f) - 100.0f : oscillator.fineCents;
    const auto tuning = static_cast<float>(oscillator.octave * 12 + oscillator.semitone);
    const auto extra = std::clamp(global.nativeTuneSemitones, -64.0f, 64.0f)
        + std::clamp(oscillator.nativeCoarseSemitones, -64.0f, 64.0f);
    return untrackedOscillatorBaseHz * fastExp2(tuning / 12.0f + fine / 1200.0f)
        * fastExp2(extra / 12.0f);
}

std::array<parameters::OscillatorSnapshot,2> VoiceCore::referenceOscillators(
    const parameters::ParameterSnapshot& snapshot, std::array<float,2>* frequencies) noexcept
{
    modulation::SourceValues sources;
    sources.macros = snapshot.macroValues;
    sources.velocity = sources.ampEnvelope = 1.0f;
    sources.filterEnvelope = snapshot.filterEnvelope.sustainLevel;
    sources.modEnvelope = snapshot.modEnvelope.sustainLevel;
    sources.nativeNoteNumber = 57.0f / 127.0f;
    sources.nativeVoiceActive = true;
    auto values = modulation::ModMatrix::render(snapshot.modulation, sources, snapshot.global.vitalAmplitude);
    resolveNativeTuning(snapshot, values);
    const auto context = prepareRenderContext(snapshot);
    auto result = snapshot.oscillators;
    for (size_t i = 0; i < result.size(); ++i)
    {
        updateRenderedOscillatorSnapshot(snapshot,i,values,snapshot.oscillators[i],result[i]);
        if (frequencies)
        {
            const auto pitch = values.oscillatorPitchSemitones[i] + values.globalPitchSemitones
                + values.nativeMasterPitchSemitones + values.nativeCoarsePitchSemitones[i]
                + values.nativeSemiPitchSemitones[i] + values.nativeFinePitchCents[i] * .01f
                + nativeOctavePitchOffset(result[i].octave,result[i].nativeOctavePosition,values.nativeOctaveOffsets[i]);
            (*frequencies)[i] = (result[i].pitchTracking ? 220.0f : untrackedOscillatorBaseHz)
                * context.zeroBendTuningRatios[i] * fastExp2(pitch / 12.0f);
        }
    }
    return result;
}

VoiceCore::VoiceCore()
{
    nativeSubTable = &nativeSubWavetable();
    prepare({ fallbackVoiceSampleRate, fallbackVoiceBlockSize, fallbackVoiceChannelCount });
}

void VoiceCore::prepare(const fad::engine::ProcessSpec& spec)
{
    sampleRate = fadmodel1::dsp::utils::sanitizeSampleRate(spec.sampleRate);
    const auto safeSampleRate = sampleRate;

    for (auto& oscillator : oscillators)
        oscillator.prepare(safeSampleRate);

    for (auto& unisonArray : unisonOscillators)
        for (auto& uosc : unisonArray)
            uosc.prepare(safeSampleRate);

    // Each oscillator borrows its own VoiceCore's fixed-address curve runtime.
    // prepare/reset do not replace it; rebinding every render subdivision
    // needlessly touches every inactive unison head for one-sample segments.
    for (size_t i = 0; i < oscillators.size(); ++i)
    {
        oscillators[i].setNodeCurve(&nodeWaves[i]);
        for (auto& oscillator : unisonOscillators[i])
            oscillator.setNodeCurve(&nodeWaves[i]);
    }

    for (auto& lfo : noteLfos) lfo.prepare(safeSampleRate);
    subOscillator.prepare(safeSampleRate);

    for (auto& filter : filters)
        filter.prepare(safeSampleRate);
    for (auto& gain : filterInputGains) { gain.reset(safeSampleRate, 0.005); gain.setCurrentAndTargetValue(1.0f); }
    filterWetGain.reset(safeSampleRate, 0.005);
    filterWetGain.setCurrentAndTargetValue(1.0f);
    filterMixBaseGain.reset(safeSampleRate, 0.005);
    filterMixBaseGain.setCurrentAndTargetValue(1.0f);
    filterEnabledGain.reset(safeSampleRate, 0.005);
    filterEnabledGain.setCurrentAndTargetValue(1.0f);


    ampEnvelope.setSampleRate(safeSampleRate);
    filterEnvelope.setSampleRate(safeSampleRate);
    modEnvelope.setSampleRate(safeSampleRate);
    baseFrequencySmoother.reset(safeSampleRate, minGlideSmoothingSeconds);
    velocitySmoother.reset(safeSampleRate, minGlideSmoothingSeconds);
    velocitySensitivitySmoother.reset(safeSampleRate, 0.005);
    channelVolumeSmoother.reset(safeSampleRate, 0.005);
    expressionSmoother.reset(safeSampleRate, 0.005);
    mpeManagerVolumeSmoother.reset(safeSampleRate, 0.005);
    mpeManagerExpressionSmoother.reset(safeSampleRate, 0.005);
    mpeManagerPitchSmoother.reset(safeSampleRate, 0.005);
    modSmoothCoeff = calculateModSmoothCoeff(safeSampleRate);
    stealFadeSamples = std::max(8, static_cast<int>(0.002 * safeSampleRate));
    structuralFadeSamples = std::clamp(fad::engine::math::roundToNearestInt(static_cast<float>(safeSampleRate) * 0.003f), 16, 256);
    reset();
    hasRememberedNativePhases = false;
}

void VoiceCore::reset()
{
    if (hasRenderedVoiceBodySample)
    {
        hasRememberedNativePhases = renderedSerumAmplitude;
        if (hasRememberedNativePhases)
            for (size_t oscillator = 0; oscillator < oscillators.size(); ++oscillator)
            {
                nativeRememberedPhases[oscillator][0] = oscillators[oscillator].getPhase01();
                for (size_t head = 0; head < unisonOscillators[oscillator].size(); ++head)
                    nativeRememberedPhases[oscillator][head + 1] =
                        unisonOscillators[oscillator][head].getPhase01();
            }
    }
    nativeRateRouteMask=0;ownedRateLfoMask=0;
    for (auto& oscillator : oscillators)
        oscillator.reset();

    // Keep native phase-memory history. Explicit phase retriggers below reset
    // the dependent waveform state through OscillatorBlock::setPhase.

    subOscillator.reset();

    for (auto& filter : filters)
        filter.reset();

    for (auto& lfo : noteLfos) lfo.reset();
    ampEnvelope.reset();
    filterEnvelope.reset();
    modEnvelope.reset();
    baseFrequencySmoother.setCurrentAndTargetValue(baseFrequencyHz);
    nativePitchRamp.reset(baseFrequencyHz);
    velocitySmoother.setCurrentAndTargetValue(noteVelocity);
    velocitySensitivitySmoother.setCurrentAndTargetValue(
        parameterSnapshot != nullptr ? parameterSnapshot->global.velocitySensitivity : 1.0f);
    channelVolumeSmoother.setCurrentAndTargetValue(1.0f);
    expressionSmoother.setCurrentAndTargetValue(1.0f);
    mpeManagerVolumeSmoother.setCurrentAndTargetValue(1.0f);
    mpeManagerExpressionSmoother.setCurrentAndTargetValue(1.0f);
    mpeManagerPitchSmoother.setCurrentAndTargetValue(0.0f);
    stealFadeRemaining = 0;
    stealTailRemaining = 0;
    lastRenderedVoiceSample = {};
    lastRenderedVoiceBodySample = {};
    stealTailSample = {};
    structuralFadeStartSample = {};
    lastRenderedDirectBody = {}; stealDirectTailSample = {}; structuralDirectFadeStart = {};
    structuralFadeRemaining = 0;
    hasRenderedVoiceBodySample = false;
    renderedUnisonVoiceCounts.fill(1);
    filterCoefficientSampleCounter = 0;
    nativeNoiseModulationValue = 0.0f;
    nativeNoiseModulationPhase = 0;
    nativeReleaseResumeGraceRemaining = 0;
}

void VoiceCore::hardReset() noexcept
{
    reset();
    hasRememberedNativePhases = false;
    for (auto& unisonArray : unisonOscillators)
        for (auto& oscillator : unisonArray)
            oscillator.reset();
    unisonRenderCaches = {};
    resetRandomState();
    resetRealtimeState();
}

void VoiceCore::setParameterSnapshot(const parameters::ParameterSnapshot* snapshot) noexcept
{
    parameterSnapshot = snapshot;
    // Retire borrowed replay pointers before MIDI note/seed work as well as
    // rendering. The bank may have changed ownership at this block boundary.
    if(snapshot)
    {
        for(size_t i=0;i<parameters::lfoCount;++i)if(snapshot->curves[i+2].history || nodeLfos[i].history)
            nodeLfos[i].prepare(modulation::modulatedCurve(snapshot->curves[i+2],{}));
        for(size_t i=0;i<2;++i)if(snapshot->curves[i].history || nodeWaves[i].history)
            nodeWaves[i].prepare(modulation::modulatedCurve(snapshot->curves[i],{}),sampleRate);
    }
    if (snapshot != nullptr)
        velocitySensitivitySmoother.setTargetValue(snapshot->global.velocitySensitivity);
    if (snapshot != nullptr && activeVelocityCurve != snapshot->global.velocityCurve)
    {
        activeVelocityCurve = snapshot->global.velocityCurve;
        noteVelocity = shapeVelocity(rawNoteVelocity, activeVelocityCurve);
        velocitySmoother.setTargetValue(noteVelocity);
    }
}

void VoiceCore::setPitchBendRange(float semitones) noexcept
{
    pitchBendRange = std::clamp(semitones, 0.0f, 128.0f);
    // RPN may change while an MPE note is releasing. Retain that note's bend;
    // startNote applies the new range to the next note on this channel.
    if (! mpeMember || isKeyDown())
        pitchBendSemitones = pitchBendRange * pitchWheelBipolar;
}

void VoiceCore::setMpeManagerControls(float volume, float expression,
                                  float pitchSemitones, bool snap) noexcept
{
    const auto safeVolume = std::clamp(utils::isFinite(volume) ? volume : 1.0f, 0.0f, 1.0f);
    const auto safeExpression = std::clamp(utils::isFinite(expression) ? expression : 1.0f, 0.0f, 1.0f);
    const auto safePitch = std::clamp(utils::isFinite(pitchSemitones) ? pitchSemitones : 0.0f, -96.0f, 96.0f);
    if (snap)
    {
        mpeManagerVolumeSmoother.setCurrentAndTargetValue(safeVolume);
        mpeManagerExpressionSmoother.setCurrentAndTargetValue(safeExpression);
        mpeManagerPitchSmoother.setCurrentAndTargetValue(safePitch);
    }
    else
    {
        mpeManagerVolumeSmoother.setTargetValue(safeVolume);
        mpeManagerExpressionSmoother.setTargetValue(safeExpression);
        mpeManagerPitchSmoother.setTargetValue(safePitch);
    }
}

void VoiceCore::seedModulationStateFromCurrentSources() noexcept
{
    smoothedModValues = renderCurrentModulationSeed();
    modulationResidualMaskValid = false;
}

VoiceCore::ContinuousOscillatorParameters VoiceCore::captureContinuousOscillatorParameters(
    const parameters::OscillatorSnapshot& snapshot) noexcept
{
    return {
        fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.timbre, 0.5f, 0.0f, 1.0f),
        snapshot.enabled
            ? fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.level, 0.0f, 0.0f, 1.0f)
            : 0.0f,
        fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.pan, 0.0f, -1.0f, 1.0f),
        fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.analogShape, 0.5f, 0.0f, 1.0f),
        fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.analogPulseWidth, 0.5f, 0.01f, 0.99f),
        fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.wavetablePosition, 0.0f, 0.0f, 1.0f),
        fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.wavetableWarp, 0.0f, 0.0f, 1.0f),
        fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.fmIndex, 0.0f, 0.0f, snapshot.wideFmIndex ? 100.0f : 10.0f),
        fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.fmFeedback, 0.0f, 0.0f, 1.0f),
        fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.fmLevel, 0.5f, 0.0f, 1.0f)
    };
}

void VoiceCore::seedContinuousParameterStateFromSnapshot() noexcept
{
    if (parameterSnapshot == nullptr)
        return;

    for (size_t i = 0; i < filterInputGains.size(); ++i)
        filterInputGains[i].setCurrentAndTargetValue(parameterSnapshot->filter.inputs[i] ? 1.0f : 0.0f);
    filterWetGain.setCurrentAndTargetValue(parameterSnapshot->filter.enabled
        ? fadmodel1::dsp::utils::sanitizeFiniteClamped(parameterSnapshot->filter.mix, 1.0f, 0.0f, 1.0f) : 0.0f);
    filterMixBaseGain.setCurrentAndTargetValue(fadmodel1::dsp::utils::sanitizeFiniteClamped(
        parameterSnapshot->filter.mix, 1.0f, 0.0f, 1.0f));
    filterEnabledGain.setCurrentAndTargetValue(parameterSnapshot->filter.enabled ? 1.0f : 0.0f);

    for (size_t oscillatorIndex = 0; oscillatorIndex < continuousOscillatorState.size(); ++oscillatorIndex)
    {
        const auto values = captureContinuousOscillatorParameters(
            parameterSnapshot->oscillators[oscillatorIndex]);
        continuousOscillatorState[oscillatorIndex] = values;
        continuousOscillatorTargets[oscillatorIndex] = values;
    }

    continuousSubLevel = fadmodel1::dsp::utils::sanitizeFiniteClamped(
        parameterSnapshot->subLevel, 0.0f, 0.0f, 1.0f);
    continuousSubLevelTarget = continuousSubLevel;
    continuousSubEnabled = continuousSubEnabledTarget = parameterSnapshot->subEnabled ? 1.0f : 0.0f;
    continuousSubDirect = continuousSubDirectTarget = parameterSnapshot->subDirectOut ? 1.0f : 0.0f;
    continuousSubPan = continuousSubPanTarget = fadmodel1::dsp::utils::sanitizeFiniteClamped(parameterSnapshot->subPan, 0.0f, -1.0f, 1.0f);
    continuousNoiseLevel = fadmodel1::dsp::utils::sanitizeFiniteClamped(
        parameterSnapshot->noiseLevel, 0.0f, 0.0f, parameterSnapshot->noiseWideLevel ? 4.0f : 1.0f);
    continuousNoiseLevelTarget = continuousNoiseLevel;
    continuousNoiseEnabled = continuousNoiseEnabledTarget = parameterSnapshot->noise.enabled ? 1.0f : 0.0f;
    continuousNoiseDirect = continuousNoiseDirectTarget = parameterSnapshot->noise.directOut ? 1.0f : 0.0f;

    int enabledOscillatorCount = 0;
    for (size_t oscillatorIndex = 0; oscillatorIndex < continuousOscillatorState.size(); ++oscillatorIndex)
    {
        if (parameterSnapshot->oscillators[oscillatorIndex].enabled
            && continuousOscillatorState[oscillatorIndex].level >= 0.0001f)
        {
            ++enabledOscillatorCount;
        }
    }
    continuousOscillatorMixScale = parameterSnapshot->global.serumAmplitude ? 2.0f : parameterSnapshot->global.vitalAmplitude ? 1.0f : enabledOscillatorCount > 1
        ? 1.0f / std::sqrt(static_cast<float>(enabledOscillatorCount))
        : 1.0f;
    continuousOscillatorMixScaleTarget = continuousOscillatorMixScale;
}

VoiceCore::RenderContext VoiceCore::prepareRenderContext(const parameters::ParameterSnapshot& snapshot) noexcept
{
    RenderContext context;
    context.source = &snapshot;
    context.preserveSmallAmounts = snapshot.global.vitalAmplitude;
    context.routeBits = classifyRenderRouteBits(snapshot.modulation, context.preserveSmallAmounts);
    for (size_t i = 0; i < context.oscillatorTargets.size(); ++i)
        context.oscillatorTargets[i] = captureContinuousOscillatorParameters(snapshot.oscillators[i]);
    context.filterMix = fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.filter.mix, 1.0f, 0.0f, 1.0f);
    context.subPan = fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.subPan, 0.0f, -1.0f, 1.0f);
    context.subLevel = fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.subLevel, 0.0f, 0.0f, 1.0f);
    context.noiseLevel = fadmodel1::dsp::utils::sanitizeFiniteClamped(snapshot.noiseLevel, 0.0f, 0.0f, snapshot.noiseWideLevel ? 4.0f : 1.0f);
    if (!snapshot.noise.keytrack)
        context.untrackedNoiseRate = NoiseSamplePlayer::playbackRate(
            snapshot.noise.pitch, snapshot.noise.fine, false, 0);
    context.noisePanGains = applyPan(1.0f, snapshot.noise.pan * 2.0f - 1.0f,
                                    snapshot.global.serumAmplitude);
    for (size_t i = 0; i < snapshot.oscillators.size(); ++i)
    {
        const auto& osc = snapshot.oscillators[i];
        const auto semitones = static_cast<float>(osc.octave * 12 + osc.semitone);
        const auto fine = snapshot.global.serumAmplitude
            ? std::floor(osc.fineCents + 100.5f) - 100.0f : osc.fineCents;
        context.zeroBendTuningRatios[i] = fastExp2(semitones/12.0f+fine/1200.0f);
    }
    return context;
}

void VoiceCore::updateContinuousParameterTargetsFromSnapshot(const RenderContext* context) noexcept
{
    if (context != nullptr && context->source != parameterSnapshot) context = nullptr;
    for (size_t i = 0; i < filterInputGains.size(); ++i)
        filterInputGains[i].setTargetValue(parameterSnapshot->filter.inputs[i] ? 1.0f : 0.0f);
    filterWetGain.setTargetValue(parameterSnapshot->filter.enabled
        ? (context != nullptr ? context->filterMix : fadmodel1::dsp::utils::sanitizeFiniteClamped(parameterSnapshot->filter.mix, 1.0f, 0.0f, 1.0f)) : 0.0f);
    filterMixBaseGain.setTargetValue(context != nullptr ? context->filterMix : fadmodel1::dsp::utils::sanitizeFiniteClamped(
        parameterSnapshot->filter.mix, 1.0f, 0.0f, 1.0f));
    filterEnabledGain.setTargetValue(parameterSnapshot->filter.enabled ? 1.0f : 0.0f);

    for (size_t oscillatorIndex = 0; oscillatorIndex < continuousOscillatorTargets.size(); ++oscillatorIndex)
    {
        continuousOscillatorTargets[oscillatorIndex] = context != nullptr ? context->oscillatorTargets[oscillatorIndex] : captureContinuousOscillatorParameters(
            parameterSnapshot->oscillators[oscillatorIndex]);
    }

    continuousSubEnabledTarget = parameterSnapshot->subEnabled ? 1.0f : 0.0f;
    continuousSubDirectTarget = parameterSnapshot->subDirectOut ? 1.0f : 0.0f;
    continuousSubPanTarget = context != nullptr ? context->subPan : fadmodel1::dsp::utils::sanitizeFiniteClamped(parameterSnapshot->subPan, 0.0f, -1.0f, 1.0f);
    continuousSubLevelTarget = context != nullptr ? context->subLevel : fadmodel1::dsp::utils::sanitizeFiniteClamped(
        parameterSnapshot->subLevel, 0.0f, 0.0f, 1.0f);
    continuousNoiseEnabledTarget = parameterSnapshot->noise.enabled ? 1.0f : 0.0f;
    continuousNoiseDirectTarget = parameterSnapshot->noise.directOut ? 1.0f : 0.0f;
    continuousNoiseLevelTarget = context != nullptr ? context->noiseLevel : fadmodel1::dsp::utils::sanitizeFiniteClamped(
        parameterSnapshot->noiseLevel, 0.0f, 0.0f, parameterSnapshot->noiseWideLevel ? 4.0f : 1.0f);
}

bool VoiceCore::continuousParameterSmoothingActive() const noexcept
{
    constexpr float threshold = 1.0e-6f;
    // Integer flags allow all independent pairs to be checked together.
    const auto differs = [] (float current, float target) noexcept -> unsigned
    {
        return ! (std::abs(current - target) <= threshold);
    };

    if (differs(continuousSubDirect, continuousSubDirectTarget)
        | differs(continuousSubEnabled, continuousSubEnabledTarget)
        | differs(continuousSubPan, continuousSubPanTarget)
        | differs(continuousSubLevel, continuousSubLevelTarget)
        | differs(continuousNoiseLevel, continuousNoiseLevelTarget)
        | differs(continuousNoiseEnabled, continuousNoiseEnabledTarget)
        | differs(continuousNoiseDirect, continuousNoiseDirectTarget)
        | differs(continuousOscillatorMixScale, continuousOscillatorMixScaleTarget))
    {
        return true;
    }

    for (size_t oscillatorIndex = 0; oscillatorIndex < continuousOscillatorState.size(); ++oscillatorIndex)
    {
        const auto& current = continuousOscillatorState[oscillatorIndex];
        const auto& target = continuousOscillatorTargets[oscillatorIndex];
        if (differs(current.timbre, target.timbre)
            | differs(current.level, target.level)
            | differs(current.pan, target.pan)
            | differs(current.analogShape, target.analogShape)
            | differs(current.analogPulseWidth, target.analogPulseWidth)
            | differs(current.wavetablePosition, target.wavetablePosition)
            | differs(current.wavetableWarp, target.wavetableWarp)
            | differs(current.fmIndex, target.fmIndex)
            | differs(current.fmFeedback, target.fmFeedback)
            | differs(current.fmLevel, target.fmLevel))
        {
            return true;
        }
    }

    return false;
}

void VoiceCore::advanceContinuousParameterState() noexcept
{
    const auto smooth = [this] (float& current, float target) noexcept
    {
        current = fadmodel1::dsp::utils::sanitizeFinite(current, target);
        const auto difference = target - current;
        current = std::abs(difference) <= 1.0e-6f
            ? target
            : current + modSmoothCoeff * difference;
    };

    for (size_t oscillatorIndex = 0; oscillatorIndex < continuousOscillatorState.size(); ++oscillatorIndex)
    {
        auto& current = continuousOscillatorState[oscillatorIndex];
        const auto& target = continuousOscillatorTargets[oscillatorIndex];
        smooth(current.timbre, target.timbre);
        smooth(current.level, target.level);
        smooth(current.pan, target.pan);
        smooth(current.analogShape, target.analogShape);
        smooth(current.analogPulseWidth, target.analogPulseWidth);
        smooth(current.wavetablePosition, target.wavetablePosition);
        smooth(current.wavetableWarp, target.wavetableWarp);
        smooth(current.fmIndex, target.fmIndex);
        smooth(current.fmFeedback, target.fmFeedback);
        smooth(current.fmLevel, target.fmLevel);
    }

    smooth(continuousSubLevel, continuousSubLevelTarget);
    smooth(continuousSubEnabled, continuousSubEnabledTarget);
    smooth(continuousSubPan, continuousSubPanTarget);
    smooth(continuousSubDirect, continuousSubDirectTarget);
    smooth(continuousNoiseLevel, continuousNoiseLevelTarget);
    smooth(continuousNoiseEnabled, continuousNoiseEnabledTarget);
    smooth(continuousNoiseDirect, continuousNoiseDirectTarget);
}

void VoiceCore::applyContinuousOscillatorParameters(
    size_t oscillatorIndex,
    parameters::OscillatorSnapshot& snapshot) const noexcept
{
    const auto& values = continuousOscillatorState[oscillatorIndex];
    snapshot.enabled = snapshot.enabled || values.level >= 0.0001f;
    snapshot.timbre = values.timbre;
    snapshot.level = values.level;
    snapshot.pan = values.pan;
    snapshot.analogShape = values.analogShape;
    snapshot.analogPulseWidth = values.analogPulseWidth;
    snapshot.wavetablePosition = values.wavetablePosition;
    snapshot.wavetableWarp = values.wavetableWarp;
    snapshot.fmIndex = values.fmIndex;
    snapshot.fmFeedback = values.fmFeedback;
    snapshot.fmLevel = values.fmLevel;
}

float VoiceCore::getGlobalLfoSourceValue(size_t lfoIndex, int sampleOffset) const noexcept
{
    if (parameterSnapshot == nullptr || lfoIndex >= parameterSnapshot->modulation.lfos.size())
        return 0.0f;

    const auto depth = fadmodel1::dsp::utils::sanitizeFiniteClamped(
        parameterSnapshot->modulation.lfos[lfoIndex].depth, 0.0f, 0.0f, 1.0f);
    if (depth <= 0.0001f)
        return 0.0f;

    if(parameterSnapshot->curves[lfoIndex+2].enabled)
    {
        if(!nodeRuntimesInitialised)nodeLfos[lfoIndex].prepare(parameterSnapshot->curves[lfoIndex+2]);
        const auto mode=parameterSnapshot->modulation.lfos[lfoIndex].triggerMode;
        const bool owned=mode>0 || (ownedRateLfoMask & (uint32_t{1}<<lfoIndex))!=0;
        if(!owned && globalLfos==nullptr)return 0.f;
        const auto& lfo=owned?noteLfos[lfoIndex]:(*globalLfos)[lfoIndex];
        const auto offset=owned && ownedRateLfoMask!=0?0:sampleOffset;
        const auto* bank=blockLfoCurveBanks?(*blockLfoCurveBanks)[lfoIndex]:nullptr;
        return nodeLfos[lfoIndex].sample(lfo.nodePhaseAtOffset(offset,mode==2,nodeLfos[lfoIndex].history?nodeLfos[lfoIndex].loopStart:bank?bank->curveLoopStart(lfoIndex):-1.f))*depth;
    }
    const auto mode = parameterSnapshot->modulation.lfos[lfoIndex].triggerMode;
    if (mode > 0 || (ownedRateLfoMask&(uint32_t{1}<<lfoIndex))!=0) {
        const auto ownedOffset=ownedRateLfoMask!=0?0:sampleOffset;
        if (blockLfoCurveBanks != nullptr) {
            const auto* bank = (*blockLfoCurveBanks)[lfoIndex];
            if (bank != nullptr && bank->frames.size() > lfoIndex && bank->frames[lfoIndex].size() >= 2)
                return noteLfos[lfoIndex].renderCurveSampleAtOffset(bank->frames[lfoIndex].data(), bank->frames[lfoIndex].size(), ownedOffset, mode == 2, bank->curveEndpoint(lfoIndex), bank->curveLoopStart(lfoIndex)) * depth;
        }
        return noteLfos[lfoIndex].renderSampleAtOffset(parameterSnapshot->modulation.lfos[lfoIndex].shape, ownedOffset, mode == 2) * depth;
    }
    if (globalLfoBlockValues != nullptr && (*globalLfoBlockValues)[lfoIndex] != nullptr)
        return (*globalLfoBlockValues)[lfoIndex][std::max(0, sampleOffset)] * depth;

    if (globalLfos == nullptr)
        return 0.0f;

    if (blockLfoCurveBanks != nullptr) {
        const auto* bank = (*blockLfoCurveBanks)[lfoIndex];
        if (bank != nullptr && bank->frames.size() > lfoIndex && bank->frames[lfoIndex].size() >= 2)
            return (*globalLfos)[lfoIndex].renderCurveSampleAtOffset(bank->frames[lfoIndex].data(), bank->frames[lfoIndex].size(), sampleOffset, false, bank->curveEndpoint(lfoIndex), bank->curveLoopStart(lfoIndex)) * depth;
    }
    return (*globalLfos)[lfoIndex].renderSampleAtOffset(
               parameterSnapshot->modulation.lfos[lfoIndex].shape,
               sampleOffset)
        * depth;
}

modulation::SourceValues VoiceCore::makeModulationSources(float amp, float filter, float mod, int offset,
                                                       bool includeNative) const noexcept
{
    const auto* manager = mpeMember && performanceStates != nullptr
        ? &(*performanceStates)[static_cast<size_t>(mpeManagerChannel - 1)] : nullptr;
    const auto pressure = manager != nullptr
        ? std::min(1.0f, currentAftertouchValue() + manager->aftertouch)
        : currentAftertouchValue();
    modulation::SourceValues values{getGlobalLfoSourceValue(0,offset),getGlobalLfoSourceValue(1,offset),
        getGlobalLfoSourceValue(2,offset),getGlobalLfoSourceValue(3,offset),filter,amp,mod,
        noteVelocity,manager != nullptr ? manager->modWheel : modWheelValue,
        pressure,parameterSnapshot->macroValues};
    if (!includeNative) return values;
    const std::array<float,4> lfos{values.lfo1,values.lfo2,values.lfo3,values.lfo4};
    for(size_t i=0;i<parameters::lfoCount;++i) {
        const auto depth=std::clamp(parameterSnapshot->modulation.lfos[i].depth, 0.0f, 1.0f);
        const auto raw=depth>.0001f ? (i<lfos.size()?lfos[i]:getGlobalLfoSourceValue(i,offset))/depth : 0.0f;
        const auto* bank=blockLfoCurveBanks!=nullptr ? (*blockLfoCurveBanks)[i] : nullptr;
        const bool curved=parameterSnapshot->curves[i+2].enabled || (bank!=nullptr && bank->frames.size()>i && bank->frames[i].size()>=2);
        values.nativeLfos[i]=curved ? raw : .5f*(raw+1.0f);
        values.lfoDepth[i]=depth;
        values.fadLfos[i]=raw;
        if (curved) values.unipolarLfoMask |= uint32_t{1} << i;
    }
    values.nativeNoteNumber=static_cast<float>(std::clamp(pitchNoteNumber, 0, 127))/127.0f;
    values.nativeNoteOn = nativeNoteOnValues;
    values.nativePitchBend=midi2PitchActive ? std::clamp(nativePitchBend+midi2PitchValue-.5f, 0.0f, 1.0f) : nativePitchBend;
    values.nativeChannelPressure=manager != nullptr
        ? std::min(1.0f, channelPressureValue + manager->aftertouch) : channelPressureValue;
    values.nativePolyPressure=polyAftertouchValue;
    values.nativeSlide=manager != nullptr
        ? manager->combinedSlide(midi2SlideActive ? midi2Slide : slideValue) : (midi2SlideActive ? midi2Slide : slideValue);
    values.nativeReleaseVelocity=releaseVelocity;
    values.learnedMidiControllers=learnedMidiControllers;
    auto activeBanks=blockLfoCurveBanks ? *blockLfoCurveBanks : std::array<const wavetable::WavetableData*,parameters::lfoCount>{};
    for(size_t i=0;i<parameters::lfoCount;++i)if(parameterSnapshot->curves[i+2].enabled)activeBanks[i]=nullptr;
    if (blockLfoCurveBanks != nullptr)
        modulation::applyNativeLfoBuses(parameterSnapshot->modulation, values, activeBanks,
            nativeCurveRuntimes, blockWavetableGeneration,
            [&](size_t i, const modulation::NativeLfoCurveRuntime& curve) noexcept {
                const auto mode = parameterSnapshot->modulation.lfos[i].triggerMode;
                if (mode > 0 || (ownedRateLfoMask & (uint32_t{1} << i)) != 0)
                    return noteLfos[i].renderPointCurveAtOffset(curve, ownedRateLfoMask != 0 ? 0 : offset, mode == 2);
                return globalLfos != nullptr ? (*globalLfos)[i].renderPointCurveAtOffset(curve, offset) : 0.0f;
            });
    return values;
}

void VoiceCore::resolveNativeTuning(modulation::DestinationValues& values) const noexcept
{
    resolveNativeTuning(*parameterSnapshot, values);
}

void VoiceCore::resolveNativeTuning(const parameters::ParameterSnapshot& snapshot,
    modulation::DestinationValues& values) noexcept
{
    values.nativeMasterPitchSemitones=std::clamp(fadmodel1::dsp::utils::sanitizeFinite(snapshot.global.nativeTuneSemitones,0.0f)+values.nativeMasterPitchSemitones, -64.0f, 64.0f);
    for(size_t i=0;i<2;++i) {
        const auto& oscillator=snapshot.oscillators[i];
        values.nativeCoarsePitchSemitones[i]=std::clamp(fadmodel1::dsp::utils::sanitizeFinite(oscillator.nativeCoarseSemitones,0.0f)+values.nativeCoarsePitchSemitones[i], -64.0f, 64.0f);
        if(values.nativeSemiPitchSemitones[i]!=0.0f)
            values.nativeSemiPitchSemitones[i]=std::clamp(std::floor(static_cast<float>(oscillator.semitone)+12.0f+values.nativeSemiPitchSemitones[i]+.5f)-12.0f, -12.0f, 12.0f)-static_cast<float>(oscillator.semitone);
        if(values.nativeFinePitchCents[i]!=0.0f) {
            const auto base=fadmodel1::dsp::utils::sanitizeFiniteClamped(oscillator.fineCents,0.0f,-100.0f,100.0f);
            const auto appliedBase=snapshot.global.serumAmplitude ? std::floor(base+100.5f)-100.0f : base;
            values.nativeFinePitchCents[i]=std::clamp(std::floor(base+100.0f+values.nativeFinePitchCents[i]+.5f)-100.0f, -100.0f, 100.0f)-appliedBase;
        }
    }
}

modulation::DestinationValues VoiceCore::renderCurrentModulationSeed() const noexcept
{
    if (parameterSnapshot == nullptr)
        return {};

    const auto sourceValues=makeModulationSources(0,0,0,0);

    auto values=modulation::ModMatrix::render(parameterSnapshot->modulation, sourceValues, parameterSnapshot->global.vitalAmplitude);
    resolveNativeTuning(values);
    return values;
}

void VoiceCore::setGlobalLfos(
    const std::array<modulation::Lfo, parameters::lfoCount>* lfos) noexcept
{
    globalLfos = lfos;
}

void VoiceCore::setGlobalLfoBlockValues(
    const std::array<const float*, parameters::lfoCount>* blockValues) noexcept
{
    globalLfoBlockValues = blockValues;
}

void VoiceCore::setPerformanceStates(const std::array<PerformanceState, 16>* states) noexcept
{
    performanceStates = states;
}

void VoiceCore::setDiagnosticsEnabled(bool enabled) noexcept
{
    diagnosticsEnabled = enabled;
}

const diagnostics::ModulationTraceSnapshot& VoiceCore::getLastModulationTrace() const noexcept
{
    return lastModulationTrace;
}

void VoiceCore::setBlockWavetablePointers(
    const std::array<const wavetable::WavetableData*, 2>* pointers,
    uint32_t generation) noexcept
{
    blockWavetablePointers = pointers;
    blockWavetableGeneration = generation;
}

void VoiceCore::setNoiseSeed(uint32_t seed) noexcept
{
    noiseSeed = seed;
    resetRandomState();
}

void VoiceCore::resetRandomState() noexcept
{
    noiseState = noiseSeed;
    unisonRandomState = noiseSeed != 0 ? noiseSeed : 0x6d2b79f5u;
    unisonNoteRandomState = unisonRandomState;
    nativeFallbackPhaseRandom.reset();
    nativeNoteOnValues = {};
    hasPendingNativePhaseRandomValues = false;
    // Initialize drift with unique slow rates per voice (0.05 - 0.3 Hz)
    for (size_t i = 0; i < driftStates.size(); ++i)
    {
        const uint32_t h = noiseSeed * 2654435761u + static_cast<uint32_t>(i) * 0x45d9f3b;
        driftStates[i].rate = 0.05f + (static_cast<float>(h & 0xFFFF) / 65535.0f) * 0.25f;
        driftStates[i].phase = static_cast<float>((h >> 16) & 0xFFFF) / 65535.0f;
        driftStates[i].currentValue = 0.0f;
    }

    // Clear cached per-note detune until the next startNote() seeds it against a concrete snapshot.
    for (size_t oscillatorIndex = 0; oscillatorIndex < unisonRandomDetuneCents.size(); ++oscillatorIndex)
        for (size_t unisonIndex = 0; unisonIndex < unisonRandomDetuneCents[oscillatorIndex].size(); ++unisonIndex)
            unisonRandomDetuneCents[oscillatorIndex][unisonIndex] = 0.0f;

    for (auto& oscillator : oscillators)
        oscillator.resetRandomState();
    for (auto& unisonArray : unisonOscillators)
        for (auto& oscillator : unisonArray)
            oscillator.resetRandomState();
    subOscillator.resetRandomState();
}

void VoiceCore::setRealtimeUnisonVoiceLimit(int maxVoices) noexcept
{
    realtimeUnisonVoiceLimit = std::clamp(maxVoices, 1, parameters::maxRenderedUnisonVoiceCount);
}

int VoiceCore::getRenderedUnisonCount(int requestedVoiceCount) const noexcept
{
    return std::clamp(requestedVoiceCount, 1, realtimeUnisonVoiceLimit);
}

uint32_t VoiceCore::createUnisonRandomState(size_t oscillatorIndex) const noexcept
{
    // Keep the existing Serum compatibility stream; ordinary Unison advances
    // once per retrigger even when noise is disabled.
    const auto seed = parameterSnapshot != nullptr && parameterSnapshot->global.serumAmplitude
        ? noiseState : unisonNoteRandomState;
    return seed
        ^ (static_cast<uint32_t>(std::max(0, pitchNoteNumber) + 1) * 0x9e3779b9u)
        ^ (static_cast<uint32_t>(oscillatorIndex + 1) * 0x85ebca6bu);
}

void VoiceCore::initialiseNewlyActivatedUnisonVoices(size_t oscillatorIndex,
                                                 UnisonVoiceInitialisationRange range) noexcept
{
    const auto newCount = range.currentVoiceCount;
    if (parameterSnapshot == nullptr
        || oscillatorIndex >= unisonOscillators.size()
        || newCount <= 1)
    {
        return;
    }

    const auto& oscillatorSnapshot = parameterSnapshot->oscillators[oscillatorIndex];
    const auto width = std::clamp(oscillatorSnapshot.unisonWidth, 0.0f, 1.0f);
    const auto randAmount = std::clamp(oscillatorSnapshot.unisonRand, 0.0f, 1.0f);
    const auto firstVoice = std::clamp(range.firstVoiceToInitialise, 1, newCount - 1);
    auto randomState = createUnisonRandomState(oscillatorIndex);
    if (oscillatorSnapshot.extendedUnison) (void)nextUnisonRandomUnit(randomState);

    for (int voiceIndex = 1; voiceIndex < newCount; ++voiceIndex)
    {
        const auto randomUnit = nextUnisonRandomUnit(randomState);

        if (voiceIndex < firstVoice)
            continue;

        const auto unisonIndex = static_cast<size_t>(voiceIndex - 1);
        if (unisonIndex >= unisonOscillators[oscillatorIndex].size())
            break;

        float phaseOffset = 0.0f;
        if (randAmount < 0.0001f)
        {
            const auto t = static_cast<float>(voiceIndex)
                / static_cast<float>(std::max(1, newCount - 1));
            phaseOffset = (t - 0.5f) * width * 0.12f;
        }
        else
        {
            phaseOffset = randomUnit * randAmount;
        }

        if (usesNativePhaseMemory(*parameterSnapshot, oscillatorIndex))
            unisonOscillators[oscillatorIndex][unisonIndex].setPhase(hasRememberedNativePhases
                ? nativeRememberedPhases[oscillatorIndex][static_cast<size_t>(voiceIndex)] : 0.0f, false);
        else if (parameterSnapshot->global.serumAmplitude)
            unisonOscillators[oscillatorIndex][unisonIndex].setPhase(
                oscillators::serumStartPhase(oscillatorSnapshot.phaseDegrees / 360.0f,
                    randAmount, nativePhaseRandomValues[oscillatorIndex][static_cast<size_t>(voiceIndex)]));
        else if (oscillatorSnapshot.extendedUnison)
            unisonOscillators[oscillatorIndex][unisonIndex].setPhase(
                oscillatorSnapshot.phaseDegrees / 360.0f + randomUnit * randAmount);
        else
            unisonOscillators[oscillatorIndex][unisonIndex].copyPhaseFrom(
                oscillators[oscillatorIndex], phaseOffset);
    }
}

void VoiceCore::beginStructuralFade() noexcept
{
    if (! hasRenderedVoiceBodySample)
        return;

    structuralFadeStartSample = lastRenderedVoiceBodySample;
    structuralDirectFadeStart = lastRenderedDirectBody;
    structuralFadeRemaining = structuralFadeSamples;
}

void VoiceCore::refreshRenderedUnisonCounts() noexcept
{
    if (parameterSnapshot == nullptr)
    {
        renderedUnisonVoiceCounts.fill(1);
        return;
    }

    for (size_t oscillatorIndex = 0; oscillatorIndex < renderedUnisonVoiceCounts.size(); ++oscillatorIndex)
    {
        renderedUnisonVoiceCounts[oscillatorIndex] =
            getRenderedUnisonCount(parameterSnapshot->oscillators[oscillatorIndex].unisonVoices);
    }
}

void VoiceCore::refreshRenderedUnisonVoiceCounts() noexcept
{
    const auto previousCounts = renderedUnisonVoiceCounts;
    refreshRenderedUnisonCounts();
    bool countChanged = false;

    for (size_t oscillatorIndex = 0; oscillatorIndex < renderedUnisonVoiceCounts.size(); ++oscillatorIndex)
    {
        const auto previousCount = previousCounts[oscillatorIndex];
        const auto newCount = renderedUnisonVoiceCounts[oscillatorIndex];

        if (newCount == previousCount || parameterSnapshot == nullptr)
            continue;

        countChanged = true;

        const auto& oscillatorSnapshot = parameterSnapshot->oscillators[oscillatorIndex];
        const auto deterministicPhaseLayout = oscillatorSnapshot.unisonRand < 0.0001f;
        if (deterministicPhaseLayout)
            initialiseNewlyActivatedUnisonVoices(oscillatorIndex,
                                                  UnisonVoiceInitialisationRange{
                                                      .firstVoiceToInitialise = 1,
                                                      .currentVoiceCount = newCount });
        else if (newCount > previousCount)
            initialiseNewlyActivatedUnisonVoices(oscillatorIndex,
                                                  UnisonVoiceInitialisationRange{
                                                      .firstVoiceToInitialise = previousCount,
                                                      .currentVoiceCount = newCount });
    }

    if (countChanged)
        beginStructuralFade();
}

void VoiceCore::seedUnisonRandomDetuneForNote() noexcept
{
    for (size_t oscillatorIndex = 0; oscillatorIndex < unisonRandomDetuneCents.size(); ++oscillatorIndex)
    {
        auto& detuneValues = unisonRandomDetuneCents[oscillatorIndex];
        detuneValues.fill(0.0f);

        if (parameterSnapshot == nullptr)
            continue;

        const auto& oscillatorSnapshot = parameterSnapshot->oscillators[oscillatorIndex];
        if (oscillatorSnapshot.extendedUnison || oscillatorSnapshot.unisonRand < 0.0001f)
            continue;

        auto rs = createUnisonRandomState(oscillatorIndex);
        for (int unisonIndex = 0; unisonIndex < parameters::maxRenderedUnisonVoiceCount; ++unisonIndex)
        {
            detuneValues[static_cast<size_t>(unisonIndex)] =
                nextUnisonRandomBipolar(rs) * oscillatorSnapshot.unisonRand * 3.0f;
        }
    }
}

void VoiceCore::glideToNote(int newMidiNoteNumber, float newVelocity) noexcept
{
    // Change pitch without retriggering envelopes — true legato
    const auto safeMidiNoteNumber = std::clamp(newMidiNoteNumber, 0, 127);
    pitchNoteNumber = safeMidiNoteNumber;
    noteState.number = safeMidiNoteNumber;
    baseFrequencyHz = static_cast<float>((440.0 * std::pow(2.0, (safeMidiNoteNumber - 69) / 12.0)));
    rawNoteVelocity = std::clamp(newVelocity, 0.0f, 1.0f);
    noteVelocity = shapeVelocity(rawNoteVelocity, activeVelocityCurve);
    releaseVelocity = 0.0f;

    if (usesNativePitchRamp())
    {
        nativePitchRamp.start(nativePitchRamp.getCurrentValue(), baseFrequencyHz, sampleRate,
            glideMode != parameters::GlideMode::off ? glideTimeSeconds * 1000.0f : 0.0f,
            parameterSnapshot->global.nativeGlideCurve, parameterSnapshot->global.nativeGlideScaled);
        baseFrequencySmoother.setCurrentAndTargetValue(baseFrequencyHz);
    }
    else if (glideMode != parameters::GlideMode::off && glideTimeSeconds > 0.001f)
    {
        // Reconfigure ramp time, then set new target to start smooth glide
        const auto rampTime = static_cast<double>(std::max(glideTimeSeconds, 0.005f));
        const auto currentFreq = baseFrequencySmoother.getCurrentValue();
        baseFrequencySmoother.reset(sampleRate, rampTime);
        baseFrequencySmoother.setCurrentAndTargetValue(currentFreq);
        baseFrequencySmoother.setTargetValue(baseFrequencyHz);
    }
    else
    {
        // Glide off: snap immediately
        baseFrequencySmoother.setCurrentAndTargetValue(baseFrequencyHz);
    }
    velocitySmoother.setTargetValue(noteVelocity);
}

void VoiceCore::glideToRealtimeNote(int midiChannel, int newMidiNoteNumber, float newVelocity) noexcept
{
    noteState.channel = std::clamp(midiChannel, 1, 16);
    cachedMidiChannel = noteState.channel;
    polyAftertouchValue = 0.0f;
    beginMidi2Note(48.0f, -1.0f);
    syncPerformanceStateFromChannel();
    glideToNote(newMidiNoteNumber, newVelocity);
}

void VoiceCore::initialiseNote(int newMidiNoteNumber,
                      float velocity,
                      int currentPitchWheelPosition)
{
    for (size_t i = 0; i < noteLfos.size(); ++i) {
        noteLfos[i].reset();
        if (globalLfos != nullptr) noteLfos[i].setRate((*globalLfos)[i].getRateHz());
        else if (parameterSnapshot != nullptr) noteLfos[i].setRate(parameterSnapshot->modulation.lfos[i].rateHz);
    }

    const auto nativeStartFrequency = pendingNativeGlideFrequency;
    pendingNativeGlideFrequency = 0.0f;
    const auto previousFrequencyHz = nativeStartFrequency;
    const auto safeMidiNoteNumber = std::clamp(newMidiNoteNumber, 0, 127);
    const auto safePitchWheelPosition = std::clamp(currentPitchWheelPosition, 0, 16383);
    nativePitchBend=std::clamp(static_cast<float>(safePitchWheelPosition)/16384.0f, .0001f, .9999f);
    this->pitchNoteNumber = safeMidiNoteNumber;
    if (parameterSnapshot != nullptr) {
        const auto& noise = parameterSnapshot->noise;
        // Reuse the per-voice deterministic seed only when random phase is requested.
        const double random = noise.randomPhase > 0.0f ? (renderNoiseSample() + 1.0) * 0.5 : 0.0;
        noiseSamplePlayer.reset(noise.phase + noise.randomPhase * random);
    } else noiseSamplePlayer.reset();
    nativeNoiseModulationValue = 0.0f;
    nativeNoiseModulationPhase = 0;
    baseFrequencyHz = static_cast<float>((440.0 * std::pow(2.0, (safeMidiNoteNumber - 69) / 12.0)));
    rawNoteVelocity = std::clamp(velocity, 0.0f, 1.0f);
    activeVelocityCurve = parameterSnapshot != nullptr ? parameterSnapshot->global.velocityCurve : 0.5f;
    noteVelocity = shapeVelocity(rawNoteVelocity, activeVelocityCurve);
    releaseVelocity = 0.0f;
    lastPitchWheelValue = safePitchWheelPosition;
    pitchWheelBipolar = static_cast<float>(safePitchWheelPosition - 8192) / 8192.0f;
    beginMidi2Note(48.0f,-1.0f);
    pitchBendSemitones = pitchBendRange
        * static_cast<float>(safePitchWheelPosition - 8192) / 8192.0f;
    // Cache the MIDI channel at note start (avoids per-sample lookup)
    cachedMidiChannel = 1;
    for (int ch = 1; ch <= 16; ++ch)
    {
        if (isPlayingChannel(ch)) { cachedMidiChannel = ch; break; }
    }
    polyAftertouchValue = 0.0f;
    updateEnvelopeParameters();
    ampEnvelope.setSustainModulationEnabled(std::any_of(parameterSnapshot->modulation.slots.begin(),parameterSnapshot->modulation.slots.end(),
        [](const auto& route) { return route.nativeTarget==38 && route.hasSource() && route.hasEffectiveAmount(); }));
    ampEnvelope.setDecayModulationEnabled(std::any_of(parameterSnapshot->modulation.slots.begin(),parameterSnapshot->modulation.slots.end(),
        [](const auto& route) { return route.nativeTarget==37 && route.hasSource() && route.hasEffectiveAmount(); }));
    ampEnvelope.setReleaseModulationEnabled(std::any_of(parameterSnapshot->modulation.slots.begin(),parameterSnapshot->modulation.slots.end(),
        [](const auto& route) { return route.nativeTarget==39 && route.hasSource() && route.hasEffectiveAmount(); }));
    const bool preserveStealTail = stealFadeRemaining > 0 || stealTailRemaining > 0;
    const int preservedStealTailRemaining = std::max(stealFadeRemaining, stealTailRemaining);
    const auto preservedStealTailSample = lastRenderedVoiceBodySample;
    const auto preservedDirectTail = lastRenderedDirectBody;

    reset();
    syncPerformanceStateFromChannel();
    channelVolumeSmoother.setCurrentAndTargetValue(channelVolumeSmoother.getTargetValue());
    expressionSmoother.setCurrentAndTargetValue(expressionSmoother.getTargetValue());

    if (preserveStealTail)
    {
        stealTailSample = preservedStealTailSample;
        stealDirectTailSample = preservedDirectTail;
        stealTailRemaining = preservedStealTailRemaining;
    }

    if (parameterSnapshot != nullptr && parameterSnapshot->global.serumAmplitude && !hasPendingNativePhaseRandomValues)
        nativePhaseRandomValues = nativeFallbackPhaseRandom.nextNote();
    hasPendingNativePhaseRandomValues = false;
    if (parameterSnapshot != nullptr && ! parameterSnapshot->global.serumAmplitude)
    {
        unisonNoteRandomState = unisonRandomState;
        unisonRandomState = advanceUnisonRandomState(unisonRandomState);
    }
    seedModulationStateFromCurrentSources();
    if (parameterSnapshot != nullptr)
    {
        seedContinuousParameterStateFromSnapshot();
        refreshRenderedUnisonCounts();
        seedUnisonRandomDetuneForNote();

        if (parameterSnapshot->global.serumAmplitude && parameterSnapshot->subNativeShape >= 0)
            subOscillator.setPhase(serumSubStartPhase01);

        for (size_t oscillatorIndex = 0; oscillatorIndex < oscillators.size(); ++oscillatorIndex)
        {
            const auto& osc = parameterSnapshot->oscillators[oscillatorIndex];
            auto phase01 = osc.phaseDegrees / 360.0f;
            if (usesNativePhaseMemory(*parameterSnapshot, oscillatorIndex))
                phase01 = hasRememberedNativePhases ? nativeRememberedPhases[oscillatorIndex][0] : 0.0f;
            else if (parameterSnapshot->global.serumAmplitude)
                phase01 = oscillators::serumStartPhase(phase01, osc.unisonRand,
                    nativePhaseRandomValues[oscillatorIndex][0]);
            else if (osc.extendedUnison)
            {
                auto randomState = createUnisonRandomState(oscillatorIndex);
                phase01 += nextUnisonRandomUnit(randomState) * std::clamp(osc.unisonRand, 0.0f, 1.0f);
            }
            const auto unisonCount = renderedUnisonVoiceCounts[oscillatorIndex];
            oscillators[oscillatorIndex].setPhase(phase01,
                ! usesNativePhaseMemory(*parameterSnapshot, oscillatorIndex));
            initialiseNewlyActivatedUnisonVoices(oscillatorIndex,
                                                  UnisonVoiceInitialisationRange{
                                                      .firstVoiceToInitialise = 1,
                                                      .currentVoiceCount = unisonCount });
        }
    }

    // Glide mode "Always" ramps from previous pitch; others snap immediately
    if (usesNativePitchRamp())
    {
        nativePitchRamp.start(nativeStartFrequency > 0.0f ? nativeStartFrequency : baseFrequencyHz,
            baseFrequencyHz, sampleRate,
            nativeStartFrequency > 0.0f && glideMode != parameters::GlideMode::off ? glideTimeSeconds * 1000.0f : 0.0f,
            parameterSnapshot->global.nativeGlideCurve, parameterSnapshot->global.nativeGlideScaled);
        baseFrequencySmoother.setCurrentAndTargetValue(baseFrequencyHz);
    }
    else if (nativeStartFrequency > 0.0f && glideMode != parameters::GlideMode::off
        && glideTimeSeconds > 0.001f && previousFrequencyHz > 0.0f)
    {
        const auto rampTime = static_cast<double>(std::max(glideTimeSeconds, 0.005f));
        baseFrequencySmoother.reset(sampleRate, rampTime);
        baseFrequencySmoother.setCurrentAndTargetValue(previousFrequencyHz);
        baseFrequencySmoother.setTargetValue(baseFrequencyHz);
    }
    else
    {
        baseFrequencySmoother.reset(sampleRate, minGlideSmoothingSeconds);
        baseFrequencySmoother.setCurrentAndTargetValue(baseFrequencyHz);
    }
    velocitySmoother.setCurrentAndTargetValue(noteVelocity);
    ampEnvelope.noteOn();
    filterEnvelope.noteOn();
    modEnvelope.noteOn();
}

void VoiceCore::stopNote(float velocity, bool allowTailOff)
{
    releaseVelocity = std::clamp(velocity, 0.0f, 1.0f);

    if (allowTailOff)
    {
        ampEnvelope.noteOff();
        filterEnvelope.noteOff();
        modEnvelope.noteOff();
        return;
    }

    // VoiceCore stolen: start quick fade-out instead of hard cut to prevent click
    stealFadeRemaining = stealFadeSamples;
}

void VoiceCore::pitchWheelMoved(int newPitchWheelValue)
{
    if (mpeMember && ! isKeyDown())
        return;
    const auto safePitchWheelValue = std::clamp(newPitchWheelValue, 0, 16383);
    lastPitchWheelValue = safePitchWheelValue;
    pitchWheelBipolar = static_cast<float>(safePitchWheelValue - 8192) / 8192.0f;
    nativePitchBend=std::clamp(static_cast<float>(safePitchWheelValue)/16384.0f, .0001f, .9999f);
    pitchBendSemitones = pitchBendRange
        * static_cast<float>(safePitchWheelValue - 8192) / 8192.0f;
}

void VoiceCore::setMidi2ChannelPitch(float bipolar) noexcept
{
    if (mpeMember && !isKeyDown()) return;
    pitchWheelBipolar=std::clamp(bipolar, -1.0f, 1.0f);
    pitchBendSemitones=pitchBendRange*pitchWheelBipolar;
    nativePitchBend=.5f+.5f*pitchWheelBipolar;
}

void VoiceCore::controllerMoved(int controllerNumber, int newControllerValue)
{
    if (controllerNumber == 1)
        modWheelValue = std::clamp(static_cast<float>(newControllerValue) / 127.0f, 0.0f, 1.0f);
    else if (controllerNumber == 7)
        channelVolumeSmoother.setTargetValue(std::clamp(static_cast<float>(newControllerValue) / 127.0f, 0.0f, 1.0f));
    else if (controllerNumber == 11)
        expressionSmoother.setTargetValue(std::clamp(static_cast<float>(newControllerValue) / 127.0f, 0.0f, 1.0f));
    else if (controllerNumber == 74)
    {
        if (! mpeMember || isKeyDown())
            slideValue = std::clamp(static_cast<float>(newControllerValue) / 127.0f, 0.0f, 1.0f);
    }
    else if (controllerNumber == 121)
    {
        polyAftertouchValue = 0.0f;
        syncPerformanceStateFromChannel();
    }
}

void VoiceCore::aftertouchChanged(int newAftertouchValue)
{
    if (mpeMember && ! isKeyDown())
        return;
    polyAftertouchValue =
        std::clamp(static_cast<float>(newAftertouchValue) / 127.0f, 0.0f, 1.0f);
}

void VoiceCore::channelPressureChanged(int newChannelPressureValue)
{
    if (mpeMember && ! isKeyDown())
        return;
    channelPressureValue =
        std::clamp(static_cast<float>(newChannelPressureValue) / 127.0f, 0.0f, 1.0f);
}

bool VoiceCore::isPlayingChannel(int midiChannel) const
{
    if (midiChannel <= 0)
        return isVoiceActive();

    if (isVoiceActive())
        return noteState.channel == std::clamp(midiChannel, 1, 16);

    return false;
}

void VoiceCore::startRealtimeNote(int midiChannel,
                              int newMidiNoteNumber,
                              float velocity,
                              int currentPitchWheelPosition,
                              uint64_t startOrder,
                              bool sustainPedalIsDown)
{
    if (isVoiceActive())
        stopNote(0.0f, false);

    noteState.channel = std::clamp(midiChannel, 1, 16);
    noteState.number = std::clamp(newMidiNoteNumber, 0, 127);
    noteState.startOrder = startOrder;
    noteState.active = true;
    cachedMidiChannel = noteState.channel;
    setKeyDown(true);
    setSustainPedalDown(sustainPedalIsDown);
    setSostenutoPedalDown(false);
    initialiseNote(noteState.number,
              std::clamp(velocity, 0.0f, 1.0f),
              currentPitchWheelPosition);
}

void VoiceCore::resumeNativeReleasedNote(float velocity, uint64_t startOrder) noexcept
{
    // Native same-key rearticulation during release retains the oscillator and
    // filter history. Allocating a fresh voice changes phase for the whole note.
    setKeyDown(true);
    nativeReleaseResumeGraceRemaining = 0;
    noteState.startOrder = startOrder;
    rawNoteVelocity = std::clamp(velocity, 0.0f, 1.0f);
    noteVelocity = shapeVelocity(rawNoteVelocity, activeVelocityCurve);
    releaseVelocity = 0.0f;
    velocitySmoother.setTargetValue(noteVelocity);
    polyAftertouchValue = 0.0f;
    ampEnvelope.noteOn();
    filterEnvelope.noteOn();
    modEnvelope.noteOn();
    for (size_t i = 0; i < noteLfos.size(); ++i)
        if (parameterSnapshot->modulation.lfos[i].triggerMode > 0) noteLfos[i].setPhase(0.0f);
    if (parameterSnapshot != nullptr)
    {
        // Serum's Mono Retrig restarts the sampled Noise source together with
        // the envelopes/note-triggered LFOs, even when the oscillator/filter
        // history is deliberately kept alive on the reused mono voice.
        const auto& noise = parameterSnapshot->noise;
        const double random = noise.randomPhase > 0.0f
            ? (renderNoiseSample() + 1.0) * 0.5 : 0.0;
        noiseSamplePlayer.reset(noise.phase + noise.randomPhase * random);
        nativeNoiseModulationValue = 0.0f;
        nativeNoiseModulationPhase = 0;
    }
}

void VoiceCore::stopRealtimeNote(float velocity, bool allowTailOff)
{
    setKeyDown(false);
    if (allowTailOff && parameterSnapshot != nullptr
        && parameterSnapshot->global.serumAmplitude
        && parameterSnapshot->global.voiceMode == parameters::VoiceMode::mono)
    {
        // Serum keeps a just-released mono oscillator alive for about one
        // millisecond even when Env1 Release is exactly 0 ms.  The branch is
        // silent, but a note that arrives inside that window resumes the old
        // Sub/A/B phase. Independent 48 kHz probes continue through a 48-sample
        // gap and reset after a 2400-sample gap.
        nativeReleaseResumeGraceRemaining = std::max(
            nativeReleaseResumeGraceRemaining,
            fad::engine::math::roundToNearestInt(static_cast<float>(sampleRate) * 0.001f) + 1);
    }
    stopNote(std::clamp(velocity, 0.0f, 1.0f), allowTailOff);
}

void VoiceCore::resetRealtimeState() noexcept
{
    noiseSamplePlayer.releaseAssetViews();
    for (auto& curve : nativeCurveRuntimes) curve.invalidate();
    for(auto& c:nodeWaves)c.reset();
    for(auto& c:nodeLfos)c.prepare(parameters::CurveNodes{});
    nodeFilterCurve={};nodeControlCounter=0;nodeRuntimesInitialised=false;
    noteState.active = false;
    noteState.number = -1;
    noteState.channel = 1;
    noteState.startOrder = 0;
    polyAftertouchValue = 0.0f;
    channelPressureValue = 0.0f;
    mpeMember = false;
    setKeyDown(false);
    setSustainPedalDown(false);
    setSostenutoPedalDown(false);
    clearCurrentNote();
}

bool VoiceCore::matchesRealtimeNote(int midiChannel, int noteNumber) const noexcept
{
    return isVoiceActive()
        && noteState.channel == std::clamp(midiChannel, 1, 16)
        && noteState.number == std::clamp(noteNumber, 0, 127);
}

uint64_t VoiceCore::classifyRenderRouteBits(const parameters::ModulationSnapshot& snapshot,
                                      bool preserveSmallAmounts) noexcept
{
    const auto summary = summarizeRenderRoutes(snapshot, preserveSmallAmounts);
    return summary.flags | (static_cast<uint64_t>(summary.rateMask) << 32);
}

void VoiceCore::renderNextBlock(fad::engine::AudioBufferView outputBuffer,
                            int startSample,
                            int numSamples)
{
    renderNextBlockWithContext(outputBuffer, startSample, numSamples, nullptr);
}

void VoiceCore::renderNextBlockWithContext(fad::engine::AudioBufferView outputBuffer,
                            int startSample, int numSamples, const RenderContext* context)
{
    if (! isVoiceActive() || parameterSnapshot == nullptr)
        return;

    if (context != nullptr && (context->source != parameterSnapshot
        || context->preserveSmallAmounts != parameterSnapshot->global.vitalAmplitude))
        context = nullptr;

    const auto routeSummary = context != nullptr && context->source == parameterSnapshot
        && context->preserveSmallAmounts == parameterSnapshot->global.vitalAmplitude
        ? RenderRouteSummary{static_cast<uint32_t>(context->routeBits), static_cast<uint32_t>(context->routeBits >> 32)}
        : summarizeRenderRoutes(parameterSnapshot->modulation, parameterSnapshot->global.vitalAmplitude);
    nativeRateRouteMask = routeSummary.rateMask;
    const auto newlyOwned=nativeRateRouteMask & ~ownedRateLfoMask;
    for(size_t i=0;i<noteLfos.size();++i) {
        const auto bit=uint32_t{1}<<i;
        if(globalLfos!=nullptr && (newlyOwned&bit)!=0 && parameterSnapshot->modulation.lfos[i].triggerMode==0)
            noteLfos[i]=(*globalLfos)[i];
        if(globalLfos!=nullptr && (nativeRateRouteMask&bit)==0 && (parameterSnapshot->modulation.lfos[i].triggerMode>0 || (ownedRateLfoMask&bit)!=0))
            noteLfos[i].setRate((*globalLfos)[i].getRateHz());
    }
    ownedRateLfoMask|=nativeRateRouteMask;
    const std::array<int, 2> crossModes{parameterSnapshot->oscillators[0].crossMode,
                                       parameterSnapshot->oscillators[1].crossMode};
    if (crossModes != renderedCrossModes || parameterSnapshot->global.serumAmplitude != renderedSerumAmplitude)
        beginStructuralFade();
    renderedCrossModes = crossModes;
    renderedSerumAmplitude = parameterSnapshot->global.serumAmplitude;
    // Capture before the loop so we know if a steal fade was in progress
    const bool wasBeingStolen = stealFadeRemaining > 0;
    // Native Mem retains oscillator history through the host processing block.
    // MIDI and LFO render subdivisions must not become retirement boundaries.
    const bool nativeMemoryBlockTail = usesNativePhaseMemory(*parameterSnapshot, 0)
        || usesNativePhaseMemory(*parameterSnapshot, 1);

    updateEnvelopeParameters();
    const bool hasNativeSustainRoutes = routeSummary.has(RenderRouteSummary::ampSustain);
    ampEnvelope.setSustainModulationEnabled(hasNativeSustainRoutes);
    const bool hasNativeDecayRoutes = routeSummary.has(RenderRouteSummary::ampDecay);
    ampEnvelope.setDecayModulationEnabled(hasNativeDecayRoutes);
    const bool hasNativeReleaseRoutes = routeSummary.has(RenderRouteSummary::ampRelease);
    ampEnvelope.setReleaseModulationEnabled(hasNativeReleaseRoutes);
    syncPerformanceStateFromChannel();
    refreshRenderedUnisonVoiceCounts();
    const bool skipFilterEnvelope = filterEnvelope.canSkipSilentSamples();
    const bool skipModEnvelope = modEnvelope.canSkipSilentSamples();

    // Pre-compute loop-invariant values
    const auto outputChannels = outputBuffer.channels();
    const bool hasOutput = outputChannels > 0;
    const bool isMono = outputChannels == 1;
    float* outL = hasOutput ? outputBuffer.channel(0) : nullptr;
    float* outR = outputChannels > 1 ? outputBuffer.channel(1) : nullptr;

    const auto subWave = parameterSnapshot->subNativeShape >= 0
        ? (parameterSnapshot->subNativeShape == 0
               ? parameters::AnalogWave::sine
               : (parameterSnapshot->subNativeShape == 2
                      ? parameters::AnalogWave::triangle
                      : (parameterSnapshot->subNativeShape == 3
                             ? parameters::AnalogWave::saw
                             : parameters::AnalogWave::square)))
        : (parameterSnapshot->subType == parameters::SubType::sine
               ? parameters::AnalogWave::sine
               : (parameterSnapshot->subType == parameters::SubType::triangle
                      ? parameters::AnalogWave::triangle
                      : parameters::AnalogWave::square));

    const wavetable::WavetableData* oscWavetablePtrs[2] = { nullptr, nullptr };
    for (size_t oi = 0; oi < oscillators.size(); ++oi)
    {
        if (blockWavetablePointers != nullptr)
            oscWavetablePtrs[oi] = (*blockWavetablePointers)[oi];
        const auto* table = oscWavetablePtrs[oi];
        const bool exactNoOp = wavetablePointersApplied
            && appliedWavetableGeneration == blockWavetableGeneration
            && appliedWavetablePointers[oi] == table
            && (table == nullptr || table->hasRenderableLayout());
        if (! exactNoOp)
        {
            oscillators[oi].setExternalWavetable(table, blockWavetableGeneration);
            for (auto& oscillator : unisonOscillators[oi])
                oscillator.setExternalWavetable(table, blockWavetableGeneration);
        }
        appliedWavetablePointers[oi] = table;
    }

    appliedWavetableGeneration = blockWavetableGeneration;
    wavetablePointersApplied = true;
    const bool hasNodeCurves=std::any_of(parameterSnapshot->curves.begin(),parameterSnapshot->curves.end(),[](const auto& c){return c.enabled;});
    if(!nodeRuntimesInitialised){for(size_t i=0;i<parameters::lfoCount;++i)nodeLfos[i].prepare(parameterSnapshot->curves[i+2]);nodeRuntimesInitialised=true;}
    const auto safeSubOctave = std::clamp(parameterSnapshot->subOctave, -4, 4);
    const float subOctaveRatio = std::exp2(static_cast<float>(safeSubOctave));
    updateContinuousParameterTargetsFromSnapshot(context);
    const bool continuousBaseSmoothingActive = continuousParameterSmoothingActive();
    parameters::OscillatorSnapshot subSnapshot;
    subSnapshot.mode = parameters::OscillatorMode::analog;
    subSnapshot.analogWave = subWave;
    subSnapshot.analogPulseWidth = 0.5f;
    const bool useNativeSub = parameterSnapshot->global.serumAmplitude && parameterSnapshot->subNativeShape >= 0;
    subOscillator.setExternalWavetable(useNativeSub ? nativeSubTable : nullptr, 0);
    if (useNativeSub)
    {
        subSnapshot.mode = parameters::OscillatorMode::wavetable;
        subSnapshot.wavetablePosition = static_cast<float>(std::clamp(parameterSnapshot->subNativeShape, 0, 5)) / 5.0f;
    }
    const auto stereoOffset = fadmodel1::dsp::utils::sanitizeFiniteClamped(
        parameterSnapshot->filter.stereoOffsetSemitones, 0.0f, -32.0f, 32.0f);
    const auto rightFilterRatio = stereoOffset == 0.0f ? 1.0f : std::exp2(stereoOffset / 12.0f);
    const auto leftFilterRatio = 1.0f / rightFilterRatio;
    const auto& noiseConfig = parameterSnapshot->noise;
    const bool hasNativeBaseTuning=std::abs(parameterSnapshot->global.nativeTuneSemitones)>1.e-6f
        ||std::abs(parameterSnapshot->oscillators[0].nativeCoarseSemitones)>1.e-6f||std::abs(parameterSnapshot->oscillators[1].nativeCoarseSemitones)>1.e-6f;
    const bool hasNativeMasterTuning=std::abs(parameterSnapshot->global.nativeTuneSemitones)>1.e-6f
        ||std::abs(smoothedModValues.nativeMasterPitchSemitones)>1.e-6f
        ||routeSummary.has(RenderRouteSummary::nativeMasterPitch);
    const double baseNoiseRate = context != nullptr && !noiseConfig.keytrack
        ? context->untrackedNoiseRate : NoiseSamplePlayer::playbackRate(
            noiseConfig.pitch, noiseConfig.fine, noiseConfig.keytrack, pitchNoteNumber);
    noiseSamplePlayer.configure(blockNoiseSample, sampleRate, baseNoiseRate, noiseConfig.oneShot, blockNoiseOwnership, noiseConfig.sampleSlot == 0);
    const bool hasNativeNoisePitch = std::abs(smoothedModValues.nativeNoisePitchOffset) > 1.e-6f
        || routeSummary.has(RenderRouteSummary::nativeNoisePitch);
    const auto updateNoisePitch = [&](const modulation::DestinationValues& values) noexcept {
        double multiplier = noiseConfig.keytrack && hasNativeMasterTuning
            ? static_cast<double>(fastExp2(values.nativeMasterPitchSemitones / 12.0f)) : 1.0;
        if (hasNativeNoisePitch)
            multiplier *= NoiseSamplePlayer::playbackRate(noiseConfig.pitch, noiseConfig.fine,
                noiseConfig.keytrack, pitchNoteNumber, values.nativeNoisePitchOffset) / baseNoiseRate;
        noiseSamplePlayer.setPitchMultiplier(multiplier);
    };
    const auto noisePanGains = context != nullptr ? context->noisePanGains
        : applyPan(1.0f, noiseConfig.pan * 2.0f - 1.0f, parameterSnapshot->global.serumAmplitude);
    const float inverseSampleRate = 1.0f / static_cast<float>(sampleRate);
    if (appliedNativeSyncWarp != parameterSnapshot->global.serumAmplitude)
    {
        for (size_t i = 0; i < oscillators.size(); ++i)
        {
            oscillators[i].setNativeSyncWarp(parameterSnapshot->global.serumAmplitude);
            for (auto& oscillator : unisonOscillators[i])
                oscillator.setNativeSyncWarp(parameterSnapshot->global.serumAmplitude);
        }
        appliedNativeSyncWarp = parameterSnapshot->global.serumAmplitude;
    }
    auto baseOscillatorSnapshots = parameterSnapshot->oscillators;
    if (! continuousBaseSmoothingActive)
    {
        for (size_t oscillatorIndex = 0; oscillatorIndex < baseOscillatorSnapshots.size(); ++oscillatorIndex)
            applyContinuousOscillatorParameters(oscillatorIndex,
                                                baseOscillatorSnapshots[oscillatorIndex]);
    }
    std::array<parameters::OscillatorSnapshot, 2> renderedOscillatorSnapshots{};
    for (size_t i = 0; i < renderedOscillatorSnapshots.size(); ++i)
        renderedOscillatorSnapshots[i] = baseOscillatorSnapshots[i];

   #if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    const bool allowStablePitchFastPath = unmodulatedPitchFastPathEnabledForTest;
   #else
    constexpr bool allowStablePitchFastPath = true;
   #endif
    const bool managerPitchIsSmoothing = mpeManagerPitchSmoother.isSmoothing();
    const bool staticManagerPitch = allowStablePitchFastPath && ! managerPitchIsSmoothing;
    const auto managerPitchTarget = mpeManagerPitchSmoother.getTargetValue();
    const auto managerPitchBase = managerPitchIsSmoothing
        ? 0.0f : mpeManagerPitchSmoother.getCurrentValue();
    // Serum's Pitch Tracking Off fixes the oscillator at MIDI note zero while
    // retaining its octave, coarse/fine tuning, bend and modulation.
    std::array<float, 2> oscillatorBaseTuningRatios{};
    for (size_t oscillatorIndex = 0;
         oscillatorIndex < oscillatorBaseTuningRatios.size();
         ++oscillatorIndex)
    {
        const auto& oscillatorSnapshot = baseOscillatorSnapshots[oscillatorIndex];
        const auto semitones = static_cast<float>(
            oscillatorSnapshot.octave * 12 + oscillatorSnapshot.semitone)
            + pitchBendSemitones + midi2NoteBend + midi2TuningOffset + managerPitchBase;
        const auto fine=parameterSnapshot->global.serumAmplitude ? std::floor(oscillatorSnapshot.fineCents+100.5f)-100.0f : oscillatorSnapshot.fineCents;
        oscillatorBaseTuningRatios[oscillatorIndex] = context != nullptr
                && pitchBendSemitones == 0.0f && midi2NoteBend == 0.0f && midi2TuningOffset == 0.0f && managerPitchBase == 0.0f
            ? context->zeroBendTuningRatios[oscillatorIndex]
            : fastExp2(semitones/12.0f+fine/1200.0f);
    }

    if (! modulationResidualMaskValid)
    {
        const auto computed = summarizeResiduals(smoothedModValues);
        modulationResidualMask = static_cast<uint8_t>(static_cast<unsigned>(computed.any)
            | (static_cast<unsigned>(computed.pitch) << 1)
            | (static_cast<unsigned>(computed.snapshot) << 2));
        modulationResidualMaskValid = true;
    }
    const ResidualSummary residualSummary{
        (modulationResidualMask & 1u) != 0,
        (modulationResidualMask & 2u) != 0,
        (modulationResidualMask & 4u) != 0};
    const bool staticPitchForBlock = !hasNativeBaseTuning && ! baseFrequencySmoother.isSmoothing()
        && ! mpeManagerPitchSmoother.isSmoothing()
        && !(usesNativePitchRamp() && nativePitchRamp.isSmoothing())
        && ! routeSummary.has(RenderRouteSummary::oscillatorPitch)
        && ! residualSummary.pitch
        && std::all_of(baseOscillatorSnapshots.begin(),
                       baseOscillatorSnapshots.end(),
                       [](const auto& oscillatorSnapshot) noexcept
                       {
                           return utils::isFinite(oscillatorSnapshot.drift)
                               && std::abs(oscillatorSnapshot.drift) < 0.0001f;
                       });
    const auto staticBaseFrequencyHz = staticPitchForBlock
        ? fadmodel1::dsp::utils::sanitizeFiniteClamped(
              usesNativePitchRamp() ? nativePitchRamp.getCurrentValue() : baseFrequencySmoother.getCurrentValue(), 440.0f, 1.0f, 24000.0f)
        : 0.0f;
    std::array<float, 2> staticTunedCentreFrequencies{};

    for (size_t oscillatorIndex = 0; oscillatorIndex < unisonRenderCaches.size(); ++oscillatorIndex)
    {
        const auto unisonCount = renderedUnisonVoiceCounts[oscillatorIndex];
        if (unisonCount <= 1)
            continue;

        const auto& oscillatorSnapshot = baseOscillatorSnapshots[oscillatorIndex];
        const auto detuneRange = fadmodel1::dsp::utils::sanitizeFinite(
            oscillatorSnapshot.detuneCents, 0.0f);
        const auto width = fadmodel1::dsp::utils::sanitizeFiniteClamped(
            oscillatorSnapshot.unisonWidth, 1.0f, 0.0f, 1.0f);
        const auto blend = fadmodel1::dsp::utils::sanitizeFiniteClamped(
            oscillatorSnapshot.unisonBlend, 0.5f, 0.0f, 1.0f);
        auto& cache = unisonRenderCaches[oscillatorIndex];
        const bool planMatches = cache.planValid
            && cache.serumAmplitude == parameterSnapshot->global.serumAmplitude
            && cache.voiceCount == unisonCount
            && sameFloatBits(cache.detuneRange, detuneRange)
            && sameFloatBits(cache.width, width)
            && sameFloatBits(cache.blend, blend)
            && cache.randomDetuneCents == unisonRandomDetuneCents[oscillatorIndex];
        if (planMatches)
            continue;

        cache.planValid = true;
        cache.serumAmplitude = parameterSnapshot->global.serumAmplitude;
        cache.staticPanValid = false;
        cache.voiceCount = unisonCount;
        cache.detuneRange = detuneRange;
        cache.width = width;
        cache.blend = blend;
        cache.randomDetuneCents = unisonRandomDetuneCents[oscillatorIndex];
        const auto inverseCount = 1.0f / std::sqrt(static_cast<float>(unisonCount));
        auto& plan = cache.plan;

        for (int unisonIndex = 0; unisonIndex < unisonCount; ++unisonIndex)
        {
            const auto t = static_cast<float>(unisonIndex)
                / static_cast<float>(unisonCount - 1);
            const auto edgeness = std::abs(t - 0.5f) * 2.0f;
            const auto blendGain = std::clamp(fad::engine::math::interpolate(blend, 1.0f - edgeness, edgeness + 0.5f), 0.0f, 2.0f);
            const auto randomDetune = fadmodel1::dsp::utils::sanitizeFinite(
                unisonRandomDetuneCents[oscillatorIndex][static_cast<size_t>(unisonIndex)],
                0.0f);
            const auto totalDetuneCents = (t - 0.5f) * detuneRange + randomDetune;
            const auto detuneOctaves = std::clamp(totalDetuneCents / 1200.0f, -12.0f, 12.0f);

            plan.frequencyRatios[static_cast<size_t>(unisonIndex)] =
                std::exp2(detuneOctaves);
            plan.mipLevelOffsets[static_cast<size_t>(unisonIndex)] = detuneOctaves;
            plan.panOffsets[static_cast<size_t>(unisonIndex)] =
                (t - 0.5f) * 2.0f * width;
            plan.gainScales[static_cast<size_t>(unisonIndex)] =
                std::clamp(blendGain, 0.0f, 1.0f) * inverseCount;
            if (parameterSnapshot->global.serumAmplitude)
            {
                const auto stereo = oscillators::serumUnisonStereoGains(unisonIndex, unisonCount, width);
                plan.nativePanLeft[static_cast<size_t>(unisonIndex)] = stereo[0];
                plan.nativePanRight[static_cast<size_t>(unisonIndex)] = stereo[1];
                plan.gainScales[static_cast<size_t>(unisonIndex)] =
                    oscillators::serumUnisonBlendGain(unisonIndex, unisonCount, blend);
            }
        }
    }

    if (staticPitchForBlock)
    {
        for (size_t oscillatorIndex = 0; oscillatorIndex < oscillators.size(); ++oscillatorIndex)
        {
            const auto& oscillatorSnapshot = baseOscillatorSnapshots[oscillatorIndex];
            if (! oscillatorSnapshot.enabled)
                continue;

            const auto tunedCentreFrequency = (oscillatorSnapshot.pitchTracking
                ? staticBaseFrequencyHz : untrackedOscillatorBaseHz)
                * oscillatorBaseTuningRatios[oscillatorIndex];
            staticTunedCentreFrequencies[oscillatorIndex] = tunedCentreFrequency;
            const auto unisonCount = renderedUnisonVoiceCounts[oscillatorIndex];
            const auto& unisonPlan = unisonRenderCaches[oscillatorIndex].plan;
            const auto frameSize = oscWavetablePtrs[oscillatorIndex] != nullptr
                ? oscWavetablePtrs[oscillatorIndex]->frameSize
                : wavetable::WavetableData::defaultFrameSize;
            const auto centreMipLevel = oscillatorSnapshot.mode == parameters::OscillatorMode::wavetable
                ? wavetable::WavetableData::calculateIdealMipLevel(
                      frameSize,
                      tunedCentreFrequency / static_cast<float>(sampleRate))
                : 0.0f;

            for (int unisonIndex = 0; unisonIndex < unisonCount; ++unisonIndex)
            {
                const auto planIndex = static_cast<size_t>(unisonIndex);
                auto& oscillator = unisonIndex == 0
                    ? oscillators[oscillatorIndex]
                    : unisonOscillators[oscillatorIndex][static_cast<size_t>(unisonIndex - 1)];
                oscillator.setFrequencyWithoutPhaseJitter(
                    tunedCentreFrequency
                    * (unisonCount <= 1 ? 1.0f : unisonPlan.frequencyRatios[planIndex]));
                if (oscillatorSnapshot.mode == parameters::OscillatorMode::wavetable)
                {
                    oscillator.setWavetableMipLevel(
                        centreMipLevel
                        + (unisonCount <= 1 ? 0.0f : unisonPlan.mipLevelOffsets[planIndex]));
                }
            }
        }
    }

    const auto baseSustainPosition=std::sqrt(fadmodel1::dsp::utils::sanitizeUnitInterval(parameterSnapshot->ampEnvelope.sustainLevel));
    const auto baseDecayPosition=hasNativeDecayRoutes
        ? modulation::HoldEnvelope::serumDecayPosition(parameterSnapshot->ampEnvelope.decaySeconds) : 0.0f;
    const auto baseReleasePosition=hasNativeReleaseRoutes
        ? modulation::HoldEnvelope::serumReleasePosition(parameterSnapshot->ampEnvelope.releaseSeconds) : 0.0f;
    // T4 fix: cache atomic load outside the inner loop to avoid per-sample atomic reads
    const bool diagEnabled = diagnosticsEnabled.load(std::memory_order_relaxed);
    const bool hasNativeSources=routeSummary.has(RenderRouteSummary::nativeSources)
        || (blockLfoCurveBanks!=nullptr && std::any_of(blockLfoCurveBanks->begin(),blockLfoCurveBanks->end(),[](const auto* bank){return bank!=nullptr&&!bank->nativeCurves.empty();}));
    const bool hasNativeNoiseSource=routeSummary.has(RenderRouteSummary::nativeNoiseSource);
    std::array<bool,2> hasNativeWidthRoutes{
        std::abs(smoothedModValues.nativeUnisonWidthOffsets[0])>=.000001f || routeSummary.has(RenderRouteSummary::nativeWidth0),
        std::abs(smoothedModValues.nativeUnisonWidthOffsets[1])>=.000001f || routeSummary.has(RenderRouteSummary::nativeWidth1)};
    const bool hasActiveModulationRoutes=routeSummary.has(RenderRouteSummary::activeModulation);
    bool modulationSmoothingActive =
        hasActiveModulationRoutes || hasNativeBaseTuning || residualSummary.any;
    const bool oscillatorSnapshotsStaticForBlock =
        ! continuousBaseSmoothingActive
        && ! routeSummary.has(RenderRouteSummary::oscillatorSnapshot)
        && ! residualSummary.snapshot;
    int staticEnabledOscillatorCount = 0;
    float staticOscillatorMixScale = 1.0f;

    if (oscillatorSnapshotsStaticForBlock)
    {
        for (size_t oscillatorIndex = 0;
             oscillatorIndex < renderedOscillatorSnapshots.size();
             ++oscillatorIndex)
        {
            auto& renderedSnapshot = renderedOscillatorSnapshots[oscillatorIndex];
            updateRenderedOscillatorSnapshot(oscillatorIndex,
                                             smoothedModValues,
                                             baseOscillatorSnapshots[oscillatorIndex],
                                             renderedSnapshot);
            if (renderedSnapshot.enabled && renderedSnapshot.level >= 0.0001f)
                ++staticEnabledOscillatorCount;
        }

        staticOscillatorMixScale = parameterSnapshot->global.serumAmplitude ? 2.0f : parameterSnapshot->global.vitalAmplitude ? 1.0f : staticEnabledOscillatorCount > 1
            ? 1.0f / std::sqrt(static_cast<float>(staticEnabledOscillatorCount))
            : 1.0f;
        continuousOscillatorMixScale = staticOscillatorMixScale;
        continuousOscillatorMixScaleTarget = staticOscillatorMixScale;

        for (size_t oscillatorIndex = 0;
             oscillatorIndex < renderedOscillatorSnapshots.size();
             ++oscillatorIndex)
        {
            const auto& oscillatorSnapshot = renderedOscillatorSnapshots[oscillatorIndex];
            if (! oscillatorSnapshot.enabled || oscillatorSnapshot.level < 0.0001f)
                continue;

            const auto unisonCount = renderedUnisonVoiceCounts[oscillatorIndex];
            auto& cache = unisonRenderCaches[oscillatorIndex];
            const bool panMatches = cache.staticPanValid
                && cache.staticSerumAmplitude == parameterSnapshot->global.serumAmplitude
                && cache.staticVitalAmplitude == parameterSnapshot->global.vitalAmplitude
                && cache.staticPanVoiceCount == unisonCount
                && sameFloatBits(cache.staticLevel, oscillatorSnapshot.level)
                && sameFloatBits(cache.staticPan, oscillatorSnapshot.pan)
                && sameFloatBits(cache.staticMixScale, staticOscillatorMixScale);
            if (panMatches)
                continue;

            cache.staticPanValid = true;
            cache.staticSerumAmplitude = parameterSnapshot->global.serumAmplitude;
            cache.staticVitalAmplitude = parameterSnapshot->global.vitalAmplitude;
            cache.staticPanVoiceCount = unisonCount;
            cache.staticLevel = oscillatorSnapshot.level;
            cache.staticPan = oscillatorSnapshot.pan;
            cache.staticMixScale = staticOscillatorMixScale;
            const auto& plan = cache.plan;
            for (int unisonIndex = 0; unisonIndex < unisonCount; ++unisonIndex)
            {
                const auto planIndex = static_cast<size_t>(unisonIndex);
                const auto gain = oscillatorSnapshot.level * staticOscillatorMixScale
                    * (unisonCount <= 1 ? 1.0f : plan.gainScales[planIndex]);
                const auto pan = oscillatorSnapshot.pan
                    + (unisonCount <= 1 ? 0.0f : plan.panOffsets[planIndex]);
                auto panGains = applyPan(gain, pan, parameterSnapshot->global.serumAmplitude, parameterSnapshot->global.vitalAmplitude);
                if (unisonCount > 1 && parameterSnapshot->global.serumAmplitude)
                {
                    panGains = applyPan(gain, oscillatorSnapshot.pan, true);
                    panGains[0] *= plan.nativePanLeft[planIndex];
                    panGains[1] *= plan.nativePanRight[planIndex];
                }
                cache.staticPanGainLeft[planIndex] = panGains[0];
                cache.staticPanGainRight[planIndex] = panGains[1];
            }
        }
    }

    // A route-free block has the same raw tuning on every sample. Keep the
    // destination buffers outside the loop; only matrix/smoothing work writes
    // them. This also avoids copying and adding zero audio-rate lanes.
    modulation::DestinationValues baseRawModValues{};
    resolveNativeTuning(baseRawModValues);
    modulation::DestinationValues renderedRawModValues{};
    modulation::DestinationValues combinedModulationValues{};
    // When every pitch destination is exactly zero and cannot change during
    // this block, the existing sum reduces to drift. Keep drift, smoothing and
    // all oscillator state on their normal sample timeline.
    const auto pitchDestinationsAreZero = [&](const size_t i) noexcept
    {
        return smoothedModValues.oscillatorPitchSemitones[i] == 0.0f
            && smoothedModValues.nativeCoarsePitchSemitones[i] == 0.0f
            && smoothedModValues.nativeSemiPitchSemitones[i] == 0.0f
            && smoothedModValues.nativeFinePitchCents[i] == 0.0f
            && smoothedModValues.nativeOctaveOffsets[i] == 0.0f
            && baseRawModValues.nativeCoarsePitchSemitones[i] == 0.0f;
    };
    const bool unmodulatedPitchForBlock = allowStablePitchFastPath
        && ! parameterSnapshot->global.serumAmplitude
        && ! parameterSnapshot->global.vitalAmplitude
        && ! modulationSmoothingActive && ! diagEnabled && nativeRateRouteMask == 0
        && ! managerPitchIsSmoothing
        && smoothedModValues.globalPitchSemitones == 0.0f
        && smoothedModValues.nativeMasterPitchSemitones == 0.0f
        && baseRawModValues.nativeMasterPitchSemitones == 0.0f
        && pitchDestinationsAreZero(0) && pitchDestinationsAreZero(1);
   #if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    lastUnmodulatedPitchFastPathUsedForTest = unmodulatedPitchForBlock;
   #endif
    // Oscillator drift does not tune the Sub. Its base frequency can remain
    // fixed even when A/B need their per-sample pitch path.
    const bool staticSubPitchForBlock = !baseFrequencySmoother.isSmoothing()
        && !mpeManagerPitchSmoother.isSmoothing()
        && !(usesNativePitchRamp() && nativePitchRamp.isSmoothing())
        && !modulationSmoothingActive;
    const auto staticSubBaseFrequency = staticSubPitchForBlock
        ? fadmodel1::dsp::utils::sanitizeFiniteClamped(usesNativePitchRamp()
            ? nativePitchRamp.getCurrentValue() : baseFrequencySmoother.getCurrentValue(),
            440.0f, 1.0f, 24000.0f) : 0.0f;
    const auto staticSubFrequency = staticSubPitchForBlock
        ? staticSubBaseFrequency * subOctaveRatio * fastExp2((pitchBendSemitones + midi2NoteBend
            + midi2TuningOffset + managerPitchBase + smoothedModValues.globalPitchSemitones
            + smoothedModValues.nativeMasterPitchSemitones) / 12.0f) : 0.0f;
    const auto staticSubPanGains = applyPan(1.0f, continuousSubPan, useNativeSub);
    const bool staticVoiceGainForBlock = !velocitySmoother.isSmoothing()
        && !velocitySensitivitySmoother.isSmoothing() && !channelVolumeSmoother.isSmoothing()
        && !expressionSmoother.isSmoothing() && !mpeManagerVolumeSmoother.isSmoothing()
        && !mpeManagerExpressionSmoother.isSmoothing();
    const auto staticVelocity = fadmodel1::dsp::utils::sanitizeFiniteClamped(
        velocitySmoother.getCurrentValue(), 0.0f, 0.0f, 1.0f);
    const auto staticVelocitySensitivity = velocitySensitivitySmoother.getCurrentValue();
    const auto staticVelocityGain = parameterSnapshot->global.serumAmplitude ? 1.0f
        : (staticVelocitySensitivity == 1.0f ? staticVelocity
            : 1.0f + staticVelocitySensitivity * (staticVelocity - 1.0f));
    const auto staticVoiceGain = staticVelocityGain * channelVolumeSmoother.getCurrentValue()
        * expressionSmoother.getCurrentValue() * mpeManagerVolumeSmoother.getCurrentValue()
        * mpeManagerExpressionSmoother.getCurrentValue();
    const bool staticFilterRoutingForBlock = !filterWetGain.isSmoothing()
        && !filterMixBaseGain.isSmoothing() && !filterEnabledGain.isSmoothing()
        && std::none_of(filterInputGains.begin(), filterInputGains.end(),
            [](const auto& gain) noexcept { return gain.isSmoothing(); });
    std::array<float, 4> staticFilterInputGains{};
    for (size_t i = 0; i < staticFilterInputGains.size(); ++i)
        staticFilterInputGains[i] = filterInputGains[i].getCurrentValue();

    std::array<oscillators::OscillatorBlock::StaticUnisonRenderSpan, 2> unisonSpans{
        oscillators::OscillatorBlock::StaticUnisonRenderSpan{oscillators[0], unisonOscillators[0].data(),
            renderedUnisonVoiceCounts[0], renderedOscillatorSnapshots[0]},
        oscillators::OscillatorBlock::StaticUnisonRenderSpan{oscillators[1], unisonOscillators[1].data(),
            renderedUnisonVoiceCounts[1], renderedOscillatorSnapshots[1]}};

    // Cross-modulation previews read the other oscillator before each sample;
    // keep those oscillators' state immediately visible to the normal path.
    bool useUnisonSpans = !hasNodeCurves
        && std::none_of(renderedOscillatorSnapshots.begin(), renderedOscillatorSnapshots.end(),
            [](const auto& oscillator) noexcept
            {
                return oscillator.enabled && oscillator.mode == parameters::OscillatorMode::wavetable
                    && oscillator.crossMode > 0;
            });
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    useUnisonSpans = useUnisonSpans && staticUnisonSpanEnabledForTest;
#endif

    // Select the route-free control kernel once for this render span. Both
    // kernels retain the same sample loop and state/stop boundaries; the steady
    // instantiation removes checks for destinations that cannot change here.
    const bool steadyControlSpan = unmodulatedPitchForBlock && staticManagerPitch
        && staticVoiceGainForBlock && staticFilterRoutingForBlock && staticSubPitchForBlock
        && oscillatorSnapshotsStaticForBlock && !continuousBaseSmoothingActive
        && !modulationSmoothingActive && !hasActiveModulationRoutes && !diagEnabled
        && nativeRateRouteMask == 0 && !hasNativeSources && !hasNativeNoiseSource
        && !hasNativeNoisePitch && !hasNativeMasterTuning && !hasNativeSustainRoutes
        && !hasNativeDecayRoutes && !hasNativeReleaseRoutes && !hasNodeCurves
        && !hasNativeWidthRoutes[0] && !hasNativeWidthRoutes[1];
    const auto renderSamples = [&]<bool steadyControls>() -> bool
    {
        const bool continuousSmoothing = ! steadyControls && continuousBaseSmoothingActive;
        const bool managerPitchFixed = steadyControls || staticManagerPitch;
        const bool voiceGainFixed = steadyControls || staticVoiceGainForBlock;
        const bool nativeNoiseSource = ! steadyControls && hasNativeNoiseSource;
        const bool nativeNoisePitch = ! steadyControls && hasNativeNoisePitch;
        const bool nativeMasterTuning = ! steadyControls && hasNativeMasterTuning;
        const bool activeRoutes = ! steadyControls && hasActiveModulationRoutes;
        const bool nativeSources = ! steadyControls && hasNativeSources;
        const bool nativeSustain = ! steadyControls && hasNativeSustainRoutes;
        const bool nativeDecay = ! steadyControls && hasNativeDecayRoutes;
        const bool nativeRelease = ! steadyControls && hasNativeReleaseRoutes;
        bool smoothingModulation = ! steadyControls && modulationSmoothingActive;
        const bool snapshotsFixed = steadyControls || oscillatorSnapshotsStaticForBlock;
        const bool filterRoutingFixed = steadyControls || staticFilterRoutingForBlock;
        const bool subPitchFixed = steadyControls || staticSubPitchForBlock;
        const bool pitchUnmodulated = steadyControls || unmodulatedPitchForBlock;
        const bool nodeCurves = ! steadyControls && hasNodeCurves;
    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto managerPitchSemitones = managerPitchFixed ? managerPitchTarget
            : mpeManagerPitchSmoother.getNextValue();
        const auto managerPitchDelta = managerPitchSemitones - managerPitchBase;
        if (continuousSmoothing)
        {
            advanceContinuousParameterState();
            for (size_t oscillatorIndex = 0; oscillatorIndex < baseOscillatorSnapshots.size(); ++oscillatorIndex)
                applyContinuousOscillatorParameters(oscillatorIndex,
                                                    baseOscillatorSnapshots[oscillatorIndex]);
        }

        const auto currentBaseFrequencyHz = staticPitchForBlock
            ? staticBaseFrequencyHz
            : fadmodel1::dsp::utils::sanitizeFiniteClamped(
                  usesNativePitchRamp() ? nativePitchRamp.getNextValue() : baseFrequencySmoother.getNextValue(), 440.0f, 1.0f, 24000.0f);
        const auto currentVelocity = voiceGainFixed ? staticVelocity
            : fadmodel1::dsp::utils::sanitizeFiniteClamped(velocitySmoother.getNextValue(), 0.0f, 0.0f, 1.0f);
        // HoldEnvelope already returns a finite value in [0, 1], including
        // instantaneous/held stages. Keep the protection at that boundary.
        const auto ampEnvelopeValue = ampEnvelope.getNextSample();
        const auto filterEnvelopeValue = skipFilterEnvelope ? 0.0f : filterEnvelope.getNextSample();
        const auto modEnvelopeValue = skipModEnvelope ? 0.0f : modEnvelope.getNextSample();
        float modulationNoiseSample = 0.0f;
        std::array<float, 2> audibleNoiseSample {};
        if (nativeNoiseSource && continuousNoiseEnabled > 0.0f)
        {
            // Consume once, before the matrix; reuse the identical sample in the
            // audible Noise path. A master-tune feedback route uses prior state.
            if (nativeNoisePitch || (noiseConfig.keytrack && nativeMasterTuning))
                updateNoisePitch(smoothedModValues);
            std::array<float, 2> builtin{};
            if (noiseSamplePlayer.needsBuiltinSample()) builtin.fill(renderNoiseSample());
            audibleNoiseSample = noiseSamplePlayer.nextStereoSample(builtin);
            modulationNoiseSample = audibleNoiseSample[0];
        }
        if (nativeNoiseSource)
        {
            // Native 48 kHz probes resolve one held matrix sample per four
            // audio frames; the audible PCM still advances every sample.
            if (nativeNoiseModulationPhase == 0)
                nativeNoiseModulationValue = continuousNoiseEnabled > 0.0f
                    ? (.5f + .5f * modulationNoiseSample) * continuousNoiseEnabled : 0.0f;
            nativeNoiseModulationPhase = (nativeNoiseModulationPhase + 1) & 3;
        }
        const bool renderMatrix = ! steadyControls && (activeRoutes || nativeRateRouteMask!=0
            || (diagEnabled && sampleIndex == 0));
        if (renderMatrix)
        {
            auto sourceValues=makeModulationSources(ampEnvelopeValue,filterEnvelopeValue,modEnvelopeValue,sampleIndex,nativeSources);
            sourceValues.nativeNoiseOsc = nativeNoiseModulationValue;
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
            lastRenderedSourceValuesForTest = sourceValues;
#endif
            for(size_t lfo=0;lfo<noteLfos.size();++lfo)
                if((nativeRateRouteMask&(uint32_t{1}<<lfo))!=0)
                    noteLfos[lfo].setRate(modulation::modulatedNativeLfoRate(parameterSnapshot->modulation,lfo,sourceValues,transportTempoBpm));
            renderedRawModValues =
                (diagEnabled && sampleIndex == 0)
                    ? modulation::ModMatrix::renderWithTrace(parameterSnapshot->modulation,
                                                              sourceValues,
                                                              lastModulationTrace, parameterSnapshot->global.vitalAmplitude)
                    : modulation::ModMatrix::render(parameterSnapshot->modulation, sourceValues, parameterSnapshot->global.vitalAmplitude);
            resolveNativeTuning(renderedRawModValues);
        }
        const auto& rawModValues = renderMatrix ? renderedRawModValues : baseRawModValues;
        // Evaluate sustain once per sample after the source envelopes. This
        // retains one-sample feedback for a self-routed envelope.
        if (nativeSustain)
        {
            const auto sustainPosition=std::clamp(baseSustainPosition+rawModValues.ampSustainOffset, 0.0f, 1.0f);
            ampEnvelope.setModulatedSustain(sustainPosition*sustainPosition);
        }
        if (nativeDecay)
            ampEnvelope.setModulatedSerumDecay(baseDecayPosition, rawModValues.ampDecayOffset);
        if (nativeRelease)
            ampEnvelope.setModulatedSerumRelease(baseReleasePosition, rawModValues.ampReleaseOffset);

        if (smoothingModulation)
        {
            modulationResidualMaskValid = false;
            auto smoothDestination = [this](float& smoothed, float raw) noexcept
            {
                smoothed = fadmodel1::dsp::utils::sanitizeFinite(smoothed, 0.0f);
                raw = fadmodel1::dsp::utils::sanitizeFinite(raw, 0.0f);
                smoothed += modSmoothCoeff * (raw - smoothed);
            };

            smoothDestination(smoothedModValues.nativeMasterPitchSemitones,rawModValues.nativeMasterPitchSemitones);
            smoothDestination(smoothedModValues.nativeFilterCutoffOffset,rawModValues.nativeFilterCutoffOffset);
            smoothDestination(smoothedModValues.nativeFilterResonanceOffset,rawModValues.nativeFilterResonanceOffset);
            smoothDestination(smoothedModValues.nativeFilterDriveOffset,rawModValues.nativeFilterDriveOffset);
            smoothDestination(smoothedModValues.nativeFilterVariationOffset,rawModValues.nativeFilterVariationOffset);
            smoothDestination(smoothedModValues.nativeFilterMixOffset,rawModValues.nativeFilterMixOffset);
            smoothDestination(smoothedModValues.filterCutoffOctaves, rawModValues.filterCutoffOctaves);
            smoothDestination(smoothedModValues.filterResonanceOffset, rawModValues.filterResonanceOffset);
            smoothDestination(smoothedModValues.filterDriveDb, rawModValues.filterDriveDb);
            smoothDestination(smoothedModValues.globalPitchSemitones,
                              rawModValues.globalPitchSemitones);
            smoothDestination(smoothedModValues.subLevelOffset, rawModValues.subLevelOffset);
            smoothDestination(smoothedModValues.noiseLevelOffset, rawModValues.noiseLevelOffset);
            smoothDestination(smoothedModValues.nativeNoisePitchOffset, rawModValues.nativeNoisePitchOffset);

            for (size_t modulationIndex = 0; modulationIndex < smoothedModValues.oscillatorPitchSemitones.size(); ++modulationIndex)
            {
                smoothDestination(smoothedModValues.nativeCoarsePitchSemitones[modulationIndex],rawModValues.nativeCoarsePitchSemitones[modulationIndex]);
                smoothDestination(smoothedModValues.nativeOctaveOffsets[modulationIndex],rawModValues.nativeOctaveOffsets[modulationIndex]);
                smoothDestination(smoothedModValues.nativeUnisonDetuneOffsets[modulationIndex],rawModValues.nativeUnisonDetuneOffsets[modulationIndex]);
                smoothDestination(smoothedModValues.nativeUnisonWidthOffsets[modulationIndex],rawModValues.nativeUnisonWidthOffsets[modulationIndex]);
                smoothDestination(smoothedModValues.nativeFinePitchCents[modulationIndex],rawModValues.nativeFinePitchCents[modulationIndex]);
                smoothDestination(smoothedModValues.nativeSemiPitchSemitones[modulationIndex],rawModValues.nativeSemiPitchSemitones[modulationIndex]);
                smoothDestination(smoothedModValues.nativePanOffsets[modulationIndex],rawModValues.nativePanOffsets[modulationIndex]);
                smoothDestination(smoothedModValues.oscillatorPitchSemitones[modulationIndex],
                                  rawModValues.oscillatorPitchSemitones[modulationIndex]);
                smoothDestination(smoothedModValues.oscillatorLevelOffsets[modulationIndex],
                                  rawModValues.oscillatorLevelOffsets[modulationIndex]);
                smoothDestination(smoothedModValues.wavetablePositionOffsets[modulationIndex],
                                  rawModValues.wavetablePositionOffsets[modulationIndex]);
                smoothDestination(smoothedModValues.fmIndexOffsets[modulationIndex],
                                  rawModValues.fmIndexOffsets[modulationIndex]);
                smoothDestination(smoothedModValues.oscillatorPanOffsets[modulationIndex],
                                  rawModValues.oscillatorPanOffsets[modulationIndex]);
            }

            if (! activeRoutes && ! hasResidualModulation(smoothedModValues))
            {
                smoothedModValues = {};
                modulationResidualMask = 0;
                modulationResidualMaskValid = true;
                smoothingModulation = false;
            }
        }

        // Explicit audio-rate lanes follow the source sample timeline. UI/base
        // parameters and ordinary lanes retain their existing smoothing.
        const bool combineModulation = renderMatrix || smoothingModulation;
        if (combineModulation) {
            combinedModulationValues = smoothedModValues;
            combinedModulationValues.globalPitchSemitones+=rawModValues.audioPitch;
            combinedModulationValues.subLevelOffset+=rawModValues.audioSubLevel;
            combinedModulationValues.noiseLevelOffset+=rawModValues.audioNoiseLevel;
            for(size_t i=0;i<2;++i) {
                combinedModulationValues.oscillatorPitchSemitones[i]+=rawModValues.audioOscPitch[i];
                combinedModulationValues.oscillatorLevelOffsets[i]+=rawModValues.audioOscLevel[i];
                combinedModulationValues.fmIndexOffsets[i]+=rawModValues.audioFmIndex[i];
            }
        }
        const auto& modulationValues = combineModulation ? combinedModulationValues : smoothedModValues;

        // The static plan owns random detune, blend and pan. Only the symmetric
        // detune span changes here, so modulation never restarts a unison voice.
        std::array<float, 2> unisonDetuneDeltaOctaves{};
        for (size_t i = 0; i < unisonDetuneDeltaOctaves.size(); ++i)
            if (renderedUnisonVoiceCounts[i] > 1 && modulationValues.nativeUnisonDetuneOffsets[i] != 0.0f)
                unisonDetuneDeltaOctaves[i] = (oscillators::serumModulatedDetuneCents(
                    baseOscillatorSnapshots[i].detuneCents, modulationValues.nativeUnisonDetuneOffsets[i])
                    - baseOscillatorSnapshots[i].detuneCents) / 1200.0f;

        std::array<float, 2> voiceSample{0.0f, 0.0f};
        std::array<float, 2> directSample{};
        std::array<float, 2> filterBypass{};
        auto routeGains = staticFilterInputGains;
        if (!filterRoutingFixed)
            for (size_t i = 0; i < routeGains.size(); ++i) routeGains[i] = filterInputGains[i].getNextValue();
        auto wetGain = filterRoutingFixed ? filterWetGain.getCurrentValue() : filterWetGain.getNextValue();
        const auto baseMix = filterRoutingFixed ? filterMixBaseGain.getCurrentValue() : filterMixBaseGain.getNextValue();
        const auto filterEnabled = filterRoutingFixed ? filterEnabledGain.getCurrentValue() : filterEnabledGain.getNextValue();
        if (modulationValues.nativeFilterMixOffset != 0.0f)
        {
            // Keep the legacy wet ramp exactly when there is no native route.
            // The added contribution also fades on bypass, including base Mix=0.
            const auto modulatedMix = std::clamp(baseMix + modulationValues.nativeFilterMixOffset, 0.0f, 1.0f);
            wetGain = std::clamp(wetGain + filterEnabled * (modulatedMix - baseMix), 0.0f, 1.0f);
        }
        const auto routeSource = [&](const std::array<float, 2>& previous, size_t source)
        {
            if (routeGains[source] >= 1.0f) return;
            for (size_t channel = 0; channel < 2; ++channel)
            {
                const auto contribution = voiceSample[channel] - previous[channel];
                const auto dry = contribution * (1.0f - routeGains[source]);
                filterBypass[channel] += dry;
                voiceSample[channel] = previous[channel] + contribution * routeGains[source];
            }
        };


        // Count enabled oscillators for mix compensation (1/sqrt(N) scaling)
        int enabledOscCount = staticEnabledOscillatorCount;
        if (! snapshotsFixed)
        {
            for (size_t i = 0; i < oscillators.size(); ++i)
            {
                auto& renderedSnapshot = renderedOscillatorSnapshots[i];
                updateRenderedOscillatorSnapshot(i,
                                                 modulationValues,
                                                 baseOscillatorSnapshots[i],
                                                 renderedSnapshot);
                if (renderedSnapshot.enabled && renderedSnapshot.level >= 0.0001f)
                    ++enabledOscCount;
            }
        }
        float oscMixScale = staticOscillatorMixScale;
        if (! snapshotsFixed)
        {
            continuousOscillatorMixScaleTarget = parameterSnapshot->global.serumAmplitude ? 2.0f : parameterSnapshot->global.vitalAmplitude ? 1.0f : enabledOscCount > 1
                ? 1.0f / std::sqrt(static_cast<float>(enabledOscCount))
                : 1.0f;
            const auto mixScaleDifference =
                continuousOscillatorMixScaleTarget - continuousOscillatorMixScale;
            continuousOscillatorMixScale = std::abs(mixScaleDifference) <= 1.0e-6f
                ? continuousOscillatorMixScaleTarget
                : continuousOscillatorMixScale + modSmoothCoeff * mixScaleDifference;
            oscMixScale = continuousOscillatorMixScale;
        }

        const auto usesCross = [&](size_t i) noexcept {
            const auto& oscillator = renderedOscillatorSnapshots[i];
            return oscillator.enabled && oscillator.mode == parameters::OscillatorMode::wavetable
                && oscillator.crossMode > 0;
        };
        const bool hasCrossModulation = usesCross(0) || usesCross(1);
        // Static and unmodulated oscillator pitch paths do not consume these
        // destinations. Cross modulation retains its exact per-sample tuning.
        std::array<float, 2> renderedCoarsePitch{};
        if (! staticPitchForBlock && (! pitchUnmodulated || hasCrossModulation))
        {
            renderedCoarsePitch = parameterSnapshot->global.serumAmplitude
                ? rawModValues.nativeCoarsePitchSemitones
                : modulationValues.nativeCoarsePitchSemitones;
            if (nativeNoiseSource)
                for (size_t i=0;i<renderedCoarsePitch.size();++i)
                    renderedCoarsePitch[i]=std::clamp(renderedCoarsePitch[i]+rawModValues.audioRateCoarsePitchSemitones[i], -64.0f, 64.0f);

            for (size_t i = 0; i < renderedCoarsePitch.size(); ++i)
                renderedCoarsePitch[i] += nativeOctavePitchOffset(baseOscillatorSnapshots[i].octave,
                    baseOscillatorSnapshots[i].nativeOctavePosition, modulationValues.nativeOctaveOffsets[i]);
        }
        if (hasCrossModulation)
        {
            for (size_t i = 0; i < oscillators.size(); ++i)
            {
                const auto& source = renderedOscillatorSnapshots[i];
                if (!staticPitchForBlock)
                {
                    const auto pitchMod = modulationValues.oscillatorPitchSemitones[i]
                        + modulationValues.globalPitchSemitones + modulationValues.nativeMasterPitchSemitones
                        + managerPitchDelta + renderedCoarsePitch[i]
                        + modulationValues.nativeSemiPitchSemitones[i]
                        + modulationValues.nativeFinePitchCents[i]*.01f
                        + driftStates[i].currentValue * source.drift;
                    const auto frequency = (source.pitchTracking
                        ? currentBaseFrequencyHz : untrackedOscillatorBaseHz) * oscillatorBaseTuningRatios[i]
                        * fastExp2(pitchMod / 12.0f);
                    const auto ratio = renderedUnisonVoiceCounts[i] > 1
                        ? unisonRenderCaches[i].plan.frequencyRatios[0] * fastExp2(-.5f * unisonDetuneDeltaOctaves[i]) : 1.0f;
                    oscillators[i].setFrequencyWithoutPhaseJitter(frequency * ratio);
                    const auto frameSize = oscWavetablePtrs[i] != nullptr
                        ? oscWavetablePtrs[i]->frameSize : wavetable::WavetableData::defaultFrameSize;
                    oscillators[i].setWavetableMipLevel(wavetable::WavetableData::calculateIdealMipLevel(
                        frameSize, frequency * ratio / static_cast<float>(sampleRate)));
                }
            }
            for (size_t i = 0; i < oscillators.size(); ++i)
            {
                if (!usesCross(i)) continue;
                const auto source = oscillators[1-i].previewCrossSource(renderedOscillatorSnapshots[1-i]);
                oscillators[i].setCrossSource(source);
                for (int u = 1; u < renderedUnisonVoiceCounts[i]; ++u)
                    unisonOscillators[i][static_cast<size_t>(u-1)].setCrossSource(source);
            }
        }

        std::array<float, 4> unmodulatedPitchRatios{};
        const bool batchPitch = pitchUnmodulated && !staticPitchForBlock && !hasCrossModulation
            && renderedOscillatorSnapshots[0].enabled && renderedOscillatorSnapshots[1].enabled;
        if (batchPitch)
        {
            for (size_t i = 0; i < 2; ++i)
                unmodulatedPitchRatios[i] = (0.0f + driftStates[i].currentValue
                    * renderedOscillatorSnapshots[i].drift) / 12.0f;
            fastExp2Four(unmodulatedPitchRatios.data());
        }
        for (size_t oscillatorIndex = 0; oscillatorIndex < oscillators.size(); ++oscillatorIndex)
        {
            const auto previousFilterInput = voiceSample;
            const auto& oscillatorSnapshot = renderedOscillatorSnapshots[oscillatorIndex];

            const bool isCrossSource = usesCross(1-oscillatorIndex);
            if (!oscillatorSnapshot.enabled || (oscillatorSnapshot.level < 0.0001f && !isCrossSource))
                continue;

            const int unisonCount = renderedUnisonVoiceCounts[oscillatorIndex];
            auto tunedCentreFrequency = staticTunedCentreFrequencies[oscillatorIndex];
            if (! staticPitchForBlock)
            {
                const float driftScale = driftStates[oscillatorIndex].currentValue
                    * oscillatorSnapshot.drift;
                const float pitchMod = pitchUnmodulated ? 0.0f + driftScale
                    : modulationValues.oscillatorPitchSemitones[oscillatorIndex]
                    + modulationValues.globalPitchSemitones +modulationValues.nativeMasterPitchSemitones
                    + managerPitchDelta + renderedCoarsePitch[oscillatorIndex]
                    + modulationValues.nativeSemiPitchSemitones[oscillatorIndex]
                    + modulationValues.nativeFinePitchCents[oscillatorIndex]*.01f
                    + driftScale;
                tunedCentreFrequency = oscillatorSnapshot.pitchTracking
                    ? currentBaseFrequencyHz : untrackedOscillatorBaseHz;
                if (utils::isFinite(pitchMod))
                {
                    tunedCentreFrequency *= oscillatorBaseTuningRatios[oscillatorIndex];
                    if (pitchMod != 0.0f)
                        tunedCentreFrequency *= batchPitch ? unmodulatedPitchRatios[oscillatorIndex]
                            : fastExp2(pitchMod / 12.0f);
                }
            }

            if (unisonCount <= 1)
            {
                // Single voice — detune controls unison spread and does not affect the centre voice.
                bool frequencyChanged = false;
                if (staticPitchForBlock)
                    oscillators[oscillatorIndex].advancePhaseJitter();
                else
                    frequencyChanged = oscillators[oscillatorIndex].setFrequency(
                        tunedCentreFrequency);
                if (oscillatorSnapshot.mode == parameters::OscillatorMode::wavetable
                    && ! staticPitchForBlock
                    && (frequencyChanged || sampleIndex == 0))
                {
                    const auto frameSize = oscWavetablePtrs[oscillatorIndex] != nullptr
                        ? oscWavetablePtrs[oscillatorIndex]->frameSize
                        : wavetable::WavetableData::defaultFrameSize;
                    oscillators[oscillatorIndex].setWavetableMipLevel(
                        wavetable::WavetableData::calculateIdealMipLevel(
                            frameSize,
                            tunedCentreFrequency / static_cast<float>(sampleRate)));
                }
                const auto oscSample = snapshotsFixed
                    && oscillatorSnapshot.mode == parameters::OscillatorMode::wavetable
                    ? oscillators[oscillatorIndex].renderStaticWavetableSample(
                          oscillatorSnapshot)
                    : oscillators[oscillatorIndex].renderSample(oscillatorSnapshot);
                if (snapshotsFixed)
                {
                    voiceSample[0] += oscSample
                        * unisonRenderCaches[oscillatorIndex].staticPanGainLeft[0];
                    voiceSample[1] += oscSample
                        * unisonRenderCaches[oscillatorIndex].staticPanGainRight[0];
                }
                else
                {
                    const auto panned = applyPan(
                        oscSample * oscillatorSnapshot.level * oscMixScale,
                        oscillatorSnapshot.pan, parameterSnapshot->global.serumAmplitude, parameterSnapshot->global.vitalAmplitude);
                    voiceSample[0] += panned[0];
                    voiceSample[1] += panned[1];
                }
            }
            else
            {
                // Unison: render multiple detuned voices panned across the stereo field.
                // VoiceCore 0 uses the primary oscillator; voices 1..N-1 use unisonOscillators.
                const auto& unisonPlan = unisonRenderCaches[oscillatorIndex].plan;
                const auto& renderCache = unisonRenderCaches[oscillatorIndex];
                const auto addStaticUnisonSample = [&](int unisonIndex, float oscSample) noexcept
                {
                    const auto planIndex = static_cast<size_t>(unisonIndex);
                    if (steadyControls || !hasNativeWidthRoutes[oscillatorIndex])
                    {
                        voiceSample[0] += oscSample * renderCache.staticPanGainLeft[planIndex];
                        voiceSample[1] += oscSample * renderCache.staticPanGainRight[planIndex];
                        return;
                    }
                    const auto width = std::clamp(oscillatorSnapshot.unisonWidth + modulationValues.nativeUnisonWidthOffsets[oscillatorIndex], 0.0f, 1.0f);
                    const auto gain = oscSample * unisonPlan.gainScales[planIndex]
                        * oscillatorSnapshot.level * oscMixScale;
                    if (parameterSnapshot->global.serumAmplitude)
                    {
                        const auto panned = applyPan(gain, oscillatorSnapshot.pan, true);
                        const auto stereo = oscillators::serumUnisonStereoGains(unisonIndex, unisonCount, width);
                        voiceSample[0] += panned[0] * stereo[0];
                        voiceSample[1] += panned[1] * stereo[1];
                    }
                    else
                    {
                        const auto pan = oscillatorSnapshot.pan
                            + (static_cast<float>(unisonIndex) / static_cast<float>(unisonCount - 1) - 0.5f)
                                * 2.0f * width;
                        const auto panned = applyPan(gain, pan, false, parameterSnapshot->global.vitalAmplitude);
                        voiceSample[0] += panned[0];
                        voiceSample[1] += panned[1];
                    }
                };

                if (staticPitchForBlock && snapshotsFixed)
                {
                    // SIMD helpers accept at most seven lanes and return false above that.
                    // The scalar fallback below handles all sixteen preallocated voices.
                    std::array<float, parameters::maxUnisonVoiceCount> unisonSamples{};
                    if (useUnisonSpans)
                        unisonSpans[oscillatorIndex].advancePhaseJitter();
                    else
                    {
                        for (int u = 0; u < unisonCount; ++u)
                        {
                            auto& oscBlock = (u == 0)
                                ? oscillators[oscillatorIndex]
                                : unisonOscillators[oscillatorIndex][static_cast<size_t>(u - 1)];
                            oscBlock.advancePhaseJitter();
                        }
                    }

                    auto batchedOscillators = false;
                    if (useUnisonSpans)
                        batchedOscillators = unisonSpans[oscillatorIndex].render(unisonSamples.data());
                    else if (oscillatorSnapshot.mode == parameters::OscillatorMode::wavetable)
                    {
                        batchedOscillators = fadmodel1::dsp::oscillators::OscillatorBlock::
                            renderStaticWavetableUnison(
                                oscillators[oscillatorIndex],
                                unisonOscillators[oscillatorIndex].data(),
                                unisonCount,
                                oscillatorSnapshot,
                                unisonSamples.data());
                    }
                    else if (oscillatorSnapshot.mode == parameters::OscillatorMode::fm2Op)
                    {
                        batchedOscillators = fadmodel1::dsp::oscillators::OscillatorBlock::
                            renderStaticFmUnison(
                                oscillators[oscillatorIndex],
                                unisonOscillators[oscillatorIndex].data(),
                                unisonCount,
                                oscillatorSnapshot,
                                unisonSamples.data());
                    }
                    for (int u = 0; u < unisonCount; ++u)
                    {
                        const auto planIndex = static_cast<size_t>(u);
                        auto& oscBlock = (u == 0)
                            ? oscillators[oscillatorIndex]
                            : unisonOscillators[oscillatorIndex][static_cast<size_t>(u - 1)];
                        const auto oscSample = batchedOscillators
                            ? unisonSamples[planIndex]
                            : oscillatorSnapshot.mode == parameters::OscillatorMode::wavetable
                                ? oscBlock.renderStaticWavetableSample(oscillatorSnapshot)
                                : oscBlock.renderSample(oscillatorSnapshot);
                        addStaticUnisonSample(u, oscSample);
                    }
                }
                else
                {
                    float centreMipLevel = 0.0f;
                    bool centreMipLevelReady = false;

                    for (int u = 0; u < unisonCount; ++u)
                    {
                        const auto planIndex = static_cast<size_t>(u);
                        auto& oscBlock = (u == 0)
                            ? oscillators[oscillatorIndex]
                            : unisonOscillators[oscillatorIndex][static_cast<size_t>(u - 1)];
                        bool frequencyChanged = false;
                        const float detuneDelta = (static_cast<float>(u) / static_cast<float>(unisonCount - 1) - .5f)
                            * unisonDetuneDeltaOctaves[oscillatorIndex];
                        if (staticPitchForBlock)
                            oscBlock.advancePhaseJitter();
                        else
                            frequencyChanged = oscBlock.setFrequency(
                                tunedCentreFrequency * unisonPlan.frequencyRatios[planIndex] * fastExp2(detuneDelta));
                        if (oscillatorSnapshot.mode == parameters::OscillatorMode::wavetable
                            && ! staticPitchForBlock
                            && (frequencyChanged || sampleIndex == 0))
                        {
                            if (! centreMipLevelReady)
                            {
                                centreMipLevel = wavetable::WavetableData::calculateIdealMipLevel(
                                    oscWavetablePtrs[oscillatorIndex] != nullptr
                                        ? oscWavetablePtrs[oscillatorIndex]->frameSize
                                        : wavetable::WavetableData::defaultFrameSize,
                                    tunedCentreFrequency / static_cast<float>(sampleRate));
                                centreMipLevelReady = true;
                            }
                            oscBlock.setWavetableMipLevel(
                                centreMipLevel + unisonPlan.mipLevelOffsets[planIndex] + detuneDelta);
                        }

                        const auto oscSample = snapshotsFixed
                                && oscillatorSnapshot.mode
                                    == parameters::OscillatorMode::wavetable
                            ? oscBlock.renderStaticWavetableSample(oscillatorSnapshot)
                            : oscBlock.renderSample(oscillatorSnapshot);
                        if (snapshotsFixed)
                        {
                            addStaticUnisonSample(u, oscSample);
                        }
                        else
                        {
                            const auto width = std::clamp(oscillatorSnapshot.unisonWidth + modulationValues.nativeUnisonWidthOffsets[oscillatorIndex], 0.0f, 1.0f);
                            const auto voicePan = oscillatorSnapshot.pan
                                + ((! steadyControls && hasNativeWidthRoutes[oscillatorIndex])
                                    ? (static_cast<float>(u) / static_cast<float>(unisonCount - 1) - 0.5f) * 2.0f * width
                                    : unisonPlan.panOffsets[planIndex]);
                            const auto voiceGain = unisonPlan.gainScales[planIndex]
                                * oscillatorSnapshot.level * oscMixScale;
                            auto panned = applyPan(oscSample * voiceGain, voicePan, parameterSnapshot->global.serumAmplitude, parameterSnapshot->global.vitalAmplitude);
                            if (parameterSnapshot->global.serumAmplitude)
                            {
                                panned = applyPan(oscSample * voiceGain, oscillatorSnapshot.pan, true);
                                if (!steadyControls && hasNativeWidthRoutes[oscillatorIndex])
                                {
                                    const auto stereo = oscillators::serumUnisonStereoGains(u, unisonCount, width);
                                    panned[0] *= stereo[0];
                                    panned[1] *= stereo[1];
                                }
                                else
                                {
                                    panned[0] *= unisonPlan.nativePanLeft[planIndex];
                                    panned[1] *= unisonPlan.nativePanRight[planIndex];
                                }
                            }
                            voiceSample[0] += panned[0];
                            voiceSample[1] += panned[1];
                        }
                    }
                }
            }
            if (oscillatorUiCapture != nullptr)
            {
                const auto index = oscillatorUiCaptureOffset + sampleIndex;
                if (index == OscillatorUiFrame::sampleCount - 1)
                {
                    oscillatorUiCapture->positions[oscillatorIndex] = oscillatorSnapshot.wavetablePosition;
                    oscillatorUiCapture->slots[oscillatorIndex] = oscillatorSnapshot.wavetableSlot;
                    oscillatorUiCapture->modes[oscillatorIndex] = static_cast<int>(oscillatorSnapshot.mode);
                    oscillatorUiCapture->tableGeneration = blockWavetableGeneration;
                }
                if (index >= 0 && index < OscillatorUiFrame::sampleCount)
                    for (size_t channel = 0; channel < 2; ++channel)
                    {
                        const auto value = voiceSample[channel] - previousFilterInput[channel];
                        const bool finite = std::isfinite(value);
                        oscillatorUiCapture->finite = oscillatorUiCapture->finite && finite;
                        oscillatorUiCapture->samples[oscillatorIndex][channel][static_cast<size_t>(index)]
                            = finite ? value : 0.0f;
                    }
            }
            routeSource(previousFilterInput, oscillatorIndex);
        }

        const auto beforeSubInput = voiceSample;
        const float renderedSubLevel =
            std::clamp(continuousSubLevel + modulationValues.subLevelOffset, 0.0f, 1.0f);

        if (continuousSubEnabled > 0.0f && (useNativeSub || renderedSubLevel > 0.0001f))
        {
            const auto subPitchSemitones = pitchBendSemitones + midi2NoteBend + midi2TuningOffset + managerPitchSemitones
                + modulationValues.globalPitchSemitones + modulationValues.nativeMasterPitchSemitones;
            const float subFreq = subPitchFixed ? staticSubFrequency
                : currentBaseFrequencyHz * subOctaveRatio * fastExp2(subPitchSemitones / 12.0f);
            const auto subFrequencyChanged = useNativeSub
                ? subOscillator.setFrequencyWithoutPhaseJitter(subFreq)
                : subOscillator.setFrequency(subFreq);
            if (useNativeSub && (subFrequencyChanged || sampleIndex == 0))
                subOscillator.setWavetableMipLevel(wavetable::WavetableData::calculateIdealMipLevel(
                    nativeSubTable->frameSize, subFreq / static_cast<float>(sampleRate)));
            const auto subSample = subOscillator.renderSample(subSnapshot) * renderedSubLevel
                * continuousSubEnabled * (useNativeSub ? 2.0f : 0.5f);
            const auto subStereo = continuousSmoothing
                ? applyPan(subSample, continuousSubPan, useNativeSub)
                : std::array<float, 2>{subSample * staticSubPanGains[0], subSample * staticSubPanGains[1]};
            const auto routed = 1.0f - continuousSubDirect;
            voiceSample[0] += subStereo[0] * routed;
            voiceSample[1] += subStereo[1] * routed;
            directSample[0] += subStereo[0] * continuousSubDirect;
            directSample[1] += subStereo[1] * continuousSubDirect;
        }

        routeSource(beforeSubInput, 2);
        const auto beforeNoiseInput = voiceSample;
        if(nativeNoisePitch || (noiseConfig.keytrack && nativeMasterTuning))
            updateNoisePitch(modulationValues);
        float renderedNoiseLevel = std::clamp(continuousNoiseLevel + modulationValues.noiseLevelOffset, 0.0f, parameterSnapshot->noiseWideLevel ? 4.0f : 1.0f);
        if (parameterSnapshot->global.serumAmplitude && modulationValues.noiseLevelOffset != 0.0f)
        {
            const auto knob = std::clamp(std::sqrt(std::max(0.0f, continuousNoiseLevel)) + modulationValues.noiseLevelOffset, 0.0f, parameterSnapshot->noiseWideLevel ? 2.0f : 1.0f);
            renderedNoiseLevel = knob * knob;
        }

        if (continuousNoiseEnabled > 0.0f)
        {
            // A sampled source advances even when its level is zero: modulation
            // changes gain, not the sample's playback clock.
            if (!nativeNoiseSource)
            {
                std::array<float, 2> builtin{};
                if (noiseSamplePlayer.needsBuiltinSample())
                    builtin.fill(renderedNoiseLevel > 0.0001f ? renderNoiseSample() : 0.0f);
                audibleNoiseSample = noiseSamplePlayer.nextStereoSample(builtin);
            }
            const auto pcmGain = parameterSnapshot->global.serumAmplitude ? 2.0f : 0.35f;
            const bool transitioning = noiseSamplePlayer.lastSampleWasTransition();
            const auto nativeGain = transitioning ? 1.0f : noiseConfig.sampleSlot > 0 ? pcmGain : 0.35f;
            if (transitioning) audibleNoiseSample = noiseSamplePlayer.lastAudibleSample(pcmGain, 0.35f);
            const auto routed = 1.0f - continuousNoiseDirect;
            for (size_t channel = 0; channel < 2; ++channel)
            {
                const auto sample = audibleNoiseSample[channel] * renderedNoiseLevel * continuousNoiseEnabled
                    * nativeGain * noisePanGains[channel];
                voiceSample[channel] += sample * routed;
                directSample[channel] += sample * continuousNoiseDirect;
            }
        }

        routeSource(beforeNoiseInput, 3);
        if(nodeControlCounter==0)
        {
            const auto nodeSources=nodeCurves?makeModulationSources(ampEnvelopeValue,filterEnvelopeValue,modEnvelopeValue,sampleIndex,true):modulation::SourceValues{};
            for(size_t i=0;i<2;++i)if(parameterSnapshot->curves[i].enabled || nodeWaves[i].mix>0)nodeWaves[i].prepare(modulation::modulatedCurve(parameterSnapshot->curves[i],nodeSources.nativeLfos),sampleRate);
            if(nodeCurves)for(size_t i=0;i<parameters::lfoCount;++i)nodeLfos[i].prepare(modulation::modulatedCurve(parameterSnapshot->curves[i+2],nodeSources.nativeLfos));
            nodeFilterCurve=modulation::modulatedCurve(parameterSnapshot->curves[10],nodeSources.nativeLfos);
        }
        nodeControlCounter=(nodeControlCounter+1)&63;
        for(size_t i=0;i<2;++i)if(nodeWaves[i].mix>0 || parameterSnapshot->curves[i].enabled)nodeWaves[i].advance();
        if (filterCoefficientSampleCounter == 0)
        {
            // Serum applies its native filter destinations on the source timeline.
            // Do not run them through FAD's legacy 5 ms destination smoother a
            // second time.  This is especially audible for LFO -> cutoff: the
            // extra smoothing rounds each native curve corner before the filter's
            // own bounded coefficient update cadence.  Keep legacy FAD patches on
            // the existing smoothed path.
            auto filterModulationValues = modulationValues;
            if (parameterSnapshot->global.serumAmplitude)
            {
                filterModulationValues.nativeFilterCutoffOffset = rawModValues.nativeFilterCutoffOffset;
                filterModulationValues.nativeFilterResonanceOffset = rawModValues.nativeFilterResonanceOffset;
                filterModulationValues.nativeFilterDriveOffset = rawModValues.nativeFilterDriveOffset;
                filterModulationValues.nativeFilterVariationOffset = rawModValues.nativeFilterVariationOffset;
            }
            auto filterSnapshot = createRenderedFilterSnapshot(filterEnvelopeValue, filterModulationValues);
            filterSnapshot.curve=nodeFilterCurve;
            filterSnapshot.curve.enabled=filterSnapshot.curve.enabled && filterSnapshot.enabled && filterSnapshot.mix>0;
            const auto centreCutoff = filterSnapshot.cutoffHz;
            filterSnapshot.cutoffHz = centreCutoff * leftFilterRatio;
            filters[0].updateCoefficients(filterSnapshot, filterCoefficientUpdateInterval);
            filterSnapshot.cutoffHz = centreCutoff * rightFilterRatio;
            filters[1].updateCoefficients(filterSnapshot, filterCoefficientUpdateInterval);
        }
        filterCoefficientSampleCounter =
            (filterCoefficientSampleCounter + 1) & (filterCoefficientUpdateInterval - 1);
        for (size_t channel = 0; channel < 2; ++channel)
        {
            const auto dry = voiceSample[channel];
            const auto wet = filters[channel].processSample(dry);
            voiceSample[channel] = wetGain >= 1.0f ? wet : dry + wetGain * (wet - dry);
            if (filterBypass[channel] != 0.0f) voiceSample[channel] += filterBypass[channel];
        }

        const auto velocitySensitivity = voiceGainFixed
            ? staticVelocitySensitivity : velocitySensitivitySmoother.getNextValue();
        const auto velocityGain = voiceGainFixed ? staticVelocityGain : parameterSnapshot->global.serumAmplitude ? 1.0f
            : (velocitySensitivity == 1.0f ? currentVelocity
                : 1.0f + velocitySensitivity * (currentVelocity - 1.0f));
        auto gain = voiceGainFixed ? staticVoiceGain * ampEnvelopeValue
            : velocityGain * channelVolumeSmoother.getNextValue()
            * expressionSmoother.getNextValue()
            * mpeManagerVolumeSmoother.getNextValue()
            * mpeManagerExpressionSmoother.getNextValue() * ampEnvelopeValue;
        if (parameterSnapshot->global.vitalAmplitude)
            gain *= velocityGain * ampEnvelopeValue;
        voiceSample[0] *= gain;
        voiceSample[1] *= gain;
        directSample[0] *= gain; directSample[1] *= gain;

        if (structuralFadeRemaining > 0)
        {
            const auto fadeOut = static_cast<float>(structuralFadeRemaining)
                / static_cast<float>(structuralFadeSamples);
            voiceSample[0] = structuralFadeStartSample[0] * fadeOut
                + voiceSample[0] * (1.0f - fadeOut);
            voiceSample[1] = structuralFadeStartSample[1] * fadeOut
                + voiceSample[1] * (1.0f - fadeOut);
            directSample[0] = structuralDirectFadeStart[0] * fadeOut + directSample[0] * (1.0f - fadeOut);
            directSample[1] = structuralDirectFadeStart[1] * fadeOut + directSample[1] * (1.0f - fadeOut);
            --structuralFadeRemaining;
        }
        lastRenderedVoiceBodySample = voiceSample;
        lastRenderedDirectBody = directSample;
        hasRenderedVoiceBodySample = true;

        // VoiceCore steal fade: apply per-sample ramp to THIS voice only (not the
        // shared output buffer) to prevent other voices from being attenuated.
        if (stealFadeRemaining > 0)
        {
            const float fadeGain = static_cast<float>(stealFadeRemaining)
                                   / static_cast<float>(stealFadeSamples);
            voiceSample[0] *= fadeGain;
            voiceSample[1] *= fadeGain;
            directSample[0] *= fadeGain; directSample[1] *= fadeGain;
            --stealFadeRemaining;
        }

        if (stealTailRemaining > 0)
        {
            const float tailGain = std::clamp(static_cast<float>(stealTailRemaining)
                                                    / static_cast<float>(stealFadeSamples), 0.0f, 1.0f);
            voiceSample[0] += stealTailSample[0] * tailGain;
            voiceSample[1] += stealTailSample[1] * tailGain;
            directSample[0] += stealDirectTailSample[0] * tailGain;
            directSample[1] += stealDirectTailSample[1] * tailGain;
            --stealTailRemaining;
        }

        lastRenderedVoiceSample = voiceSample;
        for(size_t channel=0;channel<2;++channel)
            if(directSample[channel]!=0.0f) lastRenderedVoiceSample[channel]+=directSample[channel];

        const int idx = startSample + sampleIndex;
        const auto directIndex = idx - directOutOrigin;
        const bool hasDirectBuffer = hasOutput && directOutBuffer.has_value()
            && directIndex >= 0 && directIndex < directOutBuffer->frames()
            && directOutBuffer->channels() >= (isMono ? 1 : 2);
        if(hasDirectBuffer) {
            if(isMono) directOutBuffer->channel(0)[directIndex] += 0.5f*(directSample[0]+directSample[1]);
            else {
                directOutBuffer->channel(0)[directIndex] += directSample[0];
                directOutBuffer->channel(1)[directIndex] += directSample[1];
            }
        } else {
            // Standalone VoiceCore callers still hear the direct source after its
            // per-voice filter; the engine supplies a separate post-FX bus.
            voiceSample = lastRenderedVoiceSample;
        }
        if (! hasOutput)
        {
            // Zero-channel host callbacks still advance envelopes, oscillators,
            // MIDI and voice lifetime; there is simply no destination to write.
        }
        else if (isMono)
        {
            const auto monoSample =
                fadmodel1::dsp::utils::sanitizeFinite(0.5f * (voiceSample[0] + voiceSample[1]));
            outL[idx] = fadmodel1::dsp::utils::sanitizeFinite(outL[idx] + monoSample);
        }
        else
        {
            const auto leftSample = fadmodel1::dsp::utils::sanitizeFinite(voiceSample[0]);
            const auto rightSample = fadmodel1::dsp::utils::sanitizeFinite(voiceSample[1]);
            outL[idx] = fadmodel1::dsp::utils::sanitizeFinite(outL[idx] + leftSample);
            outR[idx] = fadmodel1::dsp::utils::sanitizeFinite(outR[idx] + rightSample);
        }

        // Advance drift oscillators (slow random pitch deviation)
        for (auto& ds : driftStates)
        {
            ds.phase += ds.rate * inverseSampleRate;
            if (ds.phase >= 1.0f) ds.phase -= 1.0f;
            // Smooth triangle-ish LFO: ±0.15 semitones max when drift = 1.0
            ds.currentValue = (2.0f * std::abs(2.0f * ds.phase - 1.0f) - 1.0f) * 0.15f;
        }

        // Complete a stop at its sample boundary. Waiting until the end of this
        // render chunk would bypass the fade once its counter reaches zero and
        // bring the stopped voice back at full gain for the remaining samples.
        if(ownedRateLfoMask!=0)
            for(size_t lfo=0;lfo<noteLfos.size();++lfo)
                if(parameterSnapshot->modulation.lfos[lfo].triggerMode>0 || (ownedRateLfoMask&(uint32_t{1}<<lfo))!=0) noteLfos[lfo].advance(1);
        const bool nativeResumeGraceActive = !nativeMemoryBlockTail
            && nativeReleaseResumeGraceRemaining > 0;
        const bool atNativeMemoryBoundary = !nativeMemoryBlockTail
            || idx + 1 >= outputBuffer.frames();
        if (nativeReleaseResumeGraceRemaining > 0)
            --nativeReleaseResumeGraceRemaining;
        if ((wasBeingStolen && stealFadeRemaining <= 0)
            || (! ampEnvelope.isActive() && !nativeResumeGraceActive && atNativeMemoryBoundary
                && stealFadeRemaining <= 0 && stealTailRemaining <= 0))
        {
            for (auto& span : unisonSpans) span.flush();
            reset();
            resetRealtimeState();
            clearCurrentNote();
            return true;
        }
    }
        return false;
    };
    if (steadyControlSpan ? renderSamples.template operator()<true>()
                          : renderSamples.template operator()<false>())
        return;

    for (size_t i = 0; i < noteLfos.size(); ++i)
        if (ownedRateLfoMask==0 && parameterSnapshot->modulation.lfos[i].triggerMode > 0) noteLfos[i].advance(numSamples);
    if (! ampEnvelope.isActive()
        && (nativeMemoryBlockTail || nativeReleaseResumeGraceRemaining <= 0)
        && (!nativeMemoryBlockTail || startSample + numSamples >= outputBuffer.frames()))
    {
        for (auto& span : unisonSpans) span.flush();
        reset();
        resetRealtimeState();
        clearCurrentNote();
    }
}

void VoiceCore::updateEnvelopeParameters() noexcept
{
    const auto& ampSnapshot = parameterSnapshot->ampEnvelope;
    ampEnvelopeParameters.attack = ampSnapshot.attackSeconds;
    ampEnvelopeParameters.decay = ampSnapshot.decaySeconds;
    ampEnvelopeParameters.sustain = ampSnapshot.sustainLevel;
    ampEnvelopeParameters.release = ampSnapshot.releaseSeconds;
    ampEnvelope.setHoldSeconds(ampSnapshot.holdSeconds);
    ampEnvelope.setCurves(ampSnapshot.curves);
    ampEnvelope.setParameters(ampEnvelopeParameters);

    const auto& filterSnapshot = parameterSnapshot->filterEnvelope;
    filterEnvelopeParameters.attack = filterSnapshot.attackSeconds;
    filterEnvelopeParameters.decay = filterSnapshot.decaySeconds;
    filterEnvelopeParameters.sustain = filterSnapshot.sustainLevel;
    filterEnvelopeParameters.release = filterSnapshot.releaseSeconds;
    filterEnvelope.setHoldSeconds(filterSnapshot.holdSeconds);
    filterEnvelope.setCurves(filterSnapshot.curves);
    filterEnvelope.setParameters(filterEnvelopeParameters);

    const auto& modSnapshot = parameterSnapshot->modEnvelope;
    modEnvelopeParameters.attack = modSnapshot.attackSeconds;
    modEnvelopeParameters.decay = modSnapshot.decaySeconds;
    modEnvelopeParameters.sustain = modSnapshot.sustainLevel;
    modEnvelopeParameters.release = modSnapshot.releaseSeconds;
    modEnvelope.setHoldSeconds(modSnapshot.holdSeconds);
    modEnvelope.setCurves(modSnapshot.curves);
    modEnvelope.setParameters(modEnvelopeParameters);
}

parameters::OscillatorSnapshot VoiceCore::createRenderedOscillatorSnapshot(
    size_t oscillatorIndex,
    const modulation::DestinationValues& modulationValues) const noexcept
{
    auto oscillatorSnapshot = parameterSnapshot->oscillators[oscillatorIndex];
    updateRenderedOscillatorSnapshot(oscillatorIndex,
                                     modulationValues,
                                     parameterSnapshot->oscillators[oscillatorIndex],
                                     oscillatorSnapshot);
    return oscillatorSnapshot;
}

void VoiceCore::updateRenderedOscillatorSnapshot(
    size_t oscillatorIndex,
    const modulation::DestinationValues& modulationValues,
    const parameters::OscillatorSnapshot& source,
    parameters::OscillatorSnapshot& target) const noexcept
{
    updateRenderedOscillatorSnapshot(*parameterSnapshot, oscillatorIndex, modulationValues, source, target);
}

void VoiceCore::updateRenderedOscillatorSnapshot(const parameters::ParameterSnapshot& snapshot,
    size_t oscillatorIndex, const modulation::DestinationValues& modulationValues,
    const parameters::OscillatorSnapshot& source, parameters::OscillatorSnapshot& target) noexcept
{
    const auto mappedTimbre = std::clamp(source.timbre, 0.0f, 1.0f);
    target.enabled = source.enabled;
    const auto levelOffset = modulationValues.oscillatorLevelOffsets[oscillatorIndex];
    if ((snapshot.global.serumAmplitude || snapshot.global.vitalAmplitude) && levelOffset != 0.0f)
    {
        const auto knob = std::clamp(std::sqrt(std::max(0.0f, source.level)) + levelOffset, 0.0f, 1.0f);
        target.level = knob * knob;
    }
    else target.level = std::clamp(source.level + levelOffset, 0.0f, 1.0f);
    target.pan = std::clamp(source.pan + modulationValues.oscillatorPanOffsets[oscillatorIndex] +modulationValues.nativePanOffsets[oscillatorIndex], -1.0f, 1.0f);

    switch (source.mode)
    {
        case parameters::OscillatorMode::analog:
            target.analogShape =
                std::clamp(0.5f * source.analogShape + 0.5f * mappedTimbre, 0.0f, 1.0f);
            target.analogPulseWidth = std::clamp(0.5f * source.analogPulseWidth + 0.5f * fad::engine::math::interpolate(mappedTimbre, 0.12f, 0.88f), 0.05f, 0.95f);
            break;

        case parameters::OscillatorMode::wavetable:
            target.wavetablePosition = std::clamp(0.5f * source.wavetablePosition + 0.5f * mappedTimbre
                    + modulationValues.wavetablePositionOffsets[oscillatorIndex], 0.0f, 1.0f);
            if (const auto warpModulation = modulationValues.fmIndexOffsets[oscillatorIndex];
                warpModulation != 0.0f)
            {
                target.wavetableWarp = std::clamp(source.wavetableWarp + warpModulation * 0.2f, 0.0f, 1.0f);
            }
            break;

        case parameters::OscillatorMode::fm2Op:
            target.fmIndex = std::clamp((source.wideFmIndex ? source.fmIndex : 0.35f * source.fmIndex + 0.65f * (mappedTimbre * 10.0f))
                    + modulationValues.fmIndexOffsets[oscillatorIndex], 0.0f, source.wideFmIndex ? 100.0f : 10.0f);
            break;
    }
}

parameters::FilterSnapshot VoiceCore::createRenderedFilterSnapshot(
    float filterEnvelopeValue,
    const modulation::DestinationValues& modulationValues) const noexcept
{
    auto renderedFilter = parameterSnapshot->filter;
    const auto keyReference=renderedFilter.nativeModel>=0 && renderedFilter.nativeCutoffHz>0 ? 32 : filterKeyTrackReferenceNote;
    const auto keyTrackOctaves=(static_cast<float>(pitchNoteNumber-keyReference)/12.0f)*renderedFilter.keyTrack;
    const auto envelopeOctaves =
        filterEnvelopeValue * renderedFilter.envAmount * maxFilterEnvelopeOctaves;
    const auto cutoffOctaves =
        keyTrackOctaves + envelopeOctaves + modulationValues.filterCutoffOctaves;

    const bool nativeCutoffOverride=renderedFilter.nativeModel>=0 && renderedFilter.nativeCutoffHz>0.0f;
    if (nativeCutoffOverride) {
        auto centre=renderedFilter.nativeCutoffHz;
        if (modulationValues.nativeFilterCutoffOffset!=0.0f || renderedFilter.keyTrack!=0.0f) {
            auto position=std::clamp(parameters::serum::cutoffFromHz(centre)+modulationValues.nativeFilterCutoffOffset, 0.0f, 1.0f);
            position+=keyTrackOctaves*(12.0f/136.8f);
            centre=position<0.0f ? parameters::serum::cutoffToHz(0.0f)*fastExp2(position*(136.8f/12.0f))
                                : parameters::serum::cutoffToHz(position);
        }
        renderedFilter.cutoffHz=std::clamp(centre*fastExp2(envelopeOctaves+modulationValues.filterCutoffOctaves), 1.0f, std::min(22050.0f,static_cast<float>(sampleRate)*.49f));
    } else if (modulationValues.nativeFilterCutoffOffset!=0.0f) {
        const auto centre=parameters::serum::cutoffToHz(parameters::serum::cutoffFromHz(renderedFilter.cutoffHz)+modulationValues.nativeFilterCutoffOffset);
        renderedFilter.cutoffHz=std::clamp(centre*fastExp2(cutoffOctaves), 20.0f, 20000.0f);
    } else {
        renderedFilter.cutoffHz=std::clamp(renderedFilter.cutoffHz*fastExp2(cutoffOctaves), 20.0f, 20000.0f);
    }
    renderedFilter.resonance = std::clamp(renderedFilter.resonance + modulationValues.filterResonanceOffset
            + .87f * modulationValues.nativeFilterResonanceOffset, 0.1f, maxFilterResonance);
    if (modulationValues.nativeFilterDriveOffset != 0.0f)
    {
        const auto drivePosition = std::pow(10.0f, renderedFilter.driveDb / 80.0f) - 1.0f;
        const auto modulatedPosition = std::clamp(drivePosition + modulationValues.nativeFilterDriveOffset, 0.0f, 1.0f);
        renderedFilter.driveDb = 80.0f * std::log10(1.0f + modulatedPosition);
    }
    renderedFilter.nativeVariation = std::clamp(renderedFilter.nativeVariation + modulationValues.nativeFilterVariationOffset, 0.0f, 1.0f);
    renderedFilter.driveDb =
        std::clamp(renderedFilter.driveDb + modulationValues.filterDriveDb, 0.0f, 36.0f);
    return renderedFilter;
}

void VoiceCore::syncPerformanceStateFromChannel() noexcept
{
    if (performanceStates == nullptr)
        return;

    if (cachedMidiChannel < 1 || cachedMidiChannel > static_cast<int>(performanceStates->size()))
        return;

    const auto& state = (*performanceStates)[static_cast<size_t>(cachedMidiChannel - 1)];
    modWheelValue = state.modWheel;
    if (! mpeMember || isKeyDown())
    {
        channelPressureValue = state.aftertouch;
        slideValue = midi2ChannelControls && !state.slideReceived ? .5f : state.slide;
    }
    channelVolumeSmoother.setTargetValue(mpeMember ? 1.0f : state.channelVolume);
    expressionSmoother.setTargetValue(mpeMember ? 1.0f : state.expression);
}

void VoiceCore::syncMidi2ChannelControls() noexcept
{
    midi2ChannelControls = true;
    syncPerformanceStateFromChannel();
}

std::array<float, 2> VoiceCore::applyPan(float sample, float pan, bool serumAmplitude, bool vitalAmplitude) noexcept
{
    // Lookup-based equal-power pan avoids per-sample sqrt in hot render paths.
    const auto normalisedPan = std::clamp(pan, -1.0f, 1.0f);
    const auto t = (normalisedPan + 1.0f) * 0.5f;
    if (serumAmplitude)
    {
        // Native pan sweeps: p + (2 * 10^(-3/20) - 1) * p * (1-p).
        constexpr float curvature = 0.4158915688f;
        const auto arc = curvature * t * (1.0f - t);
        return {sample * (1.0f - t + arc), sample * (t + arc)};
    }
    const auto& table = vitalAmplitude ? vitalPanTable : equalPowerPanTable;
    const auto scaled = t * static_cast<float>(panTableSize);
    const auto idx = std::clamp(static_cast<int>(scaled), 0, panTableSize - 1);
    const auto frac = scaled - static_cast<float>(idx);
    const auto left = fad::engine::math::interpolate(frac,
                                 table.left[static_cast<size_t>(idx)],
                                 table.left[static_cast<size_t>(idx + 1)]);
    const auto right = fad::engine::math::interpolate(frac,
                                  table.right[static_cast<size_t>(idx)],
                                  table.right[static_cast<size_t>(idx + 1)]);
    return {sample * left, sample * right};
}

float VoiceCore::renderNoiseSample() noexcept
{
    // Improved xorshift32 with full 32-bit range for better spectral flatness
    noiseState ^= noiseState << 13;
    noiseState ^= noiseState >> 17;
    noiseState ^= noiseState << 5;
    // Use all 32 bits for normalization (not just low 24)
    return static_cast<float>(static_cast<int32_t>(noiseState)) * (1.0f / 2147483648.0f);
}
}
