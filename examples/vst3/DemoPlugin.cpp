#include "../common/DemoPatch.h"
#include <fad/engine/fx/DynamicFxChain.h>
#include <fad/engine/detail/Scope.h>
#include <fad/engine/synth/RenderEngine.h>
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <thread>

namespace fad::engine::example::vst3
{
using namespace Steinberg;
using namespace Steinberg::Vst;
using Engine = fadmodel1::dsp::synth::RenderEngine;
using Event = fadmodel1::dsp::midi::RenderMidiEvent;
using Message = fadmodel1::dsp::midi::ShortMessage;
const FUID processorId(0x7EB1A903, 0x15BA4FB7, 0xAB6DC7F1, 0xB9015CC1);
const FUID controllerId(0x9BC94BA7, 0xDB2B4308, 0x872B582E, 0x4DAEF960);
constexpr uint32 stateMagic = 0x45444146; // "FADE", little endian.
constexpr uint32 stateVersion = 1;
constexpr int32 automationCapacity = 4096;

bool readState(IBStream* stream, DemoState& result)
{
    if (!stream) return false;
    IBStreamer input(stream, kLittleEndian);
    uint32 magic = 0, version = 0;
    DemoState candidate;
    if (!input.readInt32u(magic) || !input.readInt32u(version)
        || magic != stateMagic || version != stateVersion) return false;
    for (auto& value : candidate.values) if (!input.readFloat(value)) return false;
    if (!valid(candidate)) return false;
    result = candidate;
    return true;
}
bool writeState(IBStream* stream, const DemoState& state)
{
    if (!stream || !valid(state)) return false;
    IBStreamer output(stream, kLittleEndian);
    if (!output.writeInt32u(stateMagic) || !output.writeInt32u(stateVersion)) return false;
    for (const auto value : state.values) if (!output.writeFloat(value)) return false;
    return true;
}

// Each mailbox has one writer: UI state restore, or audio automation (UI only
// while processing is stopped). State bytes never enter the audio callback.
// Atomic fields avoid a data race even on rejected sequence reads. Reads have
// three attempts; audio keeps its last complete state if the writer is late.
class StateMailbox
{
public:
    struct Read { DemoState state; uint32 version = 0, restoredVersion = 0; };
    StateMailbox() { write(DemoState{}, 0); }
    uint32 version() const noexcept { return sequence_.load(); }
    uint32 write(const DemoState& state, uint32 restoredVersion) noexcept
    {
        sequence_.fetch_add(1);
        for (size_t i = 0; i < controlCount; ++i) values_[i].store(std::bit_cast<uint32>(state.values[i]));
        restoredVersion_.store(restoredVersion);
        return sequence_.fetch_add(1) + 1;
    }
    bool read(Read& result) const noexcept
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            Read candidate;
            candidate.version = sequence_.load();
            if ((candidate.version & 1) != 0) continue;
            for (size_t i = 0; i < controlCount; ++i) candidate.state.values[i] = std::bit_cast<float>(values_[i].load());
            candidate.restoredVersion = restoredVersion_.load();
            if (sequence_.load() == candidate.version) { result = candidate; return true; }
        }
        return false;
    }
private:
    static_assert(std::atomic<uint32>::is_always_lock_free);
    std::atomic<uint32> sequence_{0}, restoredVersion_{0};
    std::array<std::atomic<uint32>, controlCount> values_{};
};

