#pragma once

#include <array>
#include <cstdint>

#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fadmodel1::dsp::diagnostics
{
struct ModulationTraceSnapshot;
}

namespace fadmodel1::dsp::modulation
{
struct SourceValues
{
    float lfo1 = 0.0f;
    float lfo2 = 0.0f;
    float lfo3 = 0.0f;
    float lfo4 = 0.0f;
    float filterEnvelope = 0.0f;
    float ampEnvelope = 0.0f;
    float modEnvelope = 0.0f;
    float velocity = 0.0f;
    float modWheel = 0.0f;
    float aftertouch = 0.0f;
    // Macro knob values (0..1) — user-assignable modulation sources
    std::array<float, 8> macros{};
    std::array<float,parameters::lfoCount> nativeLfos{};
    std::array<float,parameters::lfoCount> lfoDepth{1,1,1,1,1,1,1,1};
    float nativeNoteNumber=0, nativePitchBend=.5f, nativeChannelPressure=0, nativePolyPressure=0;
    float nativeSlide=0, nativeReleaseVelocity=0;
    std::array<float, parameters::lfoCount> fadLfos{};
    // Imported curve samples remain unipolar until a new FAD primary route resolves them.
    // Native/legacy and auxiliary sources retain their original domain.
    uint32_t unipolarLfoMask = 0;
    float nativeNoiseOsc = 0.0f;
    bool nativeVoiceActive = false;
    std::array<float,32> learnedMidiControllers{};
    std::array<float,4> nativeNoteOn{};
};

struct DestinationValues
{
    float filterCutoffOctaves = 0.0f;
    float nativeFilterCutoffOffset = 0.0f;
    float ampSustainOffset = 0.0f;
    float ampDecayOffset = 0.0f;
    float ampReleaseOffset = 0.0f;
    float nativeMasterPitchSemitones = 0.0f;
    std::array<float,2> nativeCoarsePitchSemitones{}, nativeFinePitchCents{}, nativeSemiPitchSemitones{}, nativePanOffsets{};
    std::array<float,2> nativeOctaveOffsets{};
    std::array<float,2> nativeUnisonDetuneOffsets{};
    std::array<float,2> nativeUnisonWidthOffsets{};
    float nativeNoisePitchOffset = 0.0f;
    float filterResonanceOffset = 0.0f;
    float filterDriveDb = 0.0f;
    float globalPitchSemitones = 0.0f;
    bool widePitch = false;
    float audioPitch=0, audioSubLevel=0, audioNoiseLevel=0;
    std::array<float,2> audioOscPitch{}, audioOscLevel{}, audioFmIndex{};
    std::array<float, 2> oscillatorPitchSemitones{0.0f, 0.0f};
    std::array<float, 2> oscillatorLevelOffsets{0.0f, 0.0f};
    float subLevelOffset = 0.0f;
    float noiseLevelOffset = 0.0f;
    std::array<float, 2> wavetablePositionOffsets{0.0f, 0.0f};
    std::array<float, 2> fmIndexOffsets{0.0f, 0.0f};
    std::array<float, 2> oscillatorPanOffsets{0.0f, 0.0f};
    float nativeFilterResonanceOffset = 0.0f;
    float nativeFilterDriveOffset = 0.0f;
    float nativeFilterVariationOffset = 0.0f;
    float nativeFilterMixOffset = 0.0f;
    std::array<float,2> audioRateCoarsePitchSemitones{};
};

class ModMatrix final
{
public:
    // Physical delta for a legacy semantic route with source=1 and amount=1.
    // Native/imported vendor targets have separate semantics and are excluded.
    static float destinationScale(parameters::ModDestination) noexcept;
    static DestinationValues render(const parameters::ModulationSnapshot& snapshot,
                                    const SourceValues& sources, bool preserveSmallAmounts = false) noexcept;

    static DestinationValues renderWithTrace(
        const parameters::ModulationSnapshot& snapshot,
        const SourceValues& sources,
        diagnostics::ModulationTraceSnapshot& outTrace, bool preserveSmallAmounts = false) noexcept;

    static float resolveRouteSourceValue(const parameters::ModMatrixSlotSnapshot& route,
                                          const SourceValues& sources) noexcept;
    static float resolveSourceValue(parameters::ModSource source,
                                    const SourceValues& sources) noexcept;
};
}
