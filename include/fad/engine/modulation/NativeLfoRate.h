#pragma once
#include "ModMatrix.h"
#include <fad/engine/parameters/NativeModulationTargets.h>
#include <cmath>
#include <fad/engine/math/Numeric.h>

namespace fadmodel1::dsp::modulation
{
inline uint32_t nativeLfoRateRouteMask(const parameters::ModulationSnapshot& snapshot) noexcept
{
    uint32_t result=0;
    for(const auto& route:snapshot.slots) {
        const int index=parameters::nativeLfoRateTargetIndex(route.nativeTarget);
        if(index>=0 && route.hasSource() && route.hasEffectiveAmount())
            result|=uint32_t{1}<<index;
    }
    return result;
}
inline float nativeLfoBeats(float raw,int timing) noexcept
{
    const int division=std::clamp(static_cast<int>(std::floor(raw*14.0f+1.0f/6.0f)), 0, 14);
    auto beats=std::ldexp(128.0f,-division);
    const auto fraction=raw*14.0f-static_cast<float>(division);
    if((timing&2)!=0 && fraction>=.5f)beats*=2.0f/3.0f;
    else if((timing&1)!=0 && fraction>=1.0f/6.0f)beats*=.75f;
    return beats;
}
// Canonical unmodulated base. FAD routes modulate this rate in octaves;
// legacy/Serum routes keep the imported knob and division law below.
inline float baseLfoRate(const parameters::LfoSnapshot& settings, double bpm) noexcept
{
    if (settings.syncToTempo && std::isfinite(settings.cycleBeats) && settings.cycleBeats > 0)
        return static_cast<float>(bpm / 60.0) / std::clamp(settings.cycleBeats, .0078125f, 128.0f);
    if (!settings.syncToTempo && std::isfinite(settings.extendedRateHz) && settings.extendedRateHz >= 0)
        return std::clamp(settings.extendedRateHz, 0.0f, parameters::maxLfoWideRateHz);
    const float rate = std::isfinite(settings.rateHz) ? std::clamp(settings.rateHz, .01f, 40.0f) : 1.0f;
    if (!settings.syncToTempo) return rate;
    constexpr std::array<float,12> beats{4,3,2,1.5f,1,.75f,.5f,.375f,.25f,.1875f,.125f,.0625f};
    const auto normalized = std::clamp(std::pow(std::clamp((rate - .05f) / (40.0f - .05f), 0.0f, 1.0f), .3f), 0.0f, 1.0f);
    return static_cast<float>(bpm / 60.0) / beats[static_cast<size_t>(std::clamp(fad::engine::math::roundToNearestInt(normalized*11), 0, 11))];
}
inline float modulatedNativeLfoRate(const parameters::ModulationSnapshot& snapshot,size_t index,
                                    const SourceValues& sources,double bpm) noexcept
{
    const auto& settings=snapshot.lfos[index];
    const auto baseRate=settings.extendedRateHz>=0 ? settings.extendedRateHz : settings.rateHz;
    // Imported automation keeps its original 0..100 Hz law. An explicitly
    // edited base above 100 Hz extends that same law to the new free-rate ceiling.
    const auto maximumRaw = !settings.syncToTempo && baseRate > 100.0f
        ? std::sqrt(std::sqrt((baseRate > parameters::maxLfoFreeRateHz
            ? parameters::maxLfoWideRateHz : parameters::maxLfoFreeRateHz) * .01f)) : 1.0f;
    auto raw=std::sqrt(std::sqrt(std::clamp(baseRate*.01f, 0.0f, maximumRaw*maximumRaw*maximumRaw*maximumRaw)));
    bool legacy = false;
    float fadOffset = 0;
    for(const auto& route:snapshot.slots)
        if(parameters::nativeLfoRateTargetIndex(route.nativeTarget)==static_cast<int>(index)
            && route.hasSource() && route.hasEffectiveAmount()) {
            const float offset = ModMatrix::resolveRouteSourceValue(route,sources)*route.amount;
            if (route.fadSemantics) fadOffset += offset;
            else { legacy = true; raw += offset; }
        }
    raw=std::clamp(std::isfinite(raw)?raw:0.0f, 0.0f, maximumRaw);
    const float base = !legacy ? baseLfoRate(settings,bpm)
        : settings.syncToTempo ? static_cast<float>(bpm/60.0)/nativeLfoBeats(raw,settings.nativeTiming)
                              : 100.0f*raw*raw*raw*raw;
    return base * std::exp2(std::clamp(fadOffset*8.0f, -16.0f, 16.0f));
}
}
