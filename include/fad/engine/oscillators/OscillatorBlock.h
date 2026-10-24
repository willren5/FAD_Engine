#pragma once

#include <cmath>
#include <limits>

#include <array>
#include <algorithm>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/Numeric.h>

#include <fad/engine/parameters/ParameterSnapshot.h>
#include <fad/engine/assets/WavetableData.h>
#include <fad/engine/modulation/CurveNodesRuntime.h>
#include <fad/engine/math/DspSafetyUtils.h>

namespace fadmodel1::dsp::oscillators
{
class OscillatorBlock final
{
    struct FmUnisonState
    {
        // Initialized only after FM admission, avoiding scratch work for other modes.
        alignas(16) std::array<float, 8> phases, modulatorPhases, modulators;
        alignas(16) std::array<float, 8> phaseDeltas, jitter, modulatorSubDeltas, previousOutputs;
        alignas(16) std::array<uint32_t, 8> jitterStates;
        int factor = 1;
        int count = 0;
        float index = 0.0f, feedback = 0.0f, level = 0.0f;
        bool nativeFma = false;
    };
public:
    void prepare(double newSampleRate);
    void reset() noexcept;
    void resetRandomState() noexcept;
    bool setFrequencyWithoutPhaseJitter(float newFrequencyHz) noexcept
    {
        const auto safeFrequency = std::max(
            0.0f, utils::isFinite(newFrequencyHz) ? newFrequencyHz : 0.0f);
        const auto frequencyChanged = safeFrequency < currentFrequencyHz
            || safeFrequency > currentFrequencyHz;
        if (frequencyChanged)
        {
            currentFrequencyHz = safeFrequency;
            phaseDelta = std::clamp(currentFrequencyHz * inverseSampleRate, 0.0f, 0.49f);
        }

        return frequencyChanged;
    }
    void advancePhaseJitter() noexcept
    {
        jitterState ^= jitterState << 13;
        jitterState ^= jitterState >> 17;
        jitterState ^= jitterState << 5;
        phaseJitter = static_cast<float>(static_cast<int32_t>(jitterState))
            * (1.0f / 2147483648.0f) * phaseDelta * 0.00005f;
    }
    bool setFrequency(float newFrequencyHz) noexcept
    {
        const auto frequencyChanged = setFrequencyWithoutPhaseJitter(newFrequencyHz);
        advancePhaseJitter();
        return frequencyChanged;
    }
    // Explicit phase retriggers also align stateful waveform generators. Native
    // phase memory restores its saved phase without discarding that history.
    void setPhase(float newPhase01, bool resetPhaseHistory = true) noexcept;
    float getPhase01() const noexcept { return phase; }
    void setNativeSyncWarp(bool enabled) noexcept { nativeSyncWarp = enabled; }
    void copyPhaseFrom(const OscillatorBlock& source, float phaseOffset01) noexcept
    {
        const auto sourcePhase = utils::isFinite(source.phase) ? source.phase : 0.0f;
        const auto offset = utils::isFinite(phaseOffset01) ? phaseOffset01 : 0.0f;
        const auto copiedPhase = sourcePhase + offset;
        setPhase(utils::isFinite(copiedPhase) ? copiedPhase : 0.0f);
    }
    void setNodeCurve(const modulation::BandLimitedNodeCurve* curve) noexcept { nodeCurve=curve; }
    void setExternalWavetable(const wavetable::WavetableData* table,
                              uint32_t generation) noexcept;
    void setWavetableMipLevel(float idealLevel) noexcept
    {
        wavetableMipLevel = utils::isFinite(idealLevel)
            ? idealLevel
            : std::numeric_limits<float>::quiet_NaN();
    }

