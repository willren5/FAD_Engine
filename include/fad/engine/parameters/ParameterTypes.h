#pragma once
#include <array>
#include <cstddef>

namespace fadmodel1::parameters
{
inline constexpr int legacyParameterVersionHint = 1;
inline constexpr int parameterVersionHint = 2;
inline constexpr int defaultVoiceCount = 16;
inline constexpr size_t legacyLfoCount = 4;
inline constexpr size_t lfoCount = 8;
inline constexpr float maxLegacyLfoExtendedRateHz = 100.0f;
inline constexpr float maxLfoFreeRateHz = 192.0f;
inline constexpr float minLfoWideRateHz = 0.0001f;
inline constexpr float maxLfoWideRateHz = 20000.0f;
inline constexpr size_t legacyModMatrixSlotCount = 8;
inline constexpr size_t modMatrixSlotCount = 32;
inline constexpr int maxUnisonVoiceCount = 7; // Legacy host parameter range.
inline constexpr int maxRenderedUnisonVoiceCount = 16;
inline constexpr int reservedParameterCount = 50;

namespace ids
{
inline constexpr int legacyMaxFxSlots = 8;
inline constexpr int maxFxSlots = 10;
inline constexpr int legacyFxParamsPerSlot = 8;
inline constexpr int previousFxParamsPerSlot = 16; // Keep existing host parameter positions.
inline constexpr int hyperFxParamsPerSlot = 24;
inline constexpr int maxFxParamsPerSlot = 26;
}

enum class VoiceMode : int
{
    poly = 0,
    mono
};

enum class GlideMode : int
{
    off = 0,
    always,
    legato
};

enum class DriveMode : int
{
    softClip = 0,
    hardClip,
    tube
};

enum class OscillatorMode : int
{
    analog = 0,
    wavetable,
    fm2Op
};

enum class AnalogWave : int
{
    sine = 0,
    saw,
    square,
    triangle
};

enum class SubType : int
{
    sine = 0,
    square,
    triangle
};

enum class FilterMode : int
{
    lowPass = 0,
    bandPass,
    highPass
};

enum class WarpMode : int
{
    phaseDist = 0,
    sync,
    bendPlus,
    bendMinus,
    fm,
    asym
};

enum class LfoShape : int
{
    sine = 0,
    triangle,
    sawUp,
    square,
    random
};

enum class ModSource : int
{
    none = 0,
    lfo1,           // 1
    lfo2,           // 2
    filterEnvelope, // 3
    ampEnvelope,    // 4
    velocity,       // 5
    modWheel,       // 6
    aftertouch,     // 7
    // ── Added in LFO/Env3 expansion (appended to preserve backward compat) ──
    lfo3,           // 8
    lfo4,           // 9
    modEnvelope,    // 10
    // ── Macros (appended to preserve backward compat with saved presets) ──
    macro1,         // 11
    macro2,         // 12
    macro3,         // 13
    macro4,         // 14
    macro5,         // 15
    macro6,         // 16
    macro7,         // 17
    macro8          // 18
};

enum class ModDestination : int
{
    none = 0,
    filterCutoff,
    filterResonance,
    filterDrive,
    osc1Pitch,
    osc2Pitch,
    osc1Level,
    osc2Level,
    subLevel,
    noiseLevel,
    osc1WavetablePosition,
    osc2WavetablePosition,
    osc1FmIndex,
    osc2FmIndex,
    osc1Pan,
    osc2Pan,
    globalPitch           // 16: modulates all oscillator frequencies equally
};
}
