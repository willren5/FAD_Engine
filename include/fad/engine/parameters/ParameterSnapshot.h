#pragma once
#include "ParameterTypes.h"
#include "CurveNodes.h"
#include "FxRouting.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fadmodel1::parameters
{
inline float effectiveEnvelopeStageSeconds(float visibleSeconds, float encoded, bool serumAmplitude) noexcept
{
    if (!serumAmplitude || !std::isfinite(encoded) || encoded <= 0.0f) return visibleSeconds;
    const auto sourceMs = std::clamp(encoded * 32001.0f - 1.0f, 0.0f, 32000.0f);
    const auto visibleMs = std::clamp(sourceMs, 1.0f, 10000.0f);
    return (sourceMs < 1.0f || sourceMs > 10000.0f)
        && std::abs(visibleSeconds * 1000.0f - visibleMs) < .01f ? sourceMs * .001f : visibleSeconds;
}

struct EnvelopeSnapshot
{
    float attackSeconds = 0.01f;
    float decaySeconds = 0.25f;
    float sustainLevel = 1.0f;
    float releaseSeconds = 0.3f;
    float holdSeconds = 0.0f;
    std::array<float, 3> curves{};
};

struct OscillatorSnapshot
{
    bool enabled = true;
    bool pitchTracking = true;
    OscillatorMode mode = OscillatorMode::analog;
    float timbre = 0.5f;
    float level = 0.0f;
    float pan = 0.0f;
    int octave = 0;
    int semitone = 0;
    float fineCents = 0.0f;
    float nativeCoarseSemitones = 0.0f;
    float nativeOctavePosition = -1.0f;
    float detuneCents = 0.0f;
    int unisonVoices = 1;
    float unisonWidth = 1.0f;
    float unisonBlend = 0.5f;
    float unisonRand = 0.25f;
    bool extendedUnison = false;
    float phaseDegrees = 0.0f;
    float drift = 0.05f;

    AnalogWave analogWave = AnalogWave::sine;
    float analogShape = 0.5f;
    float analogPulseWidth = 0.5f;

    float wavetablePosition = 0.0f;
    float wavetableWarp = 0.0f;
    int wavetableSlot = 0;
    WarpMode warpMode = WarpMode::phaseDist;
    int crossMode = 0; // 0 local warp, 1 FM, 2 AM, 3 RM from the other oscillator
    int nativeWarpMode = -1; // Source identity; -1 preserves the legacy warp path.

    float fmRatio = 1.0f;
    float fmIndex = 0.0f;
    float fmFeedback = 0.0f;
    float fmLevel = 0.5f;
    bool wideFmIndex = false;
};

struct FilterSnapshot
{
    CurveNodes curve;
    bool enabled = true;
    float mix = 1.0f;
    // A, B, sub, noise. All-routed/full-wet preserves the legacy sound.
    std::array<bool, 4> inputs{{true, true, true, true}};
    FilterMode mode = FilterMode::lowPass;
    float cutoffHz = 18000.0f;
    float resonance = 0.1f;
    float driveDb = 0.0f;
    float keyTrack = 0.0f;
    float envAmount = 0.0f;
    // Rounded Serum filter ID 0..95; -1 preserves legacy FAD behavior.
    int nativeModel = -1;
    float stereoOffsetSemitones = 0.0f;
    float nativeVariation = 0.5f;
    float nativeCutoffHz = 0.0f;
};

struct LfoSnapshot
{
    int triggerMode = 0; // 0 global/free, 1 per-note loop, 2 per-note one-shot
    bool anchor = true;
    int curveSlot = 0;
    float extendedRateHz = -1.0f;
    float cycleBeats = 0.0f;
    float rateHz = 1.0f;
    float depth = 1.0f;
    LfoShape shape = LfoShape::sine;
    bool syncToTempo = false;
    int nativeTiming = 0;
};

struct DriveSnapshot
{
    bool enabled = false;
    DriveMode mode = DriveMode::softClip;
    float amount = 0.0f;
    float tone = 0.55f;
    float mix = 0.0f;
};

struct ChorusSnapshot
{
    bool enabled = false;
    float rateHz = 0.35f;
    float depth = 0.18f;
    float delayMs = 7.5f;
    float feedback = 0.08f;
    float mix = 0.0f;
    float highCutHz = 18000.0f;
    float lowCutHz = 20.0f;
};

struct DelaySnapshot
{
    bool enabled = false;
    int timeIndex = 6;
    float feedback = 0.25f;
    float highCutHz = 12000.0f;
    float lowCutHz = 120.0f;
    float mix = 0.0f;
};

struct ReverbSnapshot
{
    bool enabled = false;
    float size = 0.35f;
    float damping = 0.45f;
    float wet = 0.0f;
    float width = 0.85f;
    float highCutHz = 16000.0f;
    float lowCutHz = 20.0f;
    float preDelayMs = 0.0f;
};

struct FxSnapshot
{
    DriveSnapshot drive;
    ChorusSnapshot chorus;
    DelaySnapshot delay;
    ReverbSnapshot reverb;
};

inline constexpr std::array<int, ids::maxFxSlots> defaultDynamicFxTypes{
    1, 2, 3, 4, 0, 0, 0, 0};

inline constexpr int defaultDynamicFxTypeForSlot(int slotIndex) noexcept
{
    return slotIndex >= 0 && slotIndex < static_cast<int>(defaultDynamicFxTypes.size())
        ? defaultDynamicFxTypes[static_cast<size_t>(slotIndex)]
        : 0;
}

struct GenericFxSlotSnapshot
{
    int type = 0;           // FxType as int (0=none, 1=drive, 2=chorus, etc.)
    bool enabled = false;
    int order = 0;          // position in processing chain
    int routingStage = -1;  // absent legacy fields resolve to the physical slot
    int routingLane = 0;
    std::array<float, ids::maxFxParamsPerSlot> params{};  // normalized 0-1 values
    float wideRateHz=-1, wideLeftMs=-1, wideRightMs=-1;
};

struct DynamicFxSnapshot
{
    bool hasExplicitTopology = false;
    int slotCount = 4;
    std::array<GenericFxSlotSnapshot, ids::maxFxSlots> slots;
};

inline std::array<int, ids::maxFxSlots> sanitizeDynamicFxSlotOrder(
    const DynamicFxSnapshot& dynamicFx,
    int activeSlotCount) noexcept
{
    std::array<int, ids::maxFxSlots> sanitized{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::array<bool, ids::maxFxSlots> used{};
    const auto clampedCount = std::clamp(activeSlotCount, 0, ids::maxFxSlots);
    int writeIndex = 0;

    for (int orderRank = 0; orderRank < clampedCount; ++orderRank)
    {
        for (int slotIndex = 0; slotIndex < clampedCount; ++slotIndex)
        {
            if (used[static_cast<size_t>(slotIndex)]
                || dynamicFx.slots[static_cast<size_t>(slotIndex)].order != orderRank)
                continue;

            sanitized[static_cast<size_t>(writeIndex++)] = slotIndex;
            used[static_cast<size_t>(slotIndex)] = true;
        }
    }

    for (int slotIndex = 0; slotIndex < clampedCount; ++slotIndex)
        if (! used[static_cast<size_t>(slotIndex)])
            sanitized[static_cast<size_t>(writeIndex++)] = slotIndex;

    for (int slotIndex = clampedCount; slotIndex < ids::maxFxSlots; ++slotIndex)
        sanitized[static_cast<size_t>(slotIndex)] = slotIndex;

    return sanitized;
}

inline FxRoutingPlan buildDynamicFxRoutingPlan(const DynamicFxSnapshot& dynamicFx, int activeSlotCount) noexcept
{
    std::array<int, ids::maxFxSlots> stages{}, lanes{};
    uint32_t configuredMask = 0;
    for (int i = 0; i < ids::maxFxSlots; ++i)
    {
        const auto& slot = dynamicFx.slots[static_cast<size_t>(i)];
        stages[static_cast<size_t>(i)] = resolvedFxRoutingStage(i, slot.routingStage);
        lanes[static_cast<size_t>(i)] = slot.routingLane;
        if (slot.type != 0) configuredMask |= uint32_t{1} << i;
    }
    return buildFxRoutingPlan(sanitizeDynamicFxSlotOrder(dynamicFx, activeSlotCount), activeSlotCount, stages, lanes, configuredMask);
}

inline std::array<int, ids::maxFxSlots> buildDynamicFxProcessingRanks(
    const DynamicFxSnapshot& dynamicFx,
    int activeSlotCount) noexcept
{
    std::array<int, ids::maxFxSlots> ranks{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto count = std::clamp(activeSlotCount, 0, ids::maxFxSlots);
    bool defaultSerial = true;
    for (int i = 0; i < count; ++i)
    {
        const auto& slot = dynamicFx.slots[static_cast<size_t>(i)];
        defaultSerial = defaultSerial && resolvedFxRoutingStage(i, slot.routingStage) == i && slot.routingLane == 0;
    }
    if (defaultSerial)
    {
        const auto order = sanitizeDynamicFxSlotOrder(dynamicFx, count);
        for (int i = 0; i < count; ++i) ranks[static_cast<size_t>(order[static_cast<size_t>(i)])] = i;
        return ranks;
    }
    const auto routing = buildDynamicFxRoutingPlan(dynamicFx, count);

    for (int processingIndex = 0; processingIndex < routing.slotCount; ++processingIndex)
    {
        const auto slotIndex = routing.flattenedOrder[static_cast<size_t>(processingIndex)];
        if (slotIndex >= 0 && slotIndex < ids::maxFxSlots)
            ranks[static_cast<size_t>(slotIndex)] = processingIndex;
    }

    return ranks;
}

struct ModMatrixSlotSnapshot
{
    ModSource source = ModSource::none;
    ModDestination destination = ModDestination::none;
    float amount = 0.0f;
    int nativeTarget = -1;
    int nativeSource = -1;
    int nativeAuxSource = 0;
    int nativeFlags = 0;
    bool fadSemantics = false;
    bool enabled = true;
    bool widePitch = false, audioRate = false;
    // Derived row identity, never a host parameter or stored schema field.
    int midiLearnSlot = 0;
    bool hasSource() const noexcept { return enabled && (nativeSource>=0 ? nativeSource>0 : source!=ModSource::none); }
    bool hasEffectiveAmount(bool preserveSmallAmounts = false) const noexcept
    {
        return std::isfinite(amount)
            && ((nativeTarget >= 0 || preserveSmallAmounts)
                    ? amount != 0.0f : std::abs(amount) >= 0.0001f);
    }
};

struct ModulationSnapshot
{
    std::array<LfoSnapshot, lfoCount> lfos;
    std::array<ModMatrixSlotSnapshot, modMatrixSlotCount> slots;

    bool hasActiveRoutes(bool preserveSmallAmounts = false) const noexcept
    {
        for (const auto& slot : slots)
        {
            if (slot.hasSource()
                && (slot.destination != ModDestination::none || slot.nativeTarget >= 0)
                && slot.hasEffectiveAmount(preserveSmallAmounts))
            {
                return true;
            }
        }

        return false;
    }
};

struct GlobalSnapshot
{
    bool serumAmplitude = false;
    bool vitalAmplitude = false;
    bool nativeMonoRetrigger = false;
    float presetGain = 1.0f;
    float nativeTuneSemitones = 0.0f;
    float outputGainDb = -6.0f;
    float outputGainLinear = 0.501187f;
    VoiceMode voiceMode = VoiceMode::poly;
    float pitchBendRange = 2.0f;  // semitones, 1-24
    float velocityCurve = 0.5f; // 0.5 preserves the linear legacy response
    float velocitySensitivity = 1.0f;
    bool mpeEnabled = false;
    float mpePitchBendRange = 48.0f;
    float glideTimeMs = 0.0f;     // legacy control 0-2000 ms; imported duration up to 8000 ms
    GlideMode glideMode = GlideMode::off;
    bool nativeGlide = false;
    bool nativeGlideScaled = false;
    float nativeGlideCurve = 0.5f;
};

struct LiveCaptureSnapshot
{
    bool enabled = false;
    int triggerMode = 0;
    float thresholdDb = -24.0f;
    int budgetLevel = 1;
    int resynthMode = 0;
    float lengthSeconds = 1.0f;
    int inputSource = 0;
};

struct NoiseSnapshot
{
    bool enabled = true;
    int sampleSlot = 0;
    float pitch = 0.5f, fine = 0.5f, pan = 0.5f, phase = 0.0f, randomPhase = 0.0f;
    bool oneShot = false, keytrack = false, directOut = false;
};

struct ParameterSnapshot
{
    ParameterSnapshot() noexcept;

    std::array<CurveNodes,curveOwnerCount> curves{};
    GlobalSnapshot global;
    FxSnapshot fx;
    std::array<OscillatorSnapshot, 2> oscillators;
    SubType subType = SubType::square;
    int subNativeShape = -1;
    float subLevel = 0.0f;
    bool subEnabled = true;
    bool subDirectOut = false;
    float subPan = 0.0f;
    int subOctave = -1;
    float noiseLevel = 0.0f;
    bool noiseWideLevel = false;
    NoiseSnapshot noise;
    FilterSnapshot filter;
    EnvelopeSnapshot ampEnvelope;
    EnvelopeSnapshot filterEnvelope;
    EnvelopeSnapshot modEnvelope;
    ModulationSnapshot modulation;
    std::array<float, 8> macroValues{};
    DynamicFxSnapshot dynamicFx;
    LiveCaptureSnapshot liveCapture;
    uint32_t wavetableAssetGeneration = 0;
};

}