class Processor final : public AudioEffect
{
public:
    Processor() : engine_(bank_, graph_) { setControllerClass(controllerId); }
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new Processor); }
    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = AudioEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        addEventInput(STR16("Notes"), 16);
        return kResultOk;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32 inputs,
                                         SpeakerArrangement* outputs, int32 count) override
    {
        if (inputs != 0 || count != 1 || !outputs || outputs[0] != SpeakerArr::kStereo) return kResultFalse;
        return AudioEffect::setBusArrangements(nullptr, 0, outputs, count);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override
    { return size == kSample32 ? kResultTrue : kResultFalse; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override
    {
        if (setup.symbolicSampleSize != kSample32 || !std::isfinite(setup.sampleRate)
            || setup.sampleRate < 8000 || setup.sampleRate > 384000
            || setup.maxSamplesPerBlock < 1 || setup.maxSamplesPerBlock > 8192) return kResultFalse;
        DemoState initial;
        if (!capture(initial)) return kResultFalse;
        try
        {
            engine_.stageIsolatedOfflineInitialSnapshot(makeSnapshot(initial));
            engine_.prepare({setup.sampleRate, static_cast<uint32>(setup.maxSamplesPerBlock), 2}, demoSeed);
            engine_.setRealtimeProcessing(setup.processMode != kOffline);
        }
        catch (...) { prepared_ = false; return kResultFalse; }
        current_ = initial;
        StateMailbox::Read pending;
        if (pending_.read(pending)) restoredVersion_ = pending.version;
        audible_.write(current_, restoredVersion_);
        prepared_ = true;
        dirty_ = false;
        return AudioEffect::setupProcessing(setup);
    }
    tresult PLUGIN_API setActive(TBool active) override
    {
        if (prepared_) engine_.reset();
        return AudioEffect::setActive(active);
    }
    tresult PLUGIN_API setProcessing(TBool active) override
    {
        if (!prepared_) return kResultFalse;
        // VST3 can call this on its processing thread. Request the core's
        // bounded reset; the UI/control-thread reset belongs to setActive.
        if (!active) engine_.requestReset();
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override
    { return prepared_ ? static_cast<uint32>(std::ceil(processSetup.sampleRate * 3)) : 0; }
    tresult PLUGIN_API setState(IBStream* stream) override
    {
        DemoState state;
        if (!readState(stream, state)) return kResultFalse;
        pending_.write(state, 0);
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* stream) override
    {
        DemoState state;
        return capture(state) && writeState(stream, state) ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (data.numSamples < 0 || data.symbolicSampleSize != kSample32) return kInvalidArgument;
        bool stateChanged = false;
        StateMailbox::Read pending;
        if (pending_.version() != restoredVersion_ && pending_.read(pending) && pending.version != restoredVersion_)
        { current_ = pending.state; restoredVersion_ = pending.version; dirty_ = true; stateChanged = true; }

        struct Cursor
        {
            IParamValueQueue* queue = nullptr;
            int32 index = 0, count = 0, offset = std::numeric_limits<int32>::max();
            size_t control = 0;
            ParamValue value = 0;
            void next(int32 samples, int32 lowerBound) noexcept
            {
                int32 position = 0;
                if (!queue || index >= count || queue->getPoint(index++, position, value) != kResultTrue)
                { offset = std::numeric_limits<int32>::max(); return; }
                offset = std::clamp(position, lowerBound, samples);
            }
        };
        std::array<Cursor, controlCount> controls{};
        int64 pointCount = 0;
        if (data.inputParameterChanges)
        {
            const auto queues = std::min<int32>(static_cast<int32>(controlCount), data.inputParameterChanges->getParameterCount());
            for (int32 i = 0; i < queues; ++i)
            {
                auto* queue = data.inputParameterChanges->getParameterData(i);
                if (!queue || queue->getParameterId() < 1 || queue->getParameterId() > controlCount) continue;
                auto& control = controls[static_cast<size_t>(i)];
                control.queue = queue; control.count = std::max<int32>(0, queue->getPointCount());
                control.control = queue->getParameterId() - 1;
                pointCount += control.count;
            }
            // Oversized automation retains the previous complete control state.
            if (pointCount > automationCapacity) controls = {};
            else for (auto& control : controls) control.next(data.numSamples, 0);
        }

        if (data.numSamples != 0 && (!prepared_ || data.numSamples > processSetup.maxSamplesPerBlock)) return kResultFalse;
        size_t midiCount = 0, midiCursor = 0;
        if (data.inputEvents && data.numSamples > 0)
        {
            const auto count = std::max<int32>(0, data.inputEvents->getEventCount());
            if (count > static_cast<int32>(events_.size())) engine_.reportMidiInputOverflow();
            int32 previous = 0;
            for (int32 i = 0; i < std::min<int32>(count, static_cast<int32>(events_.size())); ++i)
            {
                Steinberg::Vst::Event event{};
                if (data.inputEvents->getEvent(i, event) != kResultTrue || event.busIndex != 0) continue;
                const auto offset = std::clamp(event.sampleOffset, previous, data.numSamples - 1);
                if (event.type == Steinberg::Vst::Event::kNoteOnEvent
                    && event.noteOn.channel >= 0 && event.noteOn.channel < 16
                    && event.noteOn.pitch >= 0 && event.noteOn.pitch < 128 && std::isfinite(event.noteOn.velocity))
                {
                    const auto velocity = static_cast<int>(std::lround(std::clamp(event.noteOn.velocity, 0.0f, 1.0f) * 127));
                    events_[midiCount++] = {Message::noteOn(event.noteOn.channel + 1, event.noteOn.pitch, velocity), offset};
                    previous = offset;
                }
                else if (event.type == Steinberg::Vst::Event::kNoteOffEvent
                    && event.noteOff.channel >= 0 && event.noteOff.channel < 16
                    && event.noteOff.pitch >= 0 && event.noteOff.pitch < 128)
                { events_[midiCount++] = {Message::noteOff(event.noteOff.channel + 1, event.noteOff.pitch), offset}; previous = offset; }
            }
        }
        const bool output = data.numOutputs > 0 && data.outputs && data.outputs[0].numChannels == 2
            && data.outputs[0].channelBuffers32 && data.outputs[0].channelBuffers32[0] && data.outputs[0].channelBuffers32[1];
        if (data.processContext && prepared_)
        {
            const auto& context = *data.processContext;
            if ((context.state & ProcessContext::kTempoValid) != 0) engine_.setTransportBpm(context.tempo);
            if ((context.state & ProcessContext::kProjectTimeMusicValid) != 0)
                engine_.setTransportPosition(context.projectTimeMusic, (context.state & ProcessContext::kPlaying) != 0, data.numSamples);
        }
        if (prepared_ && data.numSamples > 0) engine_.beginAudioCallback();
        const auto close = detail::ScopeExit{[&] { if (prepared_ && data.numSamples > 0) engine_.endAudioCallback(); }};
        for (int32 start = 0;;)
        {
            for (auto& control : controls)
                while (control.offset <= start)
                {
                    if (std::isfinite(control.value))
                    { current_.values[control.control] = static_cast<float>(std::clamp(control.value, 0.0, 1.0)); dirty_ = true; stateChanged = true; }
                    control.next(data.numSamples, start);
                }
            if (prepared_ && dirty_) dirty_ = !engine_.setParameterSnapshotFromAudioThread(makeSnapshot(current_));
            if (start == data.numSamples) break;
            auto end = data.numSamples;
            for (const auto& control : controls) end = std::min(end, control.offset);
            const auto first = midiCursor;
            while (midiCursor < midiCount && events_[midiCursor].samplePosition < end)
            { events_[midiCursor].samplePosition -= start; ++midiCursor; }
            const fadmodel1::dsp::midi::MidiEventSpan midi{events_.data() + first, midiCursor - first};
            if (output)
            {
                std::array<float*, 2> planes{data.outputs[0].channelBuffers32[0] + start, data.outputs[0].channelBuffers32[1] + start};
                engine_.render({planes.data(), 2, end - start}, midi);
            }
            else engine_.advanceBypassed(midi, end - start);
            start = end;
        }
        if (stateChanged) audible_.write(current_, restoredVersion_);
        if (data.numOutputs > 0 && data.outputs) data.outputs[0].silenceFlags = 0;
        return kResultOk;
    }
private:
    bool capture(DemoState& result) const noexcept
    {
        // setupProcessing/getState are UI-thread calls in the VST3 contract.
        // Let a short concurrent audio publication finish before reporting a
        // save failure. Audio itself still has only the three no-wait reads.
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            StateMailbox::Read pending, live;
            if (pending_.read(pending) && audible_.read(live))
            {
                result = pending.version == live.restoredVersion ? live.state : pending.state;
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }
    fadmodel1::dsp::wavetable::RuntimeWavetableBank bank_;
    fadmodel1::dsp::fx::DynamicFxChainCore graph_;
    Engine engine_;
    StateMailbox pending_, audible_;
    DemoState current_;
    uint32 restoredVersion_ = 0;
    bool prepared_ = false, dirty_ = false;
    std::array<Event, Engine::midiEventCapacity> events_{};
};

class Controller final : public EditController
{
public:
    static FUnknown* create(void*) { return static_cast<IEditController*>(new Controller); }
    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = EditController::initialize(context);
        if (result != kResultOk) return result;
        const DemoState defaults;
        const std::array<const char16*, controlCount> names{STR16("Gain"), STR16("Tone"), STR16("Attack"), STR16("Release")};
        for (size_t i = 0; i < controlCount; ++i)
            parameters.addParameter(names[i], nullptr, 0, defaults.values[i], ParameterInfo::kCanAutomate, static_cast<ParamID>(i + 1));
        return kResultOk;
    }
    tresult PLUGIN_API setComponentState(IBStream* stream) override { return restore(stream); }
    tresult PLUGIN_API setState(IBStream* stream) override { return restore(stream); }
    tresult PLUGIN_API getState(IBStream* stream) override
    {
        DemoState state;
        for (size_t i = 0; i < controlCount; ++i) state.values[i] = static_cast<float>(getParamNormalized(static_cast<ParamID>(i + 1)));
        return writeState(stream, state) ? kResultOk : kResultFalse;
    }
private:
    tresult restore(IBStream* stream)
    {
        DemoState state;
        if (!readState(stream, state)) return kResultFalse;
        for (size_t i = 0; i < controlCount; ++i) setParamNormalized(static_cast<ParamID>(i + 1), state.values[i]);
        return kResultOk;
    }
};
}

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace fad::engine::example::vst3;
BEGIN_FACTORY_DEF("FAD Engine Examples", "https://fadrecords.com", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(processorId), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "FAD Engine Community Demo", Vst::kDistributable, "Instrument|Synth", "0.1.0", kVstVersionString, Processor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(controllerId), PClassInfo::kManyInstances, kVstComponentControllerClass,
           "FAD Engine Community Demo Controller", 0, "", "0.1.0", kVstVersionString, Controller::create)
END_FACTORY
