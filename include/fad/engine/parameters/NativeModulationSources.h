#pragma once
#include "ParameterTypes.h"
#include <array>
#include <string_view>

namespace fadmodel1::parameters
{
struct NativeModulationSource { int id; const char* name; const char* label; };
inline constexpr std::array<NativeModulationSource, 29> nativeModulationSources{{
    {1,"serum.modWheel","Mod Wheel"}, {2,"serum.ampEnvelope","Env 1"},
    {3,"serum.filterEnvelope","Env 2"}, {4,"serum.modEnvelope","Env 3"},
    {5,"serum.lfo1","LFO 1"}, {6,"serum.lfo2","LFO 2"},
    {7,"serum.lfo3","LFO 3"}, {8,"serum.lfo4","LFO 4"},
    {9,"serum.lfo5","LFO 5"},{10,"serum.lfo6","LFO 6"},{11,"serum.lfo7","LFO 7"},{12,"serum.lfo8","LFO 8"},
    {13,"serum.velocity","Velocity"}, {14,"serum.noteNumber","Note Number"},
    {15,"serum.aftertouch","Aftertouch"}, {16,"serum.polyAftertouch","Poly Aftertouch"},
    {19,"serum.noiseOsc","Noise OSC"},
    {20,"serum.noteOnRandom1","Note-On Random 1"}, {21,"serum.noteOnRandom2","Note-On Random 2"},
    {22,"serum.noteOnAlternate1","Note-On Alternate 1"}, {23,"serum.noteOnAlternate2","Note-On Alternate 2"},
    {24,"serum.macro1","Macro 1"}, {25,"serum.macro2","Macro 2"},
    {26,"serum.macro3","Macro 3"}, {27,"serum.macro4","Macro 4"},
    {28,"serum.pitchBend","Pitch Bend"},
    {29,"mpe.slide","MPE Slide (CC74)"},
    {30,"midi.releaseVelocity","Release Velocity"},
    {31,"midi.learn","MIDI Per-Note"}
}};
inline constexpr bool nativeModulationSourceSupported(int id) noexcept
{
    return (id>=1 && id<=16) || (id>=19 && id<=31);
}
// Serum FXP codes 29/30 have not been verified; these IDs belong only to FAD routes.
inline constexpr bool serumNativeModulationSourceSupported(int id) noexcept
{
    return (id>=1 && id<=16) || (id>=19 && id<=28);
}
inline const NativeModulationSource* nativeModulationSource(int id) noexcept
{
    for (const auto& source:nativeModulationSources) if(source.id==id) return &source;
    return nullptr;
}
inline int nativeModulationSourceFromName(std::string_view name) noexcept
{
    for (const auto& source:nativeModulationSources) if(name==source.name) return source.id;
    return -1;
}
inline ModSource legacySourceForNativeCode(int id) noexcept
{
    switch(id) {
        case 1:return ModSource::modWheel;
        case 2:return ModSource::ampEnvelope;
        case 3:return ModSource::filterEnvelope;
        case 4:return ModSource::modEnvelope;
        case 5:return ModSource::lfo1;
        case 6:return ModSource::lfo2;
        case 7:return ModSource::lfo3;
        case 8:return ModSource::lfo4;
        case 13:return ModSource::velocity;
        case 15:return ModSource::aftertouch;
        case 24:return ModSource::macro1;
        case 25:return ModSource::macro2;
        case 26:return ModSource::macro3;
        case 27:return ModSource::macro4;
        default:return ModSource::none;
    }
}
}
