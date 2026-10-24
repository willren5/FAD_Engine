#include <fad/engine/synth/RenderEngine.h>
#include <fad/engine/fx/DynamicFxChain.h>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using namespace fadmodel1;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<float> render()
{
    dsp::wavetable::RuntimeWavetableBank bank;
    dsp::fx::DynamicFxChainCore graph;
    dsp::synth::RenderEngine engine(bank, graph);
    engine.prepare({48000, 64, 2}, 1727);
    parameters::ParameterSnapshot patch;
    patch.oscillators[0].level = .2f; patch.oscillators[0].unisonRand = 0; patch.oscillators[0].drift = 0;
    patch.oscillators[1].enabled = false; patch.filter.enabled = false;
    patch.ampEnvelope = {.001f, .01f, .8f, .02f};
    patch.dynamicFx.hasExplicitTopology = true; patch.dynamicFx.slotCount = 4;
    const std::array<dsp::fx::FxType, 4> types{dsp::fx::FxType::drive,dsp::fx::FxType::chorus,
        dsp::fx::FxType::delay,dsp::fx::FxType::reverb};
    for (size_t index = 0; index < types.size(); ++index)
    {
        auto& slot = patch.dynamicFx.slots[index]; slot.type = static_cast<int>(types[index]);
        slot.enabled = true; slot.order = static_cast<int>(index); slot.routingStage = static_cast<int>(index);
        const int count = dsp::fx::FxCatalog::getParamCountForType(types[index]);
        for (int parameter = 0; parameter < count; ++parameter) slot.params[static_cast<size_t>(parameter)] = .25f;
    }
    require(engine.setParameterSnapshot(patch), "full standalone snapshot rejected");
    fad::engine::detail::PlanarBuffer audio(2, 64);
    std::vector<float> result; result.reserve(64 * 2 * 96);
    for (int block = 0; block < 96; ++block)
    {
        std::array<dsp::midi::RenderMidiEvent, 2> events{};
        size_t count = 0;
        if (block == 0) events[count++] = {dsp::midi::ShortMessage::noteOn(1, 60, 100), 17};
        if (block == 24) events[count++] = {dsp::midi::ShortMessage::noteOff(1, 60), 31};
        if (block == 48)
        {
            patch.dynamicFx.slots[0].type = static_cast<int>(dsp::fx::FxType::limiter);
            patch.dynamicFx.slots[0].params[2] = .5f;
            require(engine.setParameterSnapshot(patch), "standalone prepared graph publication rejected");
        }
        engine.render(audio.view(), {events.data(), count});
        require(engine.getFxLatencySamples() >= 0, "negative standalone graph latency");
        for (int sample = 0; sample < 64; ++sample)
            for (int channel = 0; channel < 2; ++channel)
            {
                const float value = audio.getSample(channel, sample);
                require(std::isfinite(value), "nonfinite full standalone engine output"); result.push_back(value);
            }
    }
    require(std::any_of(result.begin(), result.end(), [](float value) { return std::abs(value) > 1.e-6f; }), "full standalone engine was silent");
    return result;
}

