#include <fad/engine/synth/RenderEngine.h>
#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using namespace fadmodel1;
void require(bool condition, const char* description)
{ if (!condition) throw std::runtime_error(description); }

// An explicitly dry test fixture, never a production/demo implementation.
// It refuses enabled effects so this test cannot silently claim FX coverage.
class DisabledFxFixture final : public dsp::fx::FxBackend
{
public:
    void prepare(const fad::engine::ProcessSpec&) override {}
    void reset() override {}
    void process(fad::engine::AudioBufferView) override {}
    void processWithDiagnostics(fad::engine::AudioBufferView, std::vector<dsp::diagnostics::CpuMeter>&, dsp::diagnostics::ClipDetector&) override {}
    void setEqCurveInput(const std::array<parameters::CurveNodes, parameters::curveOwnerCount>*) noexcept override {}
    void setNativeModulationInput(const parameters::ModulationSnapshot*, const dsp::modulation::SourceValues*, int) noexcept override {}
    void setSlotCount(int) override {}
    void setSlotType(int, dsp::fx::FxType) override {}
    void setSlotOrder(const std::array<int, parameters::ids::maxFxSlots>&, int) override {}
    void setSlotEnabled(int, bool enabled) override { require(!enabled, "dry fixture cannot accept enabled FX"); }
    void setSlotRouting(int, int, int) override {}
    void setSlotParam(int, int, float, bool) override {}
    void setTransportBpm(double) override {}
    void beginBatchUpdate(bool) noexcept override {}
    void endBatchUpdate() noexcept override { sequence_ += 2; }
    uint64_t getLatestDesiredStateSequence() const noexcept override { return sequence_; }
    bool tryAcceptPreparedState(uint64_t sequence) noexcept override { return sequence == sequence_; }
    bool tryAcceptLatestPreparedState() noexcept override { return true; }
    void beginAudioCallback() noexcept override { callback_ = true; }
    void closeAudioAcceptanceBoundary() noexcept override {}
    void endAudioCallback() noexcept override { callback_ = false; }
    bool isAudioCallbackActive() const noexcept override { return callback_; }
    int getActiveLatencySamples() noexcept override { return 0; }
    std::array<float, parameters::ids::maxFxSlots> getGainReductionDbForUi() noexcept override
    { std::array<float, parameters::ids::maxFxSlots> result{}; result.fill(-1); return result; }
    bool hasPendingRebuildForControlThread() const noexcept override { return false; }
    void tryApplyPendingRebuildOnControlThread() noexcept override {}
private:
    uint64_t sequence_ = 2;
    bool callback_ = false;
};
class ProbeEngine final : public dsp::synth::RenderEngine
{
public:
    using RenderEngine::RenderEngine;
    bool held() const noexcept { return hasHeldOrSustainedNotes(); }
    bool active() const noexcept { return hasActiveVoices(); }
};

parameters::ParameterSnapshot patch()
{
    parameters::ParameterSnapshot snapshot;
    snapshot.oscillators[0].level = .2f;
    snapshot.oscillators[0].drift = 0;
    snapshot.oscillators[0].unisonRand = 0;
    snapshot.oscillators[1].enabled = false;
    snapshot.ampEnvelope = {.001f, .02f, .8f, .01f};
    snapshot.filter.enabled = false;
    return snapshot;
}
using Event = dsp::midi::RenderMidiEvent;
using Message = dsp::midi::ShortMessage;
std::vector<float> renderClip(int blockSize)
{
    dsp::wavetable::RuntimeWavetableBank bank;
    DisabledFxFixture fx;
    ProbeEngine engine(bank, fx);
    engine.prepare({48000, static_cast<uint32_t>(blockSize), 2}, 1709);
    require(&engine.getWavetableBank() == &bank, "render owner created a second asset bank");
    require(engine.setParameterSnapshot(patch()), "coherent initial snapshot refused");
    const std::array<Event, 8> timeline{{
        {Message::noteOn(1, 60, 100), 0}, {Message::pitchWheel(1, 10000), 17},
        {Message::controllerEvent(1, 64, 127), 400}, {Message::noteOff(1, 60), 600},
        {Message::noteOn(2, 67, 90), 800}, {Message::controllerEvent(1, 64, 0), 1000},
        {Message::noteOff(2, 67), 1500}, {Message::allSoundOff(1), 1800}}};
    std::vector<float> result(2 * 3072);
    fad::engine::detail::PlanarBuffer storage(2, blockSize);
    for (int start = 0; start < 3072; start += blockSize)
    {
        const auto count = std::min(blockSize, 3072 - start);
        std::array<Event, 8> events{};
        size_t filled = 0;
        for (auto event : timeline)
            if (event.samplePosition >= start && event.samplePosition < start + count)
            { event.samplePosition -= start; events[filled++] = event; }
        const auto view = storage.view().subview(0, count);
        engine.render(view, {events.data(), filled});
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < count; ++sample)
                result[static_cast<size_t>(2 * (start + sample) + channel)] = view.channel(channel)[sample];
    }
    require(!engine.held(), "released pedal/MIDI timeline retained held notes");
    require(std::all_of(result.begin(), result.end(), [](float value) { return std::isfinite(value); }), "nonfinite standalone render");
    require(std::any_of(result.begin(), result.end(), [](float value) { return std::abs(value) > 1.e-6f; }), "standalone render was silent");
    return result;
}
}

int main()
{
    try
    {
        const auto reference = renderClip(512);
        uint64_t compared = 0;
        for (int block : {1, 17, 64, 1024})
        {
            const auto candidate = renderClip(block);
            for (size_t sample = 0; sample < reference.size(); ++sample)
            { require(std::bit_cast<uint32_t>(reference[sample]) == std::bit_cast<uint32_t>(candidate[sample]), "standalone timeline changed with block partition"); ++compared; }
        }

        dsp::wavetable::RuntimeWavetableBank bank;
        DisabledFxFixture fx;
        ProbeEngine engine(bank, fx);
        engine.prepare({48000, 64, 2}, 1709);
        require(engine.setParameterSnapshot(patch()), "snapshot refused");
        fad::engine::detail::PlanarBuffer storage(2, 64);
        std::vector<Event> overflow(ProbeEngine::midiEventCapacity + 1, {Message::controllerEvent(1, 1, 0), 0});
        overflow[0] = {Message::noteOn(1, 60, 127), 0};
        overflow.back() = {Message::noteOff(1, 60), 0};
        engine.render(storage.view(), overflow);
        require(!engine.held(), "overflowed/dropped note-off left a held note");
        require(engine.hasPendingResetDeferredMidiOverflowLog(), "MIDI overflow was hidden from control diagnostics");
        for (int block = 0; block < 8; ++block) engine.render(storage.view(), {});
        require(!engine.active(), "overflow panic failed to retire voices");
        engine.reportMidiInputOverflow();
        engine.reset();
        const Event afterReset{Message::noteOn(1, 67, 100), 0};
        engine.render(storage.view(), {&afterReset, 1});
        require(engine.held(), "reset retained an obsolete input-overflow panic request");
        std::cout << "RenderEngineTests PASS independent_non_silent=true partition_bit_exact_values=" << compared
                  << " single_bank=true bounded_input_overflow=true FX_coverage=disabled_only\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