    // Preview is phase-preserving: both sources are captured before either carrier advances.
    std::array<float, 4> previewCrossSource(const parameters::OscillatorSnapshot& snapshot) noexcept;
    void setCrossSource(const std::array<float, 4>& samples) noexcept { crossSourceSamples = samples; }
    float renderSample(const parameters::OscillatorSnapshot& snapshot) noexcept;
    float renderStaticWavetableSample(
        const parameters::OscillatorSnapshot& snapshot) noexcept;
    static bool renderStaticWavetableUnison(
        OscillatorBlock& primary,
        OscillatorBlock* extraOscillators,
        int voiceCount,
        const parameters::OscillatorSnapshot& snapshot,
        float* outputSamples) noexcept;
    static bool renderStaticFmUnison(
        OscillatorBlock& primary,
        OscillatorBlock* extraOscillators,
        int voiceCount,
        const parameters::OscillatorSnapshot& snapshot,
        float* outputSamples) noexcept;

    // Stack-scoped renderer for one span with fixed snapshot, frequency and
    // assets. Advance jitter through this span; flush before accessing or changing
    // oscillator state. Destruction flushes automatically. A failed admission still uses the normal renderer,
    // allowing mode/asset fades to finish before a later sample is admitted.
    class StaticUnisonRenderSpan final
    {
    public:
        StaticUnisonRenderSpan(OscillatorBlock& primaryIn, OscillatorBlock* extrasIn,
                              int countIn, const parameters::OscillatorSnapshot& snapshotIn) noexcept
            : primary(primaryIn), extras(extrasIn), count(countIn), snapshot(snapshotIn) {}
        ~StaticUnisonRenderSpan() { flush(); }
        StaticUnisonRenderSpan(const StaticUnisonRenderSpan&) = delete;
        StaticUnisonRenderSpan& operator=(const StaticUnisonRenderSpan&) = delete;
        void advancePhaseJitter() noexcept;
        bool render(float* outputSamples) noexcept;
        void flush() noexcept;
    private:
        FmUnisonState fmState;
        bool fmActive = false;
        OscillatorBlock& primary;
        OscillatorBlock* extras;
        int count;
        const parameters::OscillatorSnapshot& snapshot;
        bool admitted = false;
    };

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    bool testGetNativeFmFmaAvailable() const noexcept { return nativeFmFmaAvailable; }
    void testDisableNativeFmFma() noexcept { nativeFmFmaAvailable = false; }
    float testGetPhase01() const noexcept { return phase; }
    float testGetCurrentFrequencyHz() const noexcept { return currentFrequencyHz; }
    float testGetModulatorPhase01() const noexcept { return modulatorPhase; }
    float testGetLastFmModulatorSample() const noexcept { return lastFmModulatorSample; }
    int testGetFmOversampleFactor() const noexcept { return fmOversampleFactor; }
    int testGetWarpOversampleFactor() const noexcept { return warpOversampleFactor; }
    void testSetFmOversampleFactor(int factor) noexcept
    {
        fmOversampleFactor = std::clamp(factor, 1, 4);
    }
    void testSetWarpOversampleFactor(int factor) noexcept
    {
        warpOversampleFactor = std::clamp(factor, 1, 4);
        warpOversampleMipOffset = std::log2(static_cast<float>(warpOversampleFactor));
    }
    void testSetNeutralFmFastPathEnabled(bool enabled) noexcept
    {
        neutralFmFastPathEnabled = enabled;
    }
    void testSetFastFmCarrierEnabled(bool enabled) noexcept
    {
        fastFmCarrierEnabled = enabled;
    }
    void testSetFastWarpPowEnabled(bool enabled) noexcept
    {
        fastWarpPowEnabled = enabled;
    }
    void testSetStaticWavetableFastPathEnabled(bool enabled) noexcept
    {
        staticWavetableFastPathEnabled = enabled;
    }
#endif

private:
    static bool renderStaticWavetableUnisonImpl(OscillatorBlock&, OscillatorBlock*, int,
        const parameters::OscillatorSnapshot&, float*, bool checkAdmission) noexcept;
    static bool prepareStaticFmUnison(OscillatorBlock&, OscillatorBlock*, int,
        const parameters::OscillatorSnapshot&, FmUnisonState&) noexcept;
    static void renderPreparedFmUnison(FmUnisonState&, float*) noexcept;
    static void commitFmUnison(OscillatorBlock&, OscillatorBlock*, const FmUnisonState&) noexcept;
    bool nativeFmFmaAvailable = false;
    void invalidateWavetableSamplePlan() noexcept;
    float renderWavetableSample(const wavetable::WavetableData& table,
                                float position,
                                float phaseValue,
                                float phaseDeltaValue,
                                float idealMipLevel,
                                float warp,
                                parameters::WarpMode warpMode,
                                int nativeWarpMode = -1) noexcept;
    float renderPreparedBendWarpSample(float phaseValue,
                                       float phaseDeltaValue,
                                       parameters::WarpMode warpMode) noexcept;

