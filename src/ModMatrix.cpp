#include <fad/engine/modulation/ModMatrix.h>
#include <fad/engine/parameters/NativeModulationSources.h>

#include <fad/engine/diagnostics/ModulationTrace.h>

namespace fadmodel1::dsp::modulation
{
namespace
{
constexpr float maxFilterCutoffOctaves = 5.0f; // Must match Voice.cpp maxFilterEnvelopeOctaves
constexpr float maxFilterResonanceOffset = 0.85f;
constexpr float maxFilterDriveDb = 18.0f;
constexpr float maxOscillatorPitchSemitones = 12.0f;
constexpr float maxLevelOffset = 1.0f;
constexpr float maxWavetablePositionOffset = 1.0f;
constexpr float maxFmIndexOffset = 5.0f;
constexpr float maxPanOffset = 1.0f;

float sanitizeModValue(float value) noexcept
{
    return std::isfinite(value) ? value : 0.0f;
}

float getDestinationScale(parameters::ModDestination dest) noexcept
{
    switch (dest)
    {
        case parameters::ModDestination::filterCutoff:         return maxFilterCutoffOctaves;
        case parameters::ModDestination::filterResonance:      return maxFilterResonanceOffset;
        case parameters::ModDestination::filterDrive:          return maxFilterDriveDb;
        case parameters::ModDestination::osc1Pitch:
        case parameters::ModDestination::osc2Pitch:            return maxOscillatorPitchSemitones;
        case parameters::ModDestination::osc1Level:
        case parameters::ModDestination::osc2Level:
        case parameters::ModDestination::subLevel:
        case parameters::ModDestination::noiseLevel:           return maxLevelOffset;
        case parameters::ModDestination::osc1WavetablePosition:
        case parameters::ModDestination::osc2WavetablePosition: return maxWavetablePositionOffset;
        case parameters::ModDestination::osc1FmIndex:
        case parameters::ModDestination::osc2FmIndex:          return maxFmIndexOffset;
        case parameters::ModDestination::osc1Pan:
        case parameters::ModDestination::osc2Pan:              return maxPanOffset;
        case parameters::ModDestination::globalPitch:          return maxOscillatorPitchSemitones;
        case parameters::ModDestination::none:
        default:                                               return 0.0f;
    }
}

void applyNativeDestination(DestinationValues& d,int target,float value,bool audioRate = false) noexcept
{
    switch(target) {
        case 37:d.ampDecayOffset+=value;break;
        case 39:d.ampReleaseOffset+=value;break;
        case 38:d.ampSustainOffset+=value;break;
        case 3:case 16:d.nativeOctaveOffsets[target==3?0u:1u]+=value;break;
        case 7:case 20:d.nativeUnisonDetuneOffsets[target==7?0u:1u]+=value;break;
        case 172:case 173:d.nativeUnisonWidthOffsets[target==172?0u:1u]+=value;break;
        case 28:d.nativeNoisePitchOffset+=value;break;
        case 45:d.nativeFilterCutoffOffset+=value;break;
        case 46:d.nativeFilterResonanceOffset+=value;break;
        case 47:d.nativeFilterDriveOffset+=value;break;
        case 48:d.nativeFilterVariationOffset+=value;break;
        case 49:d.nativeFilterMixOffset+=value;break;
        case 80:d.nativeMasterPitchSemitones+=128.0f*value;break;
        case 10:case 23:(audioRate ? d.audioRateCoarsePitchSemitones : d.nativeCoarsePitchSemitones)[target==10?0u:1u]+=128.0f*value;break;
        case 5:case 18:d.nativeFinePitchCents[target==5?0u:1u]+=200.0f*value;break;
        case 4:case 17:d.nativeSemiPitchSemitones[target==4?0u:1u]+=24.0f*value;break;
        case 2:case 15:d.nativePanOffsets[target==2?0u:1u]+=2.0f*value;break;
        default:break;
    }
}

void clampDestinations(DestinationValues& d) noexcept
{
    const auto pitchLimit=d.widePitch?96.f:maxOscillatorPitchSemitones;
    d.audioPitch=std::clamp(sanitizeModValue(d.audioPitch), -96.f, 96.f);
    d.audioSubLevel=std::clamp(sanitizeModValue(d.audioSubLevel), -1.f, 1.f);
    d.audioNoiseLevel=std::clamp(sanitizeModValue(d.audioNoiseLevel), -1.f, 1.f);
    for(size_t i=0;i<2;++i) {
        d.audioOscPitch[i]=std::clamp(sanitizeModValue(d.audioOscPitch[i]), -96.f, 96.f);
        d.audioOscLevel[i]=std::clamp(sanitizeModValue(d.audioOscLevel[i]), -1.f, 1.f);
        d.audioFmIndex[i]=std::clamp(sanitizeModValue(d.audioFmIndex[i]), -100.f, 100.f);
    }
    d.nativeMasterPitchSemitones=std::clamp(sanitizeModValue(d.nativeMasterPitchSemitones), -128.0f, 128.0f);
    for(size_t i=0;i<2;++i) {
        d.nativeOctaveOffsets[i]=std::clamp(sanitizeModValue(d.nativeOctaveOffsets[i]), -1.0f, 1.0f);
        d.nativeUnisonDetuneOffsets[i]=std::clamp(sanitizeModValue(d.nativeUnisonDetuneOffsets[i]), -1.0f, 1.0f);
        d.nativeUnisonWidthOffsets[i]=std::clamp(sanitizeModValue(d.nativeUnisonWidthOffsets[i]), -1.0f, 1.0f);
        d.audioRateCoarsePitchSemitones[i]=std::clamp(sanitizeModValue(d.audioRateCoarsePitchSemitones[i]), -128.0f, 128.0f);
        d.nativeCoarsePitchSemitones[i]=std::clamp(sanitizeModValue(d.nativeCoarsePitchSemitones[i]), -128.0f, 128.0f);
        d.nativeFinePitchCents[i]=std::clamp(sanitizeModValue(d.nativeFinePitchCents[i]), -200.0f, 200.0f);
        d.nativeSemiPitchSemitones[i]=std::clamp(sanitizeModValue(d.nativeSemiPitchSemitones[i]), -24.0f, 24.0f);
        d.nativePanOffsets[i]=std::clamp(sanitizeModValue(d.nativePanOffsets[i]), -2.0f, 2.0f);
    }
    d.ampSustainOffset=std::clamp(sanitizeModValue(d.ampSustainOffset), -1.0f, 1.0f);
    d.ampDecayOffset=std::clamp(sanitizeModValue(d.ampDecayOffset), -1.0f, 1.0f);
    d.ampReleaseOffset=std::clamp(sanitizeModValue(d.ampReleaseOffset), -1.0f, 1.0f);
    d.nativeNoisePitchOffset=std::clamp(sanitizeModValue(d.nativeNoisePitchOffset), -1.0f, 1.0f);
    d.nativeFilterCutoffOffset=std::clamp(sanitizeModValue(d.nativeFilterCutoffOffset), -1.0f, 1.0f);
    for (auto* offset : {&d.nativeFilterResonanceOffset, &d.nativeFilterDriveOffset,
                         &d.nativeFilterVariationOffset, &d.nativeFilterMixOffset})
        *offset = std::clamp(sanitizeModValue(*offset), -1.0f, 1.0f);
    d.filterCutoffOctaves       = std::clamp(sanitizeModValue(d.filterCutoffOctaves), -maxFilterCutoffOctaves, maxFilterCutoffOctaves);
    d.filterResonanceOffset     = std::clamp(sanitizeModValue(d.filterResonanceOffset), -maxFilterResonanceOffset, maxFilterResonanceOffset);
    d.filterDriveDb             = std::clamp(sanitizeModValue(d.filterDriveDb), -maxFilterDriveDb, maxFilterDriveDb);
    d.globalPitchSemitones      = std::clamp(sanitizeModValue(d.globalPitchSemitones), -pitchLimit, pitchLimit);
    for (auto& v : d.oscillatorPitchSemitones)  v = std::clamp(sanitizeModValue(v), -pitchLimit, pitchLimit);
    for (auto& v : d.oscillatorLevelOffsets)    v = std::clamp(sanitizeModValue(v), -maxLevelOffset, maxLevelOffset);
    d.subLevelOffset            = std::clamp(sanitizeModValue(d.subLevelOffset), -maxLevelOffset, maxLevelOffset);
    d.noiseLevelOffset          = std::clamp(sanitizeModValue(d.noiseLevelOffset), -maxLevelOffset, maxLevelOffset);
    for (auto& v : d.wavetablePositionOffsets)  v = std::clamp(sanitizeModValue(v), -maxWavetablePositionOffset, maxWavetablePositionOffset);
    for (auto& v : d.fmIndexOffsets)            v = std::clamp(sanitizeModValue(v), -maxFmIndexOffset, maxFmIndexOffset);
    for (auto& v : d.oscillatorPanOffsets)      v = std::clamp(sanitizeModValue(v), -maxPanOffset, maxPanOffset);
}

void applyToDestination(DestinationValues& destinations,
                        parameters::ModDestination dest,
                        float scaledValue, bool audioRate = false) noexcept
{
    if(audioRate) {
        using D=parameters::ModDestination;
        switch(dest) {
            case D::osc1Pitch:destinations.audioOscPitch[0]+=scaledValue;return;
            case D::osc2Pitch:destinations.audioOscPitch[1]+=scaledValue;return;
            case D::globalPitch:destinations.audioPitch+=scaledValue;return;
            case D::osc1Level:destinations.audioOscLevel[0]+=scaledValue;return;
            case D::osc2Level:destinations.audioOscLevel[1]+=scaledValue;return;
            case D::subLevel:destinations.audioSubLevel+=scaledValue;return;
            case D::noiseLevel:destinations.audioNoiseLevel+=scaledValue;return;
            case D::osc1FmIndex:destinations.audioFmIndex[0]+=scaledValue;return;
            case D::osc2FmIndex:destinations.audioFmIndex[1]+=scaledValue;return;
            default:break;
        }
    }
    switch (dest)
    {
        case parameters::ModDestination::filterCutoff:
            destinations.filterCutoffOctaves += scaledValue; break;
        case parameters::ModDestination::filterResonance:
            destinations.filterResonanceOffset += scaledValue; break;
        case parameters::ModDestination::filterDrive:
            destinations.filterDriveDb += scaledValue; break;
        case parameters::ModDestination::osc1Pitch:
            destinations.oscillatorPitchSemitones[0] += scaledValue; break;
        case parameters::ModDestination::osc2Pitch:
            destinations.oscillatorPitchSemitones[1] += scaledValue; break;
        case parameters::ModDestination::osc1Level:
            destinations.oscillatorLevelOffsets[0] += scaledValue; break;
        case parameters::ModDestination::osc2Level:
            destinations.oscillatorLevelOffsets[1] += scaledValue; break;
        case parameters::ModDestination::subLevel:
            destinations.subLevelOffset += scaledValue; break;
        case parameters::ModDestination::noiseLevel:
            destinations.noiseLevelOffset += scaledValue; break;
        case parameters::ModDestination::osc1WavetablePosition:
            destinations.wavetablePositionOffsets[0] += scaledValue; break;
        case parameters::ModDestination::osc2WavetablePosition:
            destinations.wavetablePositionOffsets[1] += scaledValue; break;
        case parameters::ModDestination::osc1FmIndex:
            destinations.fmIndexOffsets[0] += scaledValue; break;
        case parameters::ModDestination::osc2FmIndex:
            destinations.fmIndexOffsets[1] += scaledValue; break;
        case parameters::ModDestination::osc1Pan:
            destinations.oscillatorPanOffsets[0] += scaledValue; break;
        case parameters::ModDestination::osc2Pan:
            destinations.oscillatorPanOffsets[1] += scaledValue; break;
        case parameters::ModDestination::globalPitch:
            destinations.globalPitchSemitones += scaledValue; break;
        case parameters::ModDestination::none:
        default:
            break;
    }
}
}

DestinationValues ModMatrix::render(const parameters::ModulationSnapshot& snapshot,
                                    const SourceValues& sources, bool preserveSmallAmounts) noexcept
{
    DestinationValues destinations;

    for (const auto& slot : snapshot.slots)
    {
        if (!slot.enabled || !slot.hasEffectiveAmount(preserveSmallAmounts)) continue;
        if (slot.nativeTarget >= 0) {
            applyNativeDestination(destinations,slot.nativeTarget,sanitizeModValue(resolveRouteSourceValue(slot,sources)*slot.amount),
                slot.nativeSource==19 || slot.nativeAuxSource==19);
            continue;
        }
        if (slot.nativeTarget >= 0 || !slot.hasSource()
            || slot.destination == parameters::ModDestination::none)
            continue;

        const auto sourceValue = sanitizeModValue(resolveRouteSourceValue(slot, sources));
        const auto amount = sanitizeModValue(slot.amount);
        const auto scaled = sanitizeModValue(sourceValue * amount * getDestinationScale(slot.destination));
        destinations.widePitch |= slot.widePitch;
        applyToDestination(destinations, slot.destination, scaled, slot.audioRate);
    }

    clampDestinations(destinations);
    return destinations;
}

float ModMatrix::destinationScale(parameters::ModDestination destination) noexcept
{
    return getDestinationScale(destination);
}

DestinationValues ModMatrix::renderWithTrace(
    const parameters::ModulationSnapshot& snapshot,
    const SourceValues& sources,
    diagnostics::ModulationTraceSnapshot& outTrace, bool preserveSmallAmounts) noexcept
{
    outTrace = {};
    outTrace.sourceValues = {sources.lfo1, sources.lfo2, sources.lfo3, sources.lfo4,
                             sources.filterEnvelope, sources.ampEnvelope, sources.modEnvelope,
                             sources.velocity, sources.modWheel, sources.aftertouch,
                             sources.macros};

    DestinationValues destinations;

    for (int d = 0; d <= static_cast<int>(parameters::ModDestination::globalPitch); ++d)
        outTrace.destinations[static_cast<size_t>(d)].destination =
            static_cast<parameters::ModDestination>(d);

    for (size_t slotIndex = 0; slotIndex < snapshot.slots.size(); ++slotIndex)
    {
        const auto& slot = snapshot.slots[slotIndex];
        auto& trace = outTrace.slots[slotIndex];
        trace.source = slot.source;
        trace.destination = slot.destination;
        trace.configuredAmount = slot.amount;

        if (!slot.enabled || !slot.hasEffectiveAmount(preserveSmallAmounts)) continue;
        if (slot.nativeTarget >= 0) {
            applyNativeDestination(destinations,slot.nativeTarget,sanitizeModValue(resolveRouteSourceValue(slot,sources)*slot.amount),
                slot.nativeSource==19 || slot.nativeAuxSource==19);
            continue;
        }
        if (slot.nativeTarget >= 0 || !slot.hasSource()
            || slot.destination == parameters::ModDestination::none)
        {
            trace.active = false;
            continue;
        }

        trace.active = true;
        const auto sourceValue = sanitizeModValue(resolveRouteSourceValue(slot, sources));
        trace.resolvedSourceValue = sourceValue;
        const auto modulationValue = sanitizeModValue(sourceValue * sanitizeModValue(slot.amount));
        trace.rawModulation = modulationValue;

        const auto scaledContribution =
            sanitizeModValue(modulationValue * getDestinationScale(slot.destination));
        destinations.widePitch |= slot.widePitch;
        applyToDestination(destinations, slot.destination, scaledContribution, slot.audioRate);
        trace.scaledContribution = scaledContribution;

        const auto destIdx = static_cast<size_t>(slot.destination);
        if (destIdx < outTrace.destinations.size())
        {
            auto& destTrace = outTrace.destinations[destIdx];
            destTrace.totalScaledContribution += scaledContribution;
            if (destTrace.activeSlotCount < 8)
                destTrace.activeSlotIndices[static_cast<size_t>(destTrace.activeSlotCount)] =
                    static_cast<int>(slotIndex);
            destTrace.activeSlotCount++;
        }
    }

    clampDestinations(destinations);
    return destinations;
}

float ModMatrix::resolveRouteSourceValue(const parameters::ModMatrixSlotSnapshot& route,
                                         const SourceValues& sources) noexcept
{
    if (!route.enabled) return 0.0f;
    if (route.nativeSource < 0) return resolveSourceValue(route.source,sources);
    if (!parameters::nativeModulationSourceSupported(route.nativeSource)
        || (route.nativeAuxSource!=0 && !parameters::nativeModulationSourceSupported(route.nativeAuxSource))) return 0.0f;
    const auto value=[&](int id) noexcept -> float {
        if(id>=5 && id<=12) return sources.nativeLfos[static_cast<size_t>(id-5)];
        switch(id) {
            case 1:return sources.modWheel;
            case 2:return sources.ampEnvelope;
            case 3:return sources.filterEnvelope;
            case 4:return sources.modEnvelope;
            case 13:return sources.velocity;
            case 14:return sources.nativeNoteNumber;
            case 15:return sources.nativeChannelPressure;
            case 16:return sources.nativePolyPressure;
            case 19:return sources.nativeNoiseOsc;
            case 20:case 21:case 22:case 23:return sources.nativeNoteOn[static_cast<size_t>(id-20)];
            case 24:case 25:case 26:case 27:return sources.macros[static_cast<size_t>(id-24)];
            case 28:return sources.nativePitchBend;
            case 29:return sources.nativeSlide;
            case 30:return sources.nativeReleaseVelocity;
            case 31:return route.midiLearnSlot>=0 && route.midiLearnSlot<32 ? sources.learnedMidiControllers[static_cast<size_t>(route.midiLearnSlot)] : 0.0f;
            default:return 0.0f;
        }
    };
    const auto depth=[&](int id) noexcept { return id>=5 && id<=12 ? sources.lfoDepth[static_cast<size_t>(id-5)] : 1.0f; };
    auto source=sanitizeModValue(value(route.nativeSource));
    const bool fadLfo = route.fadSemantics && route.nativeSource >= 5 && route.nativeSource <= 12;
    if (fadLfo)
    {
        const auto index = static_cast<size_t>(route.nativeSource - 5);
        source = sanitizeModValue(sources.fadLfos[index]);
        if ((sources.unipolarLfoMask & (uint32_t{1} << index)) != 0)
            source = 2.0f * source - 1.0f;
    }
    if(!fadLfo && (route.nativeFlags&1)!=0)source-=.5f;
    source*=depth(route.nativeSource);
    auto auxiliary=route.nativeAuxSource==0 ? 1.0f : sanitizeModValue(value(route.nativeAuxSource))*depth(route.nativeAuxSource);
    if((route.nativeFlags&2)!=0)auxiliary=1.0f-auxiliary;
    return sanitizeModValue(source*auxiliary);
}

float ModMatrix::resolveSourceValue(parameters::ModSource source,
                                    const SourceValues& sources) noexcept
{
    switch (source)
    {
        case parameters::ModSource::lfo1:           return sources.lfo1;
        case parameters::ModSource::lfo2:           return sources.lfo2;
        case parameters::ModSource::lfo3:           return sources.lfo3;
        case parameters::ModSource::lfo4:           return sources.lfo4;
        case parameters::ModSource::filterEnvelope: return sources.filterEnvelope;
        case parameters::ModSource::ampEnvelope:    return sources.ampEnvelope;
        case parameters::ModSource::modEnvelope:    return sources.modEnvelope;
        case parameters::ModSource::velocity:       return sources.velocity;
        case parameters::ModSource::modWheel:       return sources.modWheel;
        case parameters::ModSource::aftertouch:     return sources.aftertouch;
        case parameters::ModSource::macro1:         return sources.macros[0];
        case parameters::ModSource::macro2:         return sources.macros[1];
        case parameters::ModSource::macro3:         return sources.macros[2];
        case parameters::ModSource::macro4:         return sources.macros[3];
        case parameters::ModSource::macro5:         return sources.macros[4];
        case parameters::ModSource::macro6:         return sources.macros[5];
        case parameters::ModSource::macro7:         return sources.macros[6];
        case parameters::ModSource::macro8:         return sources.macros[7];
        case parameters::ModSource::none:
        default:                                    return 0.0f;
    }
}
}
