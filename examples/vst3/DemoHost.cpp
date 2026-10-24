#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#if FAD_DEMO_RT_PROBE
extern "C" void fadmodel1BeginRealtimeMallocTracking() noexcept;
extern "C" uint64_t fadmodel1EndRealtimeMallocTracking() noexcept;
extern "C" uint64_t fadmodel1LastRealtimeFreeCount() noexcept;
extern "C" uint64_t fadmodel1LastRealtimeBlockingCallCount() noexcept;
extern "C" uint64_t fadmodel1LastRealtimeIoCallCount() noexcept;
#endif

namespace
{
using namespace Steinberg;
using namespace Steinberg::Vst;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
uint64_t callbacks = 0, allocations = 0, frees = 0, blocking = 0, io = 0;
void process(IAudioProcessor* processor, ProcessData& data)
{
#if FAD_DEMO_RT_PROBE
    fadmodel1BeginRealtimeMallocTracking();
#endif
    const auto result = processor->process(data);
#if FAD_DEMO_RT_PROBE
    allocations += fadmodel1EndRealtimeMallocTracking();
    frees += fadmodel1LastRealtimeFreeCount();
    blocking += fadmodel1LastRealtimeBlockingCallCount();
    io += fadmodel1LastRealtimeIoCallCount();
#endif
    ++callbacks;
    require(result == kResultOk, "VST3 process failed");
}
std::vector<char> capture(IComponent* component)
{
    MemoryStream state;
    require(component->getState(&state) == kResultOk && state.getSize() == 24, "Versioned component state capture failed");
    return {state.getData(), state.getData() + state.getSize()};
}
bool restore(IComponent* component, const std::vector<char>& state)
{
    MemoryStream stream(const_cast<char*>(state.data()), static_cast<TSize>(state.size()));
    return component->setState(&stream) == kResultOk;
}
void prepare(IComponent* component, IAudioProcessor* processor)
{
    SpeakerArrangement output = SpeakerArr::kStereo;
    require(processor->setBusArrangements(nullptr, 0, &output, 1) == kResultOk, "Stereo arrangement rejected");
    ProcessSetup setup{kRealtime, kSample32, 8192, 48000};
    require(processor->setupProcessing(setup) == kResultOk, "Host preparation rejected");
    require(component->activateBus(kAudio, kOutput, 0, true) == kResultOk, "Output activation rejected");
    require(component->activateBus(kEvent, kInput, 0, true) == kResultOk, "MIDI activation rejected");
    require(component->setActive(true) == kResultOk && processor->setProcessing(true) == kResultOk, "Host activation rejected");
}
void stop(IComponent* component, IAudioProcessor* processor)
{
    require(processor->setProcessing(false) == kResultOk && component->setActive(false) == kResultOk, "Host deactivation rejected");
}
std::vector<float> render(IAudioProcessor* processor)
{
    constexpr int frames = 48000;
    std::vector<float> samples(frames * 2);
    std::array<float, 64> left{}, right{};
    std::array<float*, 2> planes{left.data(), right.data()};
    AudioBusBuffers output;
    output.numChannels = 2; output.channelBuffers32 = planes.data();
    EventList events(2);
    ProcessData data;
    data.processMode = kRealtime; data.symbolicSampleSize = kSample32;
    data.numOutputs = 1; data.outputs = &output; data.numSamples = 64; data.inputEvents = &events;
    for (int start = 0; start < frames; start += 64)
    {
        events.clear(); left.fill(0); right.fill(0);
        if (start == 0)
        {
            Event note{}; note.busIndex = 0; note.type = Event::kNoteOnEvent; note.sampleOffset = 17;
            note.noteOn.channel = 0; note.noteOn.pitch = 60;
            note.noteOn.velocity = 100.0f / 127; note.noteOn.noteId = 1;
            require(events.addEvent(note) == kResultOk, "Note-on staging failed");
        }
        if (24031 >= start && 24031 < start + 64)
        {
            Event note{}; note.busIndex = 0; note.type = Event::kNoteOffEvent; note.sampleOffset = 24031 - start;
            note.noteOff.channel = 0; note.noteOff.pitch = 60; note.noteOff.noteId = 1;
            require(events.addEvent(note) == kResultOk, "Note-off staging failed");
        }
        process(processor, data);
        for (int sample = 0; sample < 64; ++sample)
        {
            require(std::isfinite(left[sample]) && std::isfinite(right[sample]), "Non-finite VST3 audio");
            samples[static_cast<size_t>(2 * (start + sample))] = left[sample];
            samples[static_cast<size_t>(2 * (start + sample) + 1)] = right[sample];
        }
    }
    require(std::any_of(samples.begin(), samples.end(), [](float sample) { return std::abs(sample) > 1.e-6f; }), "Actual VST3 MIDI render was silent");
    return samples;
}
void automate(IAudioProcessor* processor)
{
    ParameterChanges changes(2);
    int32 index = 0;
    auto* gain = changes.addParameterData(1, index);
    auto* tone = changes.addParameterData(2, index);
    require(gain && tone && gain->addPoint(17, .3, index) == kResultOk
            && tone->addPoint(31, .6, index) == kResultOk, "Automation staging failed");
    std::array<float, 64> left{}, right{};
    std::array<float*, 2> planes{left.data(), right.data()};
    AudioBusBuffers output; output.numChannels = 2; output.channelBuffers32 = planes.data();
    ProcessData data; data.processMode = kRealtime; data.symbolicSampleSize = kSample32;
    data.numSamples = 64; data.numOutputs = 1; data.outputs = &output; data.inputParameterChanges = &changes;
    process(processor, data);
}
void checkInputBounds(IComponent* component, IAudioProcessor* processor)
{
    const auto before = capture(component);
    std::vector<float> left(8192), right(8192);
    std::array<float*, 2> planes{left.data(), right.data()};
    AudioBusBuffers output; output.numChannels = 2; output.channelBuffers32 = planes.data();
    ProcessData data; data.processMode = kRealtime; data.symbolicSampleSize = kSample32;
    data.numSamples = 8192; data.numOutputs = 1; data.outputs = &output;
    ParameterChanges oversized(1);
    int32 index = 0;
    auto* queue = oversized.addParameterData(1, index);
    require(queue != nullptr, "Oversized automation fixture failed");
    for (int point = 0; point < 4097; ++point)
        require(queue->addPoint(point, .9, index) == kResultOk, "Oversized automation staging failed");
    require(queue->getPointCount() == 4097, "Automation fixture did not exceed the declared capacity");
    data.inputParameterChanges = &oversized;
    process(processor, data);
    require(capture(component) == before, "Oversized automation changed the previous complete state");
    data.inputParameterChanges = nullptr;
    data.numSamples = 64;

    EventList overflow(4097);
    for (int event = 0; event < 4097; ++event)
    {
        Event note{}; note.busIndex = 0; note.sampleOffset = 0;
        if (event < 4096)
        {
            note.type = Event::kNoteOnEvent; note.noteOn.channel = 0; note.noteOn.pitch = 60;
            note.noteOn.velocity = .8f; note.noteOn.noteId = event;
        }
        else { note.type = Event::kNoteOffEvent; note.noteOff.channel = 0; note.noteOff.pitch = 60; }
        require(overflow.addEvent(note) == kResultOk, "Oversized MIDI staging failed");
    }
    data.inputEvents = &overflow;
    process(processor, data);
    data.inputEvents = nullptr;
    for (int block = 0; block < 400; ++block) process(processor, data);
    for (int sample = 0; sample < 64; ++sample)
        require(std::isfinite(left[sample]) && std::isfinite(right[sample])
                && std::abs(left[sample]) < 1.e-7f && std::abs(right[sample]) < 1.e-7f,
                "Dropped note-off left an audible held note after MIDI overflow");

    ParameterChanges flush(1);
    auto* gain = flush.addParameterData(1, index);
    require(gain && gain->addPoint(0, .45, index) == kResultOk, "Zero-sample flush staging failed");
    data.numSamples = 0; data.numOutputs = 0; data.outputs = nullptr; data.inputParameterChanges = &flush;
    process(processor, data);
    const auto flushed = capture(component);
    require(flushed != before && restore(component, before), "Zero-sample parameter flush or its restore failed");
    std::cout << "midi_overflow_retired=true automation_overflow_preserved=true zero_sample_flush=true ";
}
void checkConcurrentRestore(IComponent* component, IAudioProcessor* processor, const std::vector<char>& initial)
{
    auto alternate = initial;
    const auto replacement = std::bit_cast<uint32_t>(.4f);
    for (unsigned byte = 0; byte < 4; ++byte) alternate[8 + byte] = static_cast<char>(replacement >> (8 * byte));
    std::atomic<bool> begin{false}, failed{false};
    std::thread audio([&]
    {
        while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
        try
        {
            std::array<float, 64> left{}, right{};
            std::array<float*, 2> planes{left.data(), right.data()};
            AudioBusBuffers output; output.numChannels = 2; output.channelBuffers32 = planes.data();
            ProcessData data; data.processMode = kRealtime; data.symbolicSampleSize = kSample32;
            data.numSamples = 64; data.numOutputs = 1; data.outputs = &output;
            for (int block = 0; block < 1000; ++block) process(processor, data);
        }
        catch (...) { failed.store(true); }
    });
    begin.store(true, std::memory_order_release);
    int observed = 0;
    for (int cycle = 0; cycle < 500; ++cycle)
    {
        if (!restore(component, (cycle & 1) ? initial : alternate)) failed.store(true);
        MemoryStream state;
        if (component->getState(&state) == kResultOk)
        {
            const std::vector<char> bytes(state.getData(), state.getData() + state.getSize());
            if (bytes != initial && bytes != alternate) failed.store(true);
            ++observed;
        }
    }
    audio.join();
    require(!failed.load() && observed == 500, "Concurrent UI restore/audio state capture failed");
    std::cout << "concurrent_restore_cycles=500 coherent_captures=" << observed << ' ';
}
}
int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "Usage: FadEngineDemoHost /absolute/candidate.vst3");
#if FAD_DEMO_RT_PROBE
        void* (*volatile allocate)(size_t) = &std::malloc;
        void (*volatile release)(void*) = &std::free;
        fadmodel1BeginRealtimeMallocTracking();
        auto* positive = allocate(64);
        release(positive);
        const auto positiveAllocations = fadmodel1EndRealtimeMallocTracking();
        require(positive && positiveAllocations > 0 && fadmodel1LastRealtimeFreeCount() > 0,
                "RT probe positive control did not detect allocation and free");
        std::cout << "rt_probe_positive_control=true ";
