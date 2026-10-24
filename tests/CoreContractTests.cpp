#include <fad/engine/Engine.h>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace
{
int checks = 0;
void require(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    using namespace fadmodel1::parameters;
    try
    {
        static_assert(std::is_trivially_copyable_v<ParameterSnapshot>);
        static_assert(std::is_standard_layout_v<ParameterSnapshot>);
        ParameterSnapshot p;
        require(defaultVoiceCount == 16 && maxRenderedUnisonVoiceCount == 16, "voice capacity changed");
        require(lfoCount == 8 && modMatrixSlotCount == 32, "modulation capacity changed");
        require(ids::maxFxSlots == 10 && ids::maxFxParamsPerSlot == 26, "FX capacity changed");
        require(static_cast<int>(ModDestination::globalPitch) == 16 && static_cast<int>(ModSource::macro8) == 18,
                "parameter ordinals changed");
        require(p.oscillators[0].level == .8f && p.oscillators[1].level == 0.f, "oscillator defaults changed");
        require(p.ampEnvelope.attackSeconds == .01f && p.ampEnvelope.releaseSeconds == .3f, "envelope defaults changed");
        require(p.modulation.lfos[0].rateHz == 1.25f && p.modulation.lfos[7].rateHz == .4f, "LFO defaults changed");
        require(curveOwnerCount == 21 && eqCurveOwnerBegin == 11, "curve owner ordinals changed");
        require(compatibleCurveHistoryOwner(11, 20) && !compatibleCurveHistoryOwner(0, 1), "history compaction changed");
        p.dynamicFx.slotCount = 4;
        p.dynamicFx.slots[0].order = 3; p.dynamicFx.slots[1].order = 0;
        p.dynamicFx.slots[2].order = 0; p.dynamicFx.slots[3].order = 99;
        auto order = sanitizeDynamicFxSlotOrder(p.dynamicFx, 4);
        require(order[0] == 1 && order[1] == 2 && order[2] == 0 && order[3] == 3, "duplicate order fallback changed");
        for (int i = 0; i < 4; ++i)
        { p.dynamicFx.slots[static_cast<size_t>(i)].type = 1; p.dynamicFx.slots[static_cast<size_t>(i)].routingStage = 0; }
        p.dynamicFx.slots[2].routingLane = 1;
        const auto route = buildDynamicFxRoutingPlan(p.dynamicFx, 4);
        require(route.count == 1 && route.slotCount == 4 && route.parallel, "parallel routing changed");
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        require(effectiveEnvelopeStageSeconds(.3f, nan, true) == .3f, "nonfinite envelope extension changed");
        require(effectiveEnvelopeStageSeconds(.001f, 1.f / 32001.f, true) == 0.f, "zero native stage changed");
        require(effectiveEnvelopeStageSeconds(.001f, 0.f, true) == .001f, "unset stage changed");
        for (uint32_t voice = 0; voice < 16; ++voice)
        {
            require(fadmodel1::dsp::synth::offlineVoiceSeed(0, voice) == 0x6d2b79f5u + voice * 0x12345u,
                    "zero seed legacy stream changed");
            require(fadmodel1::dsp::synth::offlineVoiceSeed(0x46414431u, voice) != 0, "explicit seed became zero");
        }
        for (int i = 0; i <= 127; ++i)
        {
            const float velocity = static_cast<float>(i) / 127.f;
            require(std::bit_cast<uint32_t>(fadmodel1::dsp::synth::shapeVelocity(velocity, .5f))
                    == std::bit_cast<uint32_t>(velocity), "neutral velocity is not bit exact");
        }
        require(fadmodel1::dsp::synth::shapeVelocity(nan, .5f) == 0.f, "invalid velocity not quarantined");
        std::cout << "FadEngineCoreContractTests PASS checks=" << checks << " snapshot_size=" << sizeof(p) << '\n';
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1; }
}
