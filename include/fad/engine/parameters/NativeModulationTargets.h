#pragma once
#include "ParameterTypes.h"
#include <array>
#include <string_view>

namespace fadmodel1::parameters
{
// Stable vendor parameter identity, independent of a reordered FAD FX slot.
// Only independently supported entries are exposed/imported.
struct NativeModulationTarget { int id; const char* name; const char* label; };
inline constexpr std::array<NativeModulationTarget,77> nativeModulationTargets{{
    {61,"serum.lfo1.rate","LFO 1 Rate"},
    {62,"serum.lfo2.rate","LFO 2 Rate"},
    {63,"serum.lfo3.rate","LFO 3 Rate"},
    {64,"serum.lfo4.rate","LFO 4 Rate"},
    {260,"serum.lfo5.rate","LFO 5 Rate"},
    {261,"serum.lfo6.rate","LFO 6 Rate"},
    {262,"serum.lfo7.rate","LFO 7 Rate"},
    {263,"serum.lfo8.rate","LFO 8 Rate"},
    {299,"serum.lfoBus1","LFO Bus 1"}, {300,"serum.lfoBus2","LFO Bus 2"},
    {301,"serum.lfoBus3","LFO Bus 3"}, {302,"serum.lfoBus4","LFO Bus 4"},
    {303,"serum.lfoBus5","LFO Bus 5"}, {304,"serum.lfoBus6","LFO Bus 6"},
    {305,"serum.lfoBus7","LFO Bus 7"}, {306,"serum.lfoBus8","LFO Bus 8"},
    {307,"serum.lfoBus9","LFO Bus 9"}, {308,"serum.lfoBus10","LFO Bus 10"},
    {309,"serum.lfoBus11","LFO Bus 11"}, {310,"serum.lfoBus12","LFO Bus 12"},
    {311,"serum.lfoBus13","LFO Bus 13"}, {312,"serum.lfoBus14","LFO Bus 14"},
    {313,"serum.lfoBus15","LFO Bus 15"}, {314,"serum.lfoBus16","LFO Bus 16"},

    {80,"serum.masterTune","Master Tune"},
    {3,"serum.osc1.octave","OSC A Octave"},{16,"serum.osc2.octave","OSC B Octave"},
    {10,"serum.osc1.coarsePitch","OSC A Coarse Pitch"},{23,"serum.osc2.coarsePitch","OSC B Coarse Pitch"},
    {5,"serum.osc1.fine","OSC A Fine"},{18,"serum.osc2.fine","OSC B Fine"},
    {4,"serum.osc1.semitone","OSC A Semitone"},{17,"serum.osc2.semitone","OSC B Semitone"},
    {2,"serum.osc1.pan","OSC A Pan"},{15,"serum.osc2.pan","OSC B Pan"},
    {7,"serum.osc1.unisonDetune","OSC A Unison Detune"},
    {20,"serum.osc2.unisonDetune","OSC B Unison Detune"},
    {172,"serum.osc1.unisonWidth","OSC A Unison Width"},
    {173,"serum.osc2.unisonWidth","OSC B Unison Width"},
    {28,"serum.noise.pitch","Noise Pitch"},
    {37,"serum.env1.decay","Amp Envelope Decay"},
    {39,"serum.env1.release","Amp Envelope Release"},
    {38,"serum.env1.sustain","Amp Envelope Sustain"},
    {45,"serum.filter.mainCutoff","Main Filter Frequency"},
    {46,"serum.filter.mainResonance","Main Filter Resonance"},
    {47,"serum.filter.mainDrive","Main Filter Drive"},
    {48,"serum.filter.mainVariation","Main Filter Variation"},
    {49,"serum.filter.mainMix","Main Filter Mix"},
    {96,"serum.distortion.wet","Distortion Wet"}, {97,"serum.distortion.drive","Distortion Drive"},
    {100,"serum.distortion.filterFrequency","Distortion Filter Frequency"},
    {103,"serum.flanger.wet","Flanger Wet"},
    {109,"serum.phaser.wet","Phaser Wet"},
    {113,"serum.phaser.frequency","Phaser Frequency"},
    {116,"serum.chorus.wet","Chorus Wet"},
    {147,"serum.hyper.wet","Hyper Wet"}, {149,"serum.hyper.detune","Hyper Detune"},
    {153,"serum.dimension.wet","Dimension Wet"},
    {148,"serum.hyper.rate","Hyper Rate"}, {152,"serum.dimension.size","Dimension Size"},
    {81,"serum.reverb.wet","Reverb Wet"}, {83,"serum.reverb.decay","Reverb Decay / Pre-Delay"},
    {124,"serum.delay.wet","Delay Wet"},
    {88,"serum.eq.lowFreq","EQ Low Frequency"}, {89,"serum.eq.highFreq","EQ High Frequency"},
    {90,"serum.eq.lowQ","EQ Low Q"}, {91,"serum.eq.highQ","EQ High Q"},
    {92,"serum.eq.lowGain","EQ Low Gain"}, {93,"serum.eq.highGain","EQ High Gain"},
    {143,"serum.filter.frequency","FX Filter Frequency"}, {144,"serum.filter.resonance","FX Filter Resonance"},
    {145,"serum.filter.drive","FX Filter Drive"}, {146,"serum.filter.variation","FX Filter Variation"},
    {135,"serum.compressor.threshold","Compressor Threshold"},
    {138,"serum.compressor.release","Compressor Release"}, {139,"serum.compressor.gain","Compressor Gain"},
    {269,"serum.compressor.wet","Compressor Wet"}
}};
inline const NativeModulationTarget* nativeModulationTarget(int id) noexcept
{
    for(const auto& target:nativeModulationTargets)if(target.id==id)return &target;
    return nullptr;
}
inline int nativeModulationTargetFromName(std::string_view name) noexcept
{
    for(const auto& target:nativeModulationTargets)if(name==target.name)return target.id;
    return -1;
}
inline constexpr int nativeLfoRateTargetIndex(int target) noexcept {
    return target>=61 && target<=64 ? target-61 : target>=260 && target<=263 ? target-260+4 : -1;
}
inline constexpr bool nativeFxModulationTarget(int target) noexcept {
    return (target>=147 && target<=149) || target==152 || target==153 || target==96 || target==97 || target==100 || target==103 || target==109 || target==113 || target==116 || target==81 || target==83 || target==124 || (target>=88 && target<=93) || target==135 || target==138 || target==139 || target==269 || (target>=143 && target<=146);
}
inline ModDestination legacyDestinationForNativeTarget(int id) noexcept
{
    switch(id) {
        case 45:return ModDestination::filterCutoff;
        case 46:return ModDestination::filterResonance;
        case 47:return ModDestination::filterDrive;
        case 80:return ModDestination::globalPitch;
        case 10:case 5:case 4:case 3:return ModDestination::osc1Pitch;
        case 23:case 18:case 17:case 16:return ModDestination::osc2Pitch;
        case 2:return ModDestination::osc1Pan;
        case 15:return ModDestination::osc2Pan;
        default:return ModDestination::none;
    }
}
}