parameters::ParameterSnapshot initialPatch(bool wet)
{
    parameters::ParameterSnapshot patch;
    patch.oscillators[0].level = .2f;
    patch.oscillators[0].unisonRand = 0;
    patch.oscillators[0].drift = 0;
    patch.oscillators[1].enabled = false;
    patch.subEnabled = false;
    patch.noise.enabled = false;
    patch.filter.enabled = false;
    patch.ampEnvelope = {.001f, .01f, .8f, .02f};
    patch.dynamicFx.hasExplicitTopology = true;
    patch.dynamicFx.slotCount = wet ? 1 : 0;
    if (wet)
    {
        auto& slot = patch.dynamicFx.slots[0];
        slot.type = static_cast<int>(dsp::fx::FxType::reverb);
        slot.enabled = true;
        slot.order = 0;
        slot.params[0] = dsp::fx::FxCatalog::normalizeParamValue(dsp::fx::FxType::reverb, 0, .6f);
        slot.params[2] = dsp::fx::FxCatalog::normalizeParamValue(dsp::fx::FxType::reverb, 2, .3f);
        slot.params[3] = dsp::fx::FxCatalog::normalizeParamValue(dsp::fx::FxType::reverb, 3, 1.f);
        slot.params[4] = dsp::fx::FxCatalog::normalizeParamValue(dsp::fx::FxType::reverb, 4, 12000.f);
    }
    return patch;
}
std::vector<float> renderInitial(const parameters::ParameterSnapshot& patch, bool stage)
{
    dsp::wavetable::RuntimeWavetableBank bank;
    dsp::fx::DynamicFxChainCore graph;
    dsp::synth::RenderEngine engine(bank, graph);
    if (stage) engine.stageIsolatedOfflineInitialSnapshot(patch);
    engine.prepare({48000, 256, 2}, 1727);
    if (!stage) require(engine.setParameterSnapshot(patch), "initial published snapshot refused");
    engine.setRealtimeProcessing(false);
    fad::engine::detail::PlanarBuffer audio(2, 256);
    std::vector<float> result;
    result.reserve(24000 * 2);
    for (int start = 0; start < 24000; start += 256)
    {
        const int frames = std::min(256, 24000 - start);
        std::array<dsp::midi::RenderMidiEvent, 2> events{};
        size_t count = 0;
        if (start == 0) events[count++] = {dsp::midi::ShortMessage::noteOn(1, 60, 100), 0};
        if (6000 >= start && 6000 < start + frames)
            events[count++] = {dsp::midi::ShortMessage::noteOff(1, 60), 6000 - start};
        const auto view = audio.view().subview(0, frames);
        engine.render(view, {events.data(), count});
        for (int sample = 0; sample < frames; ++sample)
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto value = view.channel(channel)[sample];
                require(std::isfinite(value), "initial snapshot render was nonfinite");
                result.push_back(value);
            }
    }
    return result;
}
void verifyStagedInitialFx()
{
    const auto stagedWet = renderInitial(initialPatch(true), true);
    const auto stagedDry = renderInitial(initialPatch(false), true);
    const auto publishedWet = renderInitial(initialPatch(true), false);
    const auto publishedDry = renderInitial(initialPatch(false), false);
    require(stagedWet != stagedDry, "staged initial FX were lost during prepare");
    require(stagedWet.size() == publishedWet.size(), "initial snapshot size changed");
    for (size_t i = 0; i < stagedWet.size(); ++i)
    {
        require(std::bit_cast<uint32_t>(stagedWet[i]) == std::bit_cast<uint32_t>(publishedWet[i]),
                "staged initial FX differ from prepare-then-publish");
        require(std::bit_cast<uint32_t>(stagedDry[i]) == std::bit_cast<uint32_t>(publishedDry[i]),
                "staged dry sound differs from prepare-then-publish");
    }
    dsp::wavetable::RuntimeWavetableBank bank;
    dsp::fx::DynamicFxChainCore graph;
    dsp::synth::RenderEngine engine(bank, graph);
    auto patch = initialPatch(true);
    patch.dynamicFx.slots[0].type = static_cast<int>(dsp::fx::FxType::limiter);
    patch.dynamicFx.slots[0].params[2] = 1.f;
    engine.stageIsolatedOfflineInitialSnapshot(patch);
    engine.prepare({48000, 256, 2}, 1727);
    require(engine.getFxLatencySamples() > 0, "staged latency-bearing FX were not prepared");
}

}
int main()
{
    try
    {
        verifyStagedInitialFx();
        const auto a = render(), b = render();
        require(a.size() == b.size(), "standalone render length changed");
        for (size_t sample = 0; sample < a.size(); ++sample)
            require(std::bit_cast<uint32_t>(a[sample]) == std::bit_cast<uint32_t>(b[sample]), "full standalone seeded render not repeatable");
        std::cout << "FullRenderTests PASS non_silent=true finite=true bit_exact_samples=" << a.size()
                  << " core_FX_graph=true graph_publication=true staged_initial_FX=true private_backend=false\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
