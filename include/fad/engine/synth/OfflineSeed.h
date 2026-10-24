#pragma once
#include <cstdint>

namespace fadmodel1::dsp::synth
{
// v1: seed zero preserves the historical voice stream. An explicit render
// seed derives noise, unison and drift streams per voice. Native phase and FX
// streams retain their existing fixed reset policies.
constexpr uint32_t offlineVoiceSeed(uint32_t renderSeed,uint32_t voice) noexcept
{
    uint32_t mix=renderSeed;
    mix^=mix>>16;mix*=0x7feb352du;mix^=mix>>15;mix*=0x846ca68bu;mix^=mix>>16;
    const auto result=(0x6d2b79f5u+voice*0x12345u)^mix;
    return result==0?0x6d2b79f5u:result;
}
}