#endif
        std::string error;
        auto module = VST3::Hosting::Module::create(argv[1], error);
        require(static_cast<bool>(module), error.c_str());
        HostApplication host;
        PluginContextFactory::instance().setPluginContext(&host);
        const auto& factory = module->getFactory();
        auto classes = factory.classInfos();
        const auto selected = std::find_if(classes.begin(), classes.end(), [](const auto& info) { return info.category() == kVstAudioEffectClass; });
        require(selected != classes.end() && selected->name() == "FAD Engine Community Demo", "Unexpected module identity");
        PlugProvider first(factory, *selected);
        require(first.initialize(), "Component initialization failed");
        auto component = first.getComponentPtr(); auto controller = first.getControllerPtr();
        FUnknownPtr<IAudioProcessor> processor(component.get());
        require(component && controller && processor && controller->getParameterCount() == 4, "Required VST3 interfaces missing");
        prepare(component.get(), processor);
        const auto original = render(processor);
        automate(processor);
        const auto state = capture(component.get());
        MemoryStream controllerState(const_cast<char*>(state.data()), static_cast<TSize>(state.size()));
        require(controller->setComponentState(&controllerState) == kResultOk
                && std::abs(controller->getParamNormalized(1) - .3) < 1.e-6
                && std::abs(controller->getParamNormalized(2) - .6) < 1.e-6, "Automation/controller readback mismatch");
        require(!restore(component.get(), std::vector<char>{'b','a','d'}) && capture(component.get()) == state,
                "Truncated restore altered component state");
        auto invalid = state; invalid[8] = 0; invalid[9] = 0; invalid[10] = static_cast<char>(0xc0); invalid[11] = 0x7f;
        require(!restore(component.get(), invalid) && capture(component.get()) == state, "NaN restore was accepted");
        checkInputBounds(component.get(), processor);
        checkConcurrentRestore(component.get(), processor, state);
        require(restore(component.get(), state), "Final controlled restore failed");
        stop(component.get(), processor);
        prepare(component.get(), processor);
        const auto reference = render(processor);
        stop(component.get(), processor);

        PlugProvider second(factory, *selected);
        require(second.initialize(), "Fresh component initialization failed");
        auto recalled = second.getComponentPtr(); FUnknownPtr<IAudioProcessor> recalledProcessor(recalled.get());
        require(recalled && recalledProcessor && restore(recalled.get(), state), "Fresh instance recall failed");
        prepare(recalled.get(), recalledProcessor);
        const auto candidate = render(recalledProcessor);
        stop(recalled.get(), recalledProcessor);
        require(reference.size() == candidate.size(), "Fresh render length mismatch");
        for (size_t sample = 0; sample < reference.size(); ++sample)
            require(std::bit_cast<uint32_t>(reference[sample]) == std::bit_cast<uint32_t>(candidate[sample]), "Fresh instance state/audio recall changed samples");
        require(original != reference, "Automation did not change actual VST3 sound");
        require(allocations == 0 && frees == 0 && blocking == 0 && io == 0, "RT allocation/free/blocking/I/O violation");
        std::cout << "DemoHost PASS non_silent=true state_recall_bit_exact_values=" << reference.size()
                  << " callbacks=" << callbacks << " allocations=" << allocations << " frees=" << frees
                  << " blocking=" << blocking << " io=" << io
#if FAD_DEMO_RT_PROBE
                  << " RT_probe=true"
#else
                  << " RT_probe=false"
#endif
                  << " visible_DAW=false module=" << module->getPath() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr << "DemoHost FAIL: " << error.what() << '\n'; return 1; }
}
