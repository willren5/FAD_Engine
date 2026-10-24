#pragma once
#include <array>
#include <span>
#include <fad/engine/math/RuntimeRange.h>
#include <fad/engine/math/DspSafetyUtils.h>

namespace fadmodel1::dsp::fx
{
enum class FxType : int
{
    none = 0, drive, chorus, delay, reverb,
    eq3Band, compressor, phaser, flanger,
    distortion, bitcrusher, filterFx, stereo, limiter
};

namespace catalog_detail
{
inline constexpr const char* driveNames[] = {"Amount", "Tone", "Mix", "Mode"};
inline constexpr const char* chorusNames[] = {"Rate", "Depth", "Delay", "Feedback", "Mix", "Hi Cut", "Lo Cut", "Serum Mix", "Serum Delay 2", "Serum Depth", "Serum Dual", "Serum Phase"};
inline constexpr const char* delayNames[] = {"Time", "Feedback", "Hi Cut", "Lo Cut", "Mix", "Time Mode", "Left Time", "Right Time", "Same Channel Feedback", "Serum Filter Hz", "Serum Bandwidth", "Ping Pong"};
inline constexpr const char* reverbNames[] = {"Size", "Damping", "Wet", "Width", "Hi Cut", "Lo Cut", "Pre-Delay", "Native Decay", "Hall Decay", "Hall HiCut", "Hall LoCut", "Hall Spin Rate", "Hall Spin Depth", "Mode", "Pre-Delay"};
inline constexpr const char* eq3BandNames[] = {"Low Freq", "Low Gain", "Mid Freq", "Mid Gain", "Mid Q", "High Freq", "High Gain", "Native EQ", "Native Low Frequency", "Native High Frequency", "Native Low Q", "Native High Q", "Native Low Gain", "Native High Gain", "Native Low Type", "Native High Type"};
inline constexpr const char* compressorNames[] = {"Threshold", "Ratio", "Attack", "Release", "Makeup", "Mix", "Knee", "Native Model", "Native Threshold", "Native Ratio", "Native Attack", "Native Release", "Native Makeup", "Low Threshold", "Mid Threshold", "High Threshold"};
inline constexpr const char* phaserNames[] = {"Rate", "Depth", "Center", "Feedback", "Stages", "Mix", "Stereo", "Serum Mix", "Native Phaser", "Native Rate", "Native Frequency", "Native Feedback", "Native Stereo", "Native Cycle Beats"};
inline constexpr const char* flangerNames[] = {"Rate", "Depth", "Manual", "Feedback", "Mix", "Hi Cut", "Lo Cut", "Serum Mix"};
inline constexpr const char* distortionNames[] = {"Amount", "Tone", "Mix", "Mode", "Pre Filter", "Post Filter", "Bias", "Serum SoftClip", "Serum Zero-Square", "Serum Post HP", "Serum Post Freq", "Serum Tube", "Serum Asym", "Serum Downsample", "Serum Filter Position", "Serum Filter Morph", "Serum Filter Bandwidth", "Serum Diode 2"};
inline constexpr const char* bitcrusherNames[] = {"Bits", "Rate", "Mix", "Jitter", "Drive"};
inline constexpr const char* filterFxNames[] = {"Cutoff", "Resonance", "Mode", "Drive", "Hi Cut", "Lo Cut", "Native Model", "Variation", "Stereo Cutoff", "Native Mix", "Native Level", "Extended Resonance", "Extended Cutoff"};
inline constexpr const char* stereoNames[] = {"Width", "Pan", "Mid Gain", "Side Gain", "Mix", "Native Hyper/Dimension", "Hyper Wet", "Hyper Rate", "Hyper Detune", "Hyper Voices", "Hyper Phase L1", "Hyper Phase L2", "Hyper Phase L3", "Hyper Phase L4", "Hyper Phase L5", "Hyper Phase L6", "Hyper Phase L7", "Hyper Phase R1", "Hyper Phase R2", "Hyper Phase R3", "Hyper Phase R4", "Hyper Phase R5", "Hyper Phase R6", "Hyper Phase R7", "Dimension Size", "Dimension Mix"};
inline constexpr const char* limiterNames[] = {"Ceiling", "Release", "Lookahead", "Knee", "Mix"};
inline constexpr const char* driveIds[] = {"amount", "tone", "mix", "mode"};
inline constexpr const char* chorusIds[] = {"rate", "depth", "delay", "feedback", "mix", "hiCut", "loCut", "serumMix", "serumDelay2", "serumDepth", "serumDual", "serumPhase"};
inline constexpr const char* delayIds[] = {"time", "feedback", "hiCut", "loCut", "mix", "timeMode", "timeLeft", "timeRight", "sameChannelFeedback", "serumFilterFrequency", "serumFilterBandwidth", "pingPong"};
inline constexpr const char* reverbIds[] = {"size", "damping", "wet", "width", "hiCut", "loCut", "preDelay", "nativeDecay", "nativeHallDecay", "nativeHallHiCut", "nativeHallLoCut", "nativeHallSpinRate", "nativeHallSpinDepth", "mode", "nativePreDelay"};
inline constexpr const char* eq3BandIds[] = {"lowFreq", "lowGain", "midFreq", "midGain", "midQ", "highFreq", "highGain", "nativeMode", "nativeLowFreq", "nativeHighFreq", "nativeLowQ", "nativeHighQ", "nativeLowGain", "nativeHighGain", "nativeLowType", "nativeHighType"};
inline constexpr const char* compressorIds[] = {"threshold", "ratio", "attack", "release", "makeup", "mix", "knee", "nativeMode", "nativeThreshold", "nativeRatio", "nativeAttack", "nativeRelease", "nativeMakeup", "nativeLowThreshold", "nativeMidThreshold", "nativeHighThreshold"};
inline constexpr const char* phaserIds[] = {"rate", "depth", "center", "feedback", "stages", "mix", "stereo", "serumMix", "nativeProfile", "nativeRate", "nativeFrequency", "nativeFeedback", "nativeStereo", "nativeCycleBeats"};
inline constexpr const char* flangerIds[] = {"rate", "depth", "manual", "feedback", "mix", "hiCut", "loCut", "serumMix"};
inline constexpr const char* distortionIds[] = {"amount", "tone", "mix", "mode", "preFilter", "postFilter", "bias", "serumSoftClip", "serumZeroSquare", "serumPostHP", "serumPostFreq", "serumTube", "serumAsym", "serumDownsample", "serumFilterPosition", "serumFilterMorph", "serumFilterBandwidth", "serumDiode2"};
inline constexpr const char* bitcrusherIds[] = {"bits", "rate", "mix", "jitter", "drive"};
inline constexpr const char* filterFxIds[] = {"cutoff", "resonance", "mode", "drive", "hiCut", "loCut", "nativeModel", "nativeVariation", "nativeStereo", "nativeMix", "nativeLevel", "nativeResonance", "nativeCutoff"};
inline constexpr const char* stereoIds[] = {"width", "pan", "midGain", "sideGain", "mix", "nativeHyper", "hyperWet", "hyperRate", "hyperDetune", "hyperVoices", "hyperPhaseL1", "hyperPhaseL2", "hyperPhaseL3", "hyperPhaseL4", "hyperPhaseL5", "hyperPhaseL6", "hyperPhaseL7", "hyperPhaseR1", "hyperPhaseR2", "hyperPhaseR3", "hyperPhaseR4", "hyperPhaseR5", "hyperPhaseR6", "hyperPhaseR7", "dimensionSize", "dimensionMix"};
inline constexpr const char* limiterIds[] = {"ceiling", "release", "lookahead", "knee", "mix"};
} // namespace catalog_detail

class FxCatalog
{
public:
    static constexpr int getParamCountForType(FxType) noexcept;
    static constexpr std::span<const char* const> getParamNamesViewForType(FxType) noexcept;
    static constexpr std::span<const char* const> getParamIdsViewForType(FxType) noexcept;
    static fad::engine::math::RuntimeRange range(FxType, int) noexcept;
    static float normalizeParamValue(FxType, int, float) noexcept;
    static float denormalizeParamValue(FxType, int, float) noexcept;
    static bool isHostRateEndpoint(FxType, int, float) noexcept;
    static bool isValidTypeValue(int value) noexcept
    { return value >= int(FxType::none) && value <= int(FxType::limiter); }
    static FxType sanitizeTypeValue(int value) noexcept
    { return isValidTypeValue(value) ? static_cast<FxType>(value) : FxType::none; }
};

inline constexpr int FxCatalog::getParamCountForType(FxType type) noexcept
{
    switch (type)
    {
        case FxType::drive:   return 4;
        case FxType::chorus:  return 12;
        case FxType::delay:   return 12;
        case FxType::reverb:  return 15;
        case FxType::eq3Band:     return 16;
        case FxType::compressor:  return 16;
        case FxType::phaser:      return 14;
        case FxType::flanger:     return 8;
        case FxType::distortion:  return 18;
        case FxType::bitcrusher:  return 5;
        case FxType::filterFx:    return 13;
        case FxType::stereo:      return 26;
        case FxType::limiter:     return 5;
        case FxType::none:
        default:              return 0;
    }
}

inline constexpr std::span<const char* const> FxCatalog::getParamNamesViewForType(FxType type) noexcept
{
    switch (type)
    {
        case FxType::drive: return catalog_detail::driveNames;
        case FxType::chorus: return catalog_detail::chorusNames;
        case FxType::delay: return catalog_detail::delayNames;
        case FxType::reverb: return catalog_detail::reverbNames;
        case FxType::eq3Band: return catalog_detail::eq3BandNames;
        case FxType::compressor: return catalog_detail::compressorNames;
        case FxType::phaser: return catalog_detail::phaserNames;
        case FxType::flanger: return catalog_detail::flangerNames;
        case FxType::distortion: return catalog_detail::distortionNames;
        case FxType::bitcrusher: return catalog_detail::bitcrusherNames;
        case FxType::filterFx: return catalog_detail::filterFxNames;
        case FxType::stereo: return catalog_detail::stereoNames;
        case FxType::limiter: return catalog_detail::limiterNames;
        case FxType::none:
        default: return {};
    }
}

inline constexpr std::span<const char* const> FxCatalog::getParamIdsViewForType(FxType type) noexcept
{
    switch (type)
    {
        case FxType::drive: return catalog_detail::driveIds;
        case FxType::chorus: return catalog_detail::chorusIds;
        case FxType::delay: return catalog_detail::delayIds;
        case FxType::reverb: return catalog_detail::reverbIds;
        case FxType::eq3Band: return catalog_detail::eq3BandIds;
        case FxType::compressor: return catalog_detail::compressorIds;
        case FxType::phaser: return catalog_detail::phaserIds;
        case FxType::flanger: return catalog_detail::flangerIds;
        case FxType::distortion: return catalog_detail::distortionIds;
        case FxType::bitcrusher: return catalog_detail::bitcrusherIds;
        case FxType::filterFx: return catalog_detail::filterFxIds;
        case FxType::stereo: return catalog_detail::stereoIds;
        case FxType::limiter: return catalog_detail::limiterIds;
        case FxType::none:
        default: return {};
    }
}

inline fad::engine::math::RuntimeRange FxCatalog::range(FxType type,
                                                                       int paramIndex) noexcept
{
    switch (type)
    {
        case FxType::drive:
            switch (paramIndex)
            {
                case 0: return {0.0f, 1.0f};
                case 1: return {0.0f, 1.0f};
                case 2: return {0.0f, 1.0f};
                case 3: return {0.0f, 2.0f, 1.0f};
                default: break;
            }
            break;

        case FxType::chorus:
            switch (paramIndex)
            {
                case 0: return {0.05f, 12.0f, 0.0f, 0.3f};
                case 1: return {0.0f, 1.0f};
                case 2: return {1.0f, 40.0f};
                case 3: return {-0.95f, 0.95f};
                case 4: return {0.0f, 1.0f};
                case 5: return {1000.0f, 20000.0f, 0.0f, 0.3f};
                case 6: return {20.0f, 4000.0f, 0.0f, 0.3f};
                case 7: return {0.0f, 1.0f, 1.0f};
                case 8: return {0.0f, 20.0f};
                case 9: return {0.0f, 62.5f};
                case 10: return {0.0f, 1.0f, 1.0f};
                case 11: return {-1.0f, 1.0f};
                default: break;
            }
            break;

        case FxType::delay:
            switch (paramIndex)
            {
                case 0: return {0.0f, 8.0f, 1.0f};
                case 1: return {0.0f, 0.95f};
                case 2: return {1000.0f, 20000.0f, 0.0f, 0.3f};
                case 3: return {20.0f, 4000.0f, 0.0f, 0.3f};
                case 4: return {0.0f, 1.0f};
                case 5: return {0.0f, 2.0f, 1.0f};
                case 6: case 7: return {0.0f, 16.0f};
                case 8: return {0.0f, 1.0f, 1.0f};
                case 9: return {0.0f, 18000.0f};
                case 10:return {0.0f, 1.0f};
                case 11:return {0.0f, 1.0f, 1.0f};
                default: break;
            }
            break;

        case FxType::reverb:
            switch (paramIndex)
            {
                case 0: return {0.0f, 1.0f};
                case 1: return {0.0f, 1.0f};
                case 2: return {0.0f, 1.0f};
                case 3: return {0.0f, 1.0f};
                case 4: return {1000.0f, 20000.0f, 0.0f, 0.3f};
                case 5: return {20.0f, 4000.0f, 0.0f, 0.3f};
                case 6: return {0.0f, 100.0f};
                case 7: return {0.0f, 12.0f};
                case 8: return {-1.0f, 1.0f}; // Negative values retain legacy original mode.
                case 14: return {-1.0f, 250.0f}; // Negative sentinel preserves legacy 0-100 ms automation.
                case 13: return {0.0f, 2.0f, 1.0f}; // Legacy, explicit Original, explicit Hall.
                case 9: case 10: case 11: case 12: return {0.0f, 1.0f};
                default: break;
            }
            break;

        case FxType::eq3Band:
            switch (paramIndex)
            {
                case 0: return {20.0f, 500.0f, 0.0f, 0.3f};       // Low Freq
                case 1: return {-12.0f, 12.0f};                     // Low Gain dB
                case 2: return {200.0f, 8000.0f, 0.0f, 0.3f};     // Mid Freq
                case 3: return {-12.0f, 12.0f};                     // Mid Gain dB
                case 4: return {0.1f, 10.0f, 0.0f, 0.5f};         // Mid Q
                case 5: return {2000.0f, 20000.0f, 0.0f, 0.3f};   // High Freq
                case 6: return {-12.0f, 12.0f};                     // High Gain dB
                case 7: return {0.0f, 1.0f, 1.0f};
                case 8: case 9: return {44100.0f / 2048.0f, 20000.0f, 0.0f, 0.3f};
                case 10: case 11: return {0.0f, 1.0f};
                case 12: case 13: return {-24.0f, 24.0f};
                case 14: case 15: return {0.0f, 2.0f, 1.0f};
                default: break;
            }
            break;

        case FxType::compressor:
            switch (paramIndex)
            {
                case 0: return {-60.0f, 0.0f};                     // Threshold dB
                case 1: return {1.0f, 20.0f, 0.0f, 0.3f};         // Ratio
                case 2: return {0.1f, 100.0f, 0.0f, 0.3f};        // Attack ms
                case 3: return {10.0f, 1000.0f, 0.0f, 0.3f};      // Release ms
                case 4: return {0.0f, 30.0f};                       // Makeup dB
                case 5: return {0.0f, 1.0f};                        // Mix
                case 6: return {0.0f, 20.0f};                       // Knee dB
                case 7: return {0.0f, 2.0f, 1.0f};
                case 8: return {-120.0f, 0.0f};
                case 9: return {0.0f, 1.0f};
                case 10: case 11: return {.1f, 1000.0f, 0.0f, .3f};
                case 12: return {0.0f, 30.10299957f};
                case 13: case 14: case 15: return {0.0f, 2.0f};
                default: break;
            }
            break;

        case FxType::phaser:
            switch (paramIndex)
            {
                case 0: return {0.01f, 10.0f, 0.0f, 0.3f};        // Rate Hz
                case 1: return {0.0f, 1.0f};                        // Depth
                case 2: return {200.0f, 8000.0f, 0.0f, 0.3f};     // Center Freq
                case 3: return {-0.95f, 0.95f};                     // Feedback
                case 4: return {0.0f, 3.0f, 1.0f};                 // Stages (discrete: 0=2, 1=4, 2=6, 3=8)
                case 5: return {0.0f, 1.0f};                        // Mix
                case 6: return {0.0f, 180.0f};                      // Stereo offset degrees
                case 7: return {0.0f, 1.0f, 1.0f};                 // Native wet law
                case 8: return {0.0f, 1.0f, 1.0f};                 // Native phaser profile
                case 9: return {0.0f, 20.0f};                      // Native free rate Hz
                case 10: case 11: case 12: return {0.0f, 1.0f};
                case 13: return {0.0f, 128.0f};                    // 0: free; otherwise beats/cycle
                default: break;
            }
            break;

        case FxType::flanger:
            switch (paramIndex)
            {
                case 0: return {0.01f, 10.0f, 0.0f, 0.3f};        // Rate Hz
                case 1: return {0.0f, 1.0f};                        // Depth
                case 2: return {0.1f, 10.0f, 0.0f, 0.3f};         // Manual ms
                case 3: return {-0.95f, 0.95f};                     // Feedback
                case 4: return {0.0f, 1.0f};                        // Mix
                case 5: return {1000.0f, 20000.0f, 0.0f, 0.3f};   // Hi Cut
                case 6: return {20.0f, 4000.0f, 0.0f, 0.3f};      // Lo Cut
                case 7: return {0.0f, 1.0f, 1.0f};                 // Native wet law
                default: break;
            }
            break;

        case FxType::distortion:
            switch (paramIndex)
            {
                case 0: return {0.0f, 1.0f};                        // Amount
                case 1: return {0.0f, 1.0f};                        // Tone
                case 2: return {0.0f, 1.0f};                        // Mix
                case 3: return {0.0f, 5.0f, 1.0f};                  // Mode (discrete 0-5)
                case 4: return {200.0f, 8000.0f, 0.0f, 0.3f};      // Pre Filter
                case 5: return {1000.0f, 20000.0f, 0.0f, 0.3f};    // Post Filter
                case 6: return {-1.0f, 1.0f};                        // Bias
                case 7: case 8: return {0.0f, 1.0f, 1.0f};
                case 9: case 11: case 12: case 13: case 17: return {0.0f, 1.0f, 1.0f};
                case 10: return {0.0f, 1.0f};
                case 14: return {0.0f, 2.0f, 1.0f};
                case 15: case 16: return {0.0f, 1.0f};
                default: break;
            }
            break;

        case FxType::bitcrusher:
            switch (paramIndex)
            {
                case 0: return {1.0f, 16.0f, 1.0f};                 // Bits (discrete)
                case 1: return {500.0f, 44100.0f, 0.0f, 0.3f};     // Rate Hz
                case 2: return {0.0f, 1.0f};                        // Mix
                case 3: return {0.0f, 1.0f};                        // Jitter
                case 4: return {0.0f, 30.0f};                       // Drive dB
                default: break;
            }
            break;

        case FxType::filterFx:
            switch (paramIndex)
            {
                case 0: return {20.0f, 20000.0f, 0.0f, 0.3f};     // Cutoff
                case 1: return {0.1f, 10.0f, 0.0f, 0.5f};         // Resonance
                case 2: return {0.0f, 2.0f, 1.0f};                  // Mode (discrete LP/BP/HP)
                case 3: return {0.0f, 30.0f};                       // Drive dB
                case 4: return {1000.0f, 20000.0f, 0.0f, 0.3f};   // Hi Cut
                case 5: return {20.0f, 4000.0f, 0.0f, 0.3f};      // Lo Cut
                case 6: return {-1.0f, 95.0f, 1.0f};               // -1 legacy
                case 7: return {0.0f, 1.0f};                        // Native variation
                case 8: return {-32.0f, 32.0f};                     // Stereo cutoff offset
                case 9: return {0.0f, 1.0f};                        // Native wet mix
                case 10: return {0.0f, 1.0f};                       // Native level control
                case 11: return {-1.0f, 1.0f};                      // -1 uses Q control
                case 12: return {0.0f, 22050.0f, 0.0f, 0.3f};       // 0 uses Cutoff
                default: break;
            }
            break;

        case FxType::stereo:
            switch (paramIndex)
            {
                case 0: return {0.0f, 2.0f};                        // Width (0=mono, 1=normal, 2=wide)
                case 1: return {-1.0f, 1.0f};                       // Pan
                case 2: return {-12.0f, 12.0f};                     // Mid Gain dB
                case 3: return {-12.0f, 12.0f};                     // Side Gain dB
                case 4: return {0.0f, 1.0f};                        // Mix
                case 5: return {0.0f, 1.0f, 1.0f};
                case 6: case 7: case 8: return {0.0f, 1.0f};
                case 9: return {0.0f, 7.0f, 1.0f};
                case 10: case 11: case 12: case 13: case 14: case 15: case 16:
                case 17: case 18: case 19: case 20: case 21: case 22: case 23:
                    return {-1.0f, 1.0f}; // -1 retains the legacy initial phase.
                default: break;
            }
            break;

        case FxType::limiter:
            switch (paramIndex)
            {
                case 0: return {-12.0f, 0.0f};                      // Ceiling dB
                case 1: return {10.0f, 500.0f, 0.0f, 0.3f};       // Release ms
                case 2: return {0.0f, 5.0f};                        // Lookahead ms
                case 3: return {0.0f, 6.0f};                        // Knee dB
                case 4: return {0.0f, 1.0f};                        // Mix
                default: break;
            }
            break;

        case FxType::none:
        default:
            break;
    }

    return {0.0f, 1.0f};
}

inline float FxCatalog::normalizeParamValue(FxType type, int paramIndex, float plainValue) noexcept
{
    const auto mapping = range(type, paramIndex);
    const auto safePlainValue = fadmodel1::dsp::utils::sanitizeFinite(plainValue, mapping.start);
    const auto legalValue = fadmodel1::dsp::utils::sanitizeFinite(
        mapping.snap(safePlainValue),
        mapping.start);
    const auto normalizedValue = fadmodel1::dsp::utils::sanitizeFinite(
        mapping.normalise(legalValue),
        0.0f);
    return std::clamp(normalizedValue, 0.0f, 1.0f);
}

inline float FxCatalog::denormalizeParamValue(FxType type, int paramIndex, float normalizedValue) noexcept
{
    const auto mapping = range(type, paramIndex);
    const auto safeNormalizedValue = fadmodel1::dsp::utils::sanitizeFinite(normalizedValue, 0.0f);
    const auto legalValue = mapping.denormalise(std::clamp(safeNormalizedValue, 0.0f, 1.0f));
    return fadmodel1::dsp::utils::sanitizeFinite(mapping.snap(legalValue), mapping.start);
}

inline bool FxCatalog::isHostRateEndpoint(FxType type,
                                           int paramIndex,
                                           float normalizedValue) noexcept
{
    return type == FxType::bitcrusher
        && paramIndex == 1
        && fadmodel1::dsp::utils::sanitizeUnitInterval(normalizedValue) >= 1.0f - 1.0e-6f;
}

}