    float renderCrossWavetable(const wavetable::WavetableData& table,
                               const parameters::OscillatorSnapshot& snapshot) noexcept;
    std::array<float, 4> crossSourceSamples{};
    const modulation::BandLimitedNodeCurve* nodeCurve=nullptr;
    double sampleRate = 0.0;
    float inverseSampleRate = 0.0f;
    float phase = 0.0f;
    float phaseDelta = 0.0f;
    float currentFrequencyHz = 0.0f;
    float modulatorPhase = 0.0f;
    float modulatorPhaseDelta = 0.0f;
    float preparedFmPhaseDelta = std::numeric_limits<float>::quiet_NaN();
    float preparedFmRatio = std::numeric_limits<float>::quiet_NaN();
    float lastFmModulatorSample = 0.0f;
    float triangleIntegrator = 0.0f;
    float triangleLeakCoeff = 0.995f;
    bool triangleNeedsAlignment = true;
    const wavetable::WavetableData* externalWavetable = nullptr;
    uint32_t externalWavetableGeneration = 0;
    float wavetableMipLevel = std::numeric_limits<float>::quiet_NaN();
    const wavetable::WavetableData* preparedWavetable = nullptr;
    float preparedWavetablePosition = std::numeric_limits<float>::quiet_NaN();
    float preparedWavetableMipLevel = std::numeric_limits<float>::quiet_NaN();
    bool nativeSyncWarp = false;
    float preparedWarp = std::numeric_limits<float>::quiet_NaN();
    float preparedWarpGain = 1.0f;
    float preparedWarpMakeup = 1.0f;
    float preparedBendPlusExponent = 1.0f;
    float preparedBendMinusExponent = 1.0f;
    wavetable::WavetableData::PreparedBandLimitedSample preparedWavetableSample;
    bool hasPreparedWavetableSample = false;
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    bool staticWavetableFastPathEnabled = true;
    bool neutralFmFastPathEnabled = true;
    bool fastFmCarrierEnabled = true;
    bool fastWarpPowEnabled = true;
#endif
    int wavetableFadeSamples = 88;
    int wavetableFadeRemaining = 0;
    float wavetableFadeStartSample = 0.0f;
    float lastWavetableSample = 0.0f;
    bool hasRenderedWavetableSample = false;
    parameters::OscillatorMode activeMode = parameters::OscillatorMode::analog;
    parameters::AnalogWave activeAnalogWave = parameters::AnalogWave::sine;
    int modeFadeRemaining = 0;
    float modeFadeStartSample = 0.0f;
    float lastOutputSample = 0.0f;
    bool hasRenderedSample = false;

    // Independent absolute-rate policies for recursive FM and wavetable warp.
    int fmOversampleFactor = 4;
    int warpOversampleFactor = 3;
    float warpOversampleMipOffset = 1.584962500721156f;
    float fmOversamplePrevOut = 0.0f;

    // Analog imperfections
    float phaseJitter = 0.0f;     // accumulated phase noise
    static constexpr uint32_t initialJitterState = 0x12345678u;
    uint32_t jitterState = initialJitterState;
};
}
