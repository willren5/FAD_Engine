#pragma once
#include <fad/engine/AudioView.h>
#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/detail/EventSignal.h>
#include <fad/engine/fx/FxBackend.h>
#include <fad/engine/parameters/RenderTimeline.h>
#include <fad/engine/parameters/RenderParameterAccess.h>
#include <fad/engine/synth/RenderServiceHooks.h>
#include <fad/engine/diagnostics/DiagnosticsSnapshot.h>
#include <fad/engine/diagnostics/SessionMetrics.h>
#include <fad/engine/midi/RenderMidiEvent.h>
#include <fad/engine/midi/MidiCiProcessState.h>
#include <fad/engine/modulation/NoteOnModulation.h>
#include <fad/engine/safety/AudioSafetyStage.h>
#include <fad/engine/safety/OutputPeakLimiter.h>
#include <fad/engine/rt/SpscCursor.h>
#include <fad/engine/synth/VoiceCore.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

struct FadModel1HostStateRestoreTestAccess;

namespace fadmodel1::dsp::synth
{
class RenderEngine
{
    friend struct ::FadModel1HostStateRestoreTestAccess;
    friend struct OscillatorUiTestAccess;
public:
    using SupplementalMidiEvent = fadmodel1::dsp::midi::RenderMidiEvent;
    static constexpr int realtimeUnisonRenderLimit = parameters::maxRenderedUnisonVoiceCount;
    // Two borrowed, stably ordered input lanes; each lane has this bound.
    // Overflow retains its accepted prefix and requests the existing panic
    // fade. It never allocates or leaves a dropped note-off holding a note.
    static constexpr size_t midiEventCapacity = 4096;
    static_assert(realtimeUnisonRenderLimit == parameters::maxRenderedUnisonVoiceCount);

    RenderEngine(wavetable::RuntimeWavetableBank&, fx::FxBackend&, fad::engine::RenderServiceHooks = {});
    ~RenderEngine();

    void prepare(const fad::engine::ProcessSpec& spec, uint32_t offlineRenderSeed = 0);
    void reset();
    void requestReset();
    // Audio thread only: route UI Panic through the existing click-safe MIDI path.
    void silenceAllChannelsFromAudioThread();
    void reportMidiInputOverflow() noexcept;
    using MidiObserver = void (*)(void*, const midi::ShortMessage&, const midi::UmpChannelVoice&, midi::Source, bool) noexcept;
    void setMidiObserver(void* owner, MidiObserver observer) noexcept { midiObserverOwner=owner; midiObserver=observer; }
    using MidiNoteInitialiser=void (*)(void*,midi::Source,int,int) noexcept;
    void setMidiNoteInitialiser(MidiNoteInitialiser fn) noexcept { midiNoteInitialiser=fn; }
    using MidiSourceResetter=void (*)(void*,midi::Source) noexcept;
    void setMidiSourceResetter(MidiSourceResetter fn) noexcept { midiSourceResetter=fn; }
    void setLearnedPerNoteController(midi::Source source,int channel,int note,int slot,float value) noexcept;


    void silenceMidiSourceFromAudioThread(midi::Source source);
    // Audio thread only: release a deflected screen wheel without resetting physical controllers at ordinary editor close.
    void resetInstrumentPitchFromAudioThread() noexcept;
    void setMidiCiMpeProfilesFromAudioThread(midi::Source, const std::array<uint8_t, 16>&) noexcept;
    void captureMidiCiChannelStatesFromAudioThread(midi::Source, std::array<midici::ChannelState, 16>&) const noexcept;
    bool setParameterSnapshot(const parameters::ParameterSnapshot& snapshot) noexcept;
    bool setParameterSnapshotFromAudioThread(const parameters::ParameterSnapshot& snapshot) noexcept;
    // The pointed gate must either outlive SynthEngine or be cleared with nullptr
    // before the owner is destroyed. PluginProcessor declares its source before
    // SynthEngine so reverse teardown keeps this pointer valid during ~SynthEngine().
    void setStateRestoreGate(const std::atomic<bool>* gate) noexcept
    {
        stateRestoreGate.store(gate, std::memory_order_release);
    }
    void setRealtimeProcessing(bool shouldRenderRealtime) noexcept
    {
        realtimeProcessing.store(shouldRenderRealtime, std::memory_order_release);
    }
    void setTransportBpm(double newTempoBpm) noexcept;
    // Private isolated owner only, before prepare and with no audio callback.
    // Initialise smoothers and FX from the frozen sound, not a previous render.
    void stageIsolatedOfflineInitialSnapshot(const parameters::ParameterSnapshot& snapshot) noexcept
    { parameterSnapshot=snapshot; stagedIsolatedInitialSnapshotPending=true; voiceRenderContext.reset(); resetRequested.store(false,std::memory_order_release); }
    void setTransportPosition(double ppqPosition, bool isPlaying, int blockSize) noexcept;
    void beginAudioCallback() noexcept { fxChain.beginAudioCallback(); }
    void closeAudioAcceptanceBoundary() noexcept { fxChain.closeAudioAcceptanceBoundary(); }
    void endAudioCallback() noexcept { fxChain.endAudioCallback(); }
    void render(fad::engine::AudioBufferView audioBuffer,
                midi::MidiEventSpan midiBuffer,
                const SupplementalMidiEvent* supplementalMidiEvents = nullptr,
                size_t supplementalMidiEventCount = 0);
    struct RenderParameterTimeline
    {
        auv3::RenderTimeline& timeline;
        fad::engine::RenderParameterAccess& source;
        parameters::ParameterSnapshot& cachedSnapshot;
        uint32_t& cachedVersion;
        bool& needsEngineApply;
        bool& wroteParameter;
    };
    void renderWithParameterTimeline(fad::engine::AudioBufferView audioBuffer,
                                     midi::MidiEventSpan midiBuffer,
                                     RenderParameterTimeline& parameters,
                                     const SupplementalMidiEvent* supplementalMidiEvents = nullptr,
                                     size_t supplementalMidiEventCount = 0);
    void renderBypassTransition(fad::engine::AudioBufferView audioBuffer,
                                midi::MidiEventSpan midiBuffer,
                                int wetSampleCount,
                                const SupplementalMidiEvent* supplementalMidiEvents = nullptr,
                                size_t supplementalMidiEventCount = 0);
    void advanceBypassed(midi::MidiEventSpan midiBuffer,
                         int numSamples,
                         const SupplementalMidiEvent* supplementalMidiEvents = nullptr,
                         size_t supplementalMidiEventCount = 0);
    void processExternalOutputSafety(fad::engine::AudioBufferView buffer,
                                     bool clampToUnity = true) noexcept;
    float getLastRenderPeakLeft() const noexcept { return renderPeakLeft.load(std::memory_order_relaxed); }
    float getLastRenderPeakRight() const noexcept { return renderPeakRight.load(std::memory_order_relaxed); }

    // PDC: report FX chain latency to host
    int getFxLatencySamples() noexcept { return fxChain.getActiveLatencySamples(); }
    auto getFxGainReductionDbForUi() noexcept { return fxChain.getGainReductionDbForUi(); }
    // Control-thread owner leases; closing an old editor cannot end a newer one.
    uint64_t beginOscillatorUiCapture() noexcept;
    void endOscillatorUiCapture(uint64_t owner) noexcept;
    void requestOscillatorUiFrame(uint64_t owner) noexcept;
    bool tryReadOscillatorUiFrame(uint64_t owner, OscillatorUiFrame& result) const noexcept;
    bool hasPendingFxRebuildForControlThread() const noexcept;
    void tryApplyPendingFxRebuildOnControlThread() noexcept;
    bool hasPendingResetDeferredMidiOverflowLog() const noexcept
    {
        return pendingResetDeferredMidiOverflowLogCount.load(std::memory_order_acquire) != 0
            || pendingInputMidiOverflowLogCount.load(std::memory_order_acquire) != 0;
    }


    void setDiagnosticsEnabled(bool enabled) noexcept;
    diagnostics::DiagnosticsSnapshot getDiagnosticsSnapshot() const noexcept;



    wavetable::RuntimeWavetableBank& getWavetableBank() noexcept;
    const wavetable::RuntimeWavetableBank& getWavetableBank() const noexcept;
    const wavetable::RuntimeWavetableBank& peekWavetableBankForStateCapture() const noexcept;

    // Control-thread local resource transaction; refuses before publication
    // when the existing bounded retirement queue has no free entry.
    bool publishNoiseSampleSlot(int slotIndex, std::shared_ptr<const wavetable::WavetableData> asset);
    bool hasNoisePublicationRetireCapacity() const noexcept;
    size_t getNoiseRetiredAssetBytes() const noexcept;
    // Safe-thread entry point: queued wavetable transactions are committed
    // from non-audio callers, then retired through deferRelease().
    bool commitQueuedWavetableTransaction() noexcept;
    void adoptWavetableRetireList(wavetable::RuntimeWavetableBank::CommitRetireList retireList) noexcept;

    /// Per-session behavior metrics (audio thread writes, message thread reads)
    diagnostics::SessionMetrics& getSessionMetrics() noexcept { return sessionMetrics; }

    void testDeferRelease(std::shared_ptr<const void> ptr) noexcept { deferRelease(std::move(ptr)); }
    void testCollectGarbage() const noexcept { collectGarbage(); }
    bool testHasDeferredReleaseWorker() const noexcept { return deferredReleaseThread.joinable(); }

    int testGetDeferredReleaseReadyCount() const noexcept { return deferredReleaseFifo.getNumReady(); }
    int testGetDeferredReleaseOverflowReadyCount() const noexcept { return deferredReleaseOverflowFifo.getNumReady(); }
    int testGetDeferredReleaseQuarantineReadyCount() const noexcept
    {
        return deferredReleaseQuarantineFifo.getNumReady();
    }
    int testGetDeferredReleaseEmergencyReadyCount() const noexcept
    {
        return deferredReleaseEmergencyFifo.getNumReady();
    }
    int testGetDeferredReleaseSpilloverReadyCount() const noexcept
    {
        const std::scoped_lock lock(deferredReleaseDrainMutex);
        return static_cast<int>(deferredReleaseSpillover.size());
    }
    uint64_t testGetDeferredReleaseSpilloverCount() const noexcept
    {
        return deferredReleaseSpilloverCount.load(std::memory_order_acquire);
    }
    void testBlockDeferredReleaseWorker() noexcept
    {
        deferredReleaseWorkerBlocked.store(true, std::memory_order_release);
    }
    bool testWaitForDeferredReleaseWorkerBlocked(int timeoutMs) noexcept
    {
        return deferredReleaseWorkerBlockedEvent.wait(timeoutMs);
    }
    void testResumeDeferredReleaseWorker() noexcept
    {
        deferredReleaseWorkerBlocked.store(false, std::memory_order_release);
        deferredReleaseWorkerResumeEvent.signal();
    }
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    void testSetResolveBlockWavetableHook(std::function<void(size_t)> hook)
    {
        resolveBlockWavetableHook = std::move(hook);
    }
    void testSetDeferredReleaseFirstGuardSampleHook(std::function<void()> hook)
    {
        const std::scoped_lock lock(deferredReleaseDrainMutex);
        deferredReleaseFirstGuardSampleHook = std::move(hook);
    }
#endif
    void testSetDeferredReleaseRotateHook(std::function<void()> hook)
    {
        const std::scoped_lock lock(deferredReleaseDrainMutex);
        deferredReleaseRotateHook = std::move(hook);
    }
    const wavetable::WavetableData* testGetBlockWavetablePointer(size_t oscillatorIndex) const noexcept
    {
        return oscillatorIndex < blockWavetablePointers.size() ? blockWavetablePointers[oscillatorIndex]
                                                               : nullptr;
    }
    const wavetable::WavetableData* testGetBlockLfoCurveBank(size_t index) const noexcept
    {
        return index < blockLfoCurveBanks.size() ? blockLfoCurveBanks[index] : nullptr;
    }
    const wavetable::WavetableData* testGetBlockNoiseSample() const noexcept
    {
        return blockNoiseSample;
    }
    static constexpr int testGetDeferredReleaseCapacity() noexcept { return deferredReleaseUsableCapacity; }
    static constexpr int testGetDeferredReleaseOverflowCapacity() noexcept
    {
        return deferredReleaseOverflowUsableCapacity;
    }
    static constexpr int testGetDeferredReleaseQuarantineCapacity() noexcept
    {
        return deferredReleaseQuarantineUsableCapacity;
    }
    static constexpr int testGetDeferredReleaseEmergencyCapacity() noexcept
    {
        return deferredReleaseEmergencyUsableCapacity;
    }
    size_t testGetResetDeferredMidiCapacity() const noexcept
    {
        return resetDeferredMidi.size();
    }
    static constexpr size_t testGetMaxDeferredMidiEvents() noexcept
    {
        return maxDeferredMidiEvents;
    }
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    const parameters::ParameterSnapshot& testGetParameterSnapshot() const noexcept
    {
        return parameterSnapshot;
    }
    float testGetGlobalLfoSample(size_t index, parameters::LfoShape shape) const noexcept
    {
        return globalLfos[index].renderSampleAtOffset(shape, 0);
    }
    const fx::FxBackend& testGetDynamicFxChain() const noexcept
    {
        return fxChain;
    }
    // Offline/test readers must own the engine exclusively and finish before
    // the next render. This exposes no live cross-thread or owning reference.
    const modulation::SourceValues* testGetNativeFxFrames(int& start, int& count) const noexcept
    {
        start = fxModFrameStart;
        count = fxModFrameCount;
        return fxModFrameStart >= 0 && fxModFrameCount > 0 ? fxModSourceFrames.data() : nullptr;
    }
    float testMidiCiPitchBendRange(midi::Source source, int channel) noexcept
    {
        const auto previous = activeMidiSource; activeMidiSource = source;
        const auto range = effectivePitchBendRange(channel); activeMidiSource = previous; return range;
    }
    VoiceCore* testFindVoiceForRealtimeNote(int midiChannel, int midiNoteNumber) const noexcept
    {
        for (auto* voice : voiceCache)
            if (voice != nullptr && voice->matchesRealtimeNote(midiChannel, midiNoteNumber))
                return voice;

        return nullptr;
    }
    VoiceCore* testFindVoiceForSource(midi::Source source, int channel, int note) const noexcept
    {
        for (auto* voice : voiceCache)
            if (voice != nullptr && voice->getMidiSource() == source
                && voice->matchesRealtimeNote(channel, note)) return voice;
        return nullptr;
    }
    bool testQueuePendingParameterSnapshot(const parameters::ParameterSnapshot& snapshot) noexcept
    {
        return tryQueuePendingParameterSnapshot(snapshot, false);
    }
    void testRenderGlobalLfoBlock(int numSamples) noexcept
    {
        renderGlobalLfoBlock(numSamples);
    }
    const float* testGetGlobalLfoBlockPointer(size_t lfoIndex) const noexcept
    {
        return lfoIndex < globalLfoBlockPointers.size() ? globalLfoBlockPointers[lfoIndex] : nullptr;
    }
    void testSetPendingParameterSnapshotReadGate(std::atomic<bool>* shouldBlock,
                                                 fad::engine::detail::TestEventRef  entered,
                                                 fad::engine::detail::TestEventRef  allow) noexcept
    {
        blockPendingParameterSnapshotReadForTest = shouldBlock;
        pendingParameterSnapshotReadEnteredForTest = entered;
        allowPendingParameterSnapshotReadForTest = allow;
    }
    void testSetPendingParameterSnapshotWriteGate(std::atomic<bool>* shouldBlock,
                                                  fad::engine::detail::TestEventRef  entered,
                                                  fad::engine::detail::TestEventRef  allow) noexcept
    {
        blockPendingParameterSnapshotWriteForTest = shouldBlock;
        pendingParameterSnapshotWriteEnteredForTest = entered;
        allowPendingParameterSnapshotWriteForTest = allow;
    }
    void testSetPendingParameterSnapshotSequenceGate(std::atomic<bool>* shouldBlock,
                                                      fad::engine::detail::TestEventRef  entered,
                                                      fad::engine::detail::TestEventRef  allow) noexcept
    {
        blockPendingParameterSnapshotSequenceForTest = shouldBlock;
        pendingParameterSnapshotSequenceEnteredForTest = entered;
        allowPendingParameterSnapshotSequenceForTest = allow;
    }
    void testSetPendingParameterSnapshotPublishGate(std::atomic<bool>* shouldBlock,
                                                     fad::engine::detail::TestEventRef  entered,
                                                     fad::engine::detail::TestEventRef  allow) noexcept
    {
        blockPendingParameterSnapshotPublishForTest = shouldBlock;
        pendingParameterSnapshotPublishEnteredForTest = entered;
        allowPendingParameterSnapshotPublishForTest = allow;
    }

    void testResetFilterCoefficientUpdateCounts() noexcept
    {
        for (auto* voice : voiceCache)
            if (voice != nullptr)
                voice->testResetFilterCoefficientUpdateCounts();
    }
    int testGetTotalFilterCoefficientUpdateCount(size_t channelIndex) const noexcept
    {
        int total = 0;
        for (const auto* voice : voiceCache)
            if (voice != nullptr)
                total += voice->testGetFilterCoefficientUpdateCount(channelIndex);
        return total;
    }
#endif

protected:
    void shutdownAssetRetirement() noexcept;
    struct MonoHeldNote
    {
        int midiChannel = 1;
        int midiNoteNumber = -1;
        float velocity = 0.0f;
        midi::Source source = midi::Source::host;
        midi::UmpChannelVoice ump;
    };

    void renderChunk(fad::engine::AudioBufferView audioBuffer, int startSample, int numSamples);
    void renderInternal(fad::engine::AudioBufferView audioBuffer,
                        midi::MidiEventSpan midiBuffer,
                        int wetSampleCount,
                        const SupplementalMidiEvent* supplementalMidiEvents,
                        size_t supplementalMidiEventCount,
                        RenderParameterTimeline* renderParameters = nullptr);
    void renderGlobalLfoBlock(int numSamples) noexcept;
    bool modulationUsesLfo(size_t lfoIndex) const noexcept;
    void renderNativeFxModulation(int startSample, int numSamples) noexcept;
    void renderRateModulatedLfos(int startSample,int numSamples) noexcept;
    void advanceBypassRuntimeState(int numSamples);
    void dispatchInstrumentPitch(int value) noexcept;
    bool instrumentPitchActive = false;
    void dispatchMidiMessage(const midi::ShortMessage& message, midi::Source source = midi::Source::host, midi::UmpChannelVoice ump = {});
    void beginAllSoundOffFade() noexcept;
    void deferResetMidiMessage(const midi::ShortMessage& message,
                               int absoluteSampleOffset, midi::Source source = midi::Source::host, midi::UmpChannelVoice ump = {}) noexcept;
    bool hasDeferredResetNote(const midi::ShortMessage& message, midi::Source source) const noexcept;
    void releaseDeferredResetNote(const midi::ShortMessage& message, midi::Source source) noexcept;
    void removeDeferredResetNoteOns(int midiChannel, midi::Source source = midi::Source::host,
                                   size_t replayedCount = 0) noexcept;
    void handleMonoNoteOn(int midiChannel, int midiNoteNumber, float velocity);
    void handleMonoNoteOff(int midiChannel, int midiNoteNumber, float velocity);
    VoiceCore* findFreeVoice() noexcept;
    VoiceCore* findVoiceToSteal(int midiNoteNumber) noexcept;
    VoiceCore* findFreeVoiceOrVoiceToSteal(int midiNoteNumber) noexcept;
    void startVoice(VoiceCore* voice, int midiChannel, int midiNoteNumber, float velocity);
    void stopVoicesForNote(int midiChannel,
                           int midiNoteNumber,
                           float velocity,
                           bool allowTailOff, bool realNoteOff = true) noexcept;
    void stopVoicesForChannel(int midiChannel, bool allowTailOff) noexcept;
    void syncVoicePedalsForChannel(VoiceCore& voice, int midiChannel) noexcept;
    bool isFxModNotePedalHeld() const noexcept;
    void retireFxModNote() noexcept;
    void releaseFxModNote() noexcept;
    void handleSustainPedal(int midiChannel, bool isDown) noexcept;
    void handleSostenutoPedal(int midiChannel, bool isDown) noexcept;
    void removeHeldMonoNote(int midiChannel, int midiNoteNumber) noexcept;
    void recordHeldNoteOn(int midiChannel, int midiNoteNumber) noexcept;
    void recordHeldNoteOff(int midiChannel, int midiNoteNumber) noexcept;
    void resetHeldNoteState() noexcept;
    void resetHeldNoteStateForChannel(int midiChannel) noexcept;
    bool hasActiveVoices() const noexcept;
    bool hasHeldOrSustainedNotes() const noexcept;
    void resetMonoState() noexcept;
    void resetMonoStateForChannel(int midiChannel, bool stopVoices = true);
    void resetVoiceState() noexcept;
    void clearBypassInactiveAudioTails() noexcept;
    void applyParameterSnapshotNow(const parameters::ParameterSnapshot& snapshot,
                                   bool updateReadableActiveFxProcessors) noexcept;
    void applyParameterSnapshotNowOnControlThread(const parameters::ParameterSnapshot& snapshot) noexcept;
    void alignSyncedLfosToTransport(double ppqPosition) noexcept;
    bool tryQueuePendingParameterSnapshot(const parameters::ParameterSnapshot& snapshot,
                                          bool mayApplyControlThreadFxRebuild) noexcept;
    uint64_t prepareDynamicFxSnapshotForDeferredAcceptanceOnControlThread(
        const parameters::DynamicFxSnapshot& snapshot) noexcept;
    void invalidatePendingParameterSnapshots() noexcept;
    void applyPendingParameterSnapshotFromAudioThread() noexcept;
    void resolveBlockWavetables() noexcept;
    float resolveLfoRateHz(const parameters::LfoSnapshot& snapshot) const noexcept;
    void propagateSnapshotToVoices(bool diagnosticsForThisBlock) noexcept;
    void applyDynamicFxSnapshot(const parameters::DynamicFxSnapshot& snapshot,
                                bool forceAll = false,
                                bool updateReadableActiveFxProcessors = true,
                                bool recordAsApplied = true,
                                bool requireExplicitPublicationAcceptance = false);
    bool tryPrepareDynamicFxSnapshotForAudioThread(
        const parameters::DynamicFxSnapshot& snapshot) noexcept;
    void recordAppliedDynamicFxSnapshot(
        const parameters::DynamicFxSnapshot& snapshot) noexcept;
    void processFx(fad::engine::AudioBufferView audioBuffer, bool diagEnabled);
    void applyOutputGain(fad::engine::AudioBufferView audioBuffer);
    void resetInternal(bool drainDeferredRelease) noexcept;
    static int tryClaimSnapshotWriteSlot(std::array<std::atomic<int>, 3>& states,
                                         const std::atomic<int>& readIndex) noexcept;
    VoiceCore* prepareOscillatorUiCapture() noexcept;
    void finishOscillatorUiCapture(VoiceCore*, int samples) noexcept;
    void invalidateOscillatorUiCapture() noexcept;
    bool publishOscillatorUiFrame() noexcept;
    int tryClaimDiagnosticsWriteSlot() noexcept;
    void publishDiagnosticsWriteSlot(int slotIndex) noexcept;
    void buildDiagnosticsSnapshot(fad::engine::AudioBufferView audioBuffer);
    void releaseBlockWavetableOwnership(bool forceRelease = false) noexcept;
    // deferRelease stores activeAudioReaders and pendingPublishedOwnershipReaders
    // inside DeferredReleaseEntry until that entry is consumed and reset() runs.
    // Callers must only pass pointers to atomics whose lifetime exceeds the
    // queued DeferredReleaseEntry; never pass stack or temporary storage here.
    bool hasDeferredReleaseWorkPending() const noexcept;
    void ensureDeferredReleaseWorkerStarted() noexcept;
    void deferRelease(std::shared_ptr<const void> ptr,
                      const std::atomic<uint32_t>* activeAudioReaders = nullptr,
                      const std::atomic<uint32_t>* pendingPublishedOwnershipReaders = nullptr) noexcept;
    bool collectGarbage() const noexcept;

    struct DeferredReleaseEntry
    {
        std::shared_ptr<const void> object;
        const std::atomic<uint32_t>* activeAudioReaders = nullptr;
        const std::atomic<uint32_t>* pendingPublishedOwnershipReaders = nullptr;
        size_t noiseAssetBytes = 0;

        void reset() noexcept
        {
            object.reset();
            activeAudioReaders = nullptr;
            pendingPublishedOwnershipReaders = nullptr;
            noiseAssetBytes = 0;
        }
    };

    struct DeferredMidiEvent
    {
        midi::ShortMessage message;
        int absoluteSampleOffset = 0;
        int orderKey = 0;
        midi::Source source = midi::Source::host;
        midi::UmpChannelVoice ump;
    };

    enum class PendingParameterSnapshotState : uint8_t
    {
        empty = 0,
        writing,
        ready,
        reading
    };

    void dispatchUmpMessage(const midi::UmpChannelVoice&, midi::Source);
    void* midiObserverOwner=nullptr;
    MidiObserver midiObserver=nullptr;
    MidiNoteInitialiser midiNoteInitialiser=nullptr;
    MidiSourceResetter midiSourceResetter=nullptr;
    std::array<float,32> fxLearnedMidiControllers{};
    bool dispatchingUmp=false;
    float umpVelocityOverride=-1.0f;
    struct Midi2NoteState { float pressure=0, slide=.5f, pitch=.5f, tuning=-1; bool slideReceived=false; };
    std::vector<Midi2NoteState> midi2NoteStates{midi::sourceCount * 16 * 128};
    std::array<bool,midi::sourceCount> midi2SourcesUsed{};
    std::vector<std::unique_ptr<VoiceCore>> ownedVoices;
    std::vector<VoiceCore*> voiceCache;
    fad::engine::detail::PlanarBuffer bypassAdvanceScratchBuffer;
    fad::engine::detail::PlanarBuffer directAuxScratch;
    parameters::ParameterSnapshot parameterSnapshot;
    // Control/lifecycle only: stage and prepare require exclusive ownership.
    bool stagedIsolatedInitialSnapshotPending = false;
    // Snapshot-only coefficients; voice smoothing, modulation and progression
    // remain independent. Invalidated at every accepted snapshot publication.
    std::optional<VoiceCore::RenderContext> voiceRenderContext;
    static constexpr size_t pendingParameterSnapshotSlotCount = 3;
    std::array<parameters::ParameterSnapshot, pendingParameterSnapshotSlotCount> pendingParameterSnapshots;
    std::array<std::atomic<PendingParameterSnapshotState>, pendingParameterSnapshotSlotCount>
        pendingParameterSnapshotStates{};
    std::array<std::atomic<uint64_t>, pendingParameterSnapshotSlotCount>
        pendingParameterSnapshotSequences{};
    std::atomic<uint64_t> pendingParameterSnapshotNextSequence{1};
    std::atomic<uint64_t> latestPendingParameterSnapshotToken{0};
    parameters::DynamicFxSnapshot appliedDynamicFxSnapshot;
    bool hasAppliedDynamicFxSnapshot = false;
    parameters::DynamicFxSnapshot pendingAudioThreadDynamicFxSnapshot;
    bool hasPendingAudioThreadDynamicFxSnapshot = false;
    std::array<std::atomic<bool>, pendingParameterSnapshotSlotCount>
        pendingParameterSnapshotMayApplyControlThreadFxRebuild{};
    std::array<std::atomic<uint64_t>, pendingParameterSnapshotSlotCount>
        pendingParameterSnapshotPreparedFxSequences{};
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    std::atomic<bool>* blockPendingParameterSnapshotReadForTest = nullptr;
    fad::engine::detail::TestEventRef  pendingParameterSnapshotReadEnteredForTest = nullptr;
    fad::engine::detail::TestEventRef  allowPendingParameterSnapshotReadForTest = nullptr;
    std::atomic<bool>* blockPendingParameterSnapshotWriteForTest = nullptr;
    fad::engine::detail::TestEventRef  pendingParameterSnapshotWriteEnteredForTest = nullptr;
    fad::engine::detail::TestEventRef  allowPendingParameterSnapshotWriteForTest = nullptr;
    std::atomic<bool>* blockPendingParameterSnapshotSequenceForTest = nullptr;
    fad::engine::detail::TestEventRef  pendingParameterSnapshotSequenceEnteredForTest = nullptr;
    fad::engine::detail::TestEventRef  allowPendingParameterSnapshotSequenceForTest = nullptr;
    std::atomic<bool>* blockPendingParameterSnapshotPublishForTest = nullptr;
    fad::engine::detail::TestEventRef  pendingParameterSnapshotPublishEnteredForTest = nullptr;
    fad::engine::detail::TestEventRef  allowPendingParameterSnapshotPublishForTest = nullptr;
#endif
    fx::FxBackend& fxChain;
    fad::engine::RenderServiceHooks serviceHooks;
    std::array<const wavetable::WavetableData*, parameters::lfoCount> blockLfoCurveBanks{};
    const wavetable::WavetableData* blockNoiseSample = nullptr;
    NoiseSamplePlayer fxNoisePlayer;
    uint32_t fxNoiseState = 0x6d2b79f5u;
    int fxNoisePhase = 0;
    float fxNoiseValue = 0;
    float renderFxNoiseSource(bool active) noexcept;
    std::array<modulation::Lfo, parameters::lfoCount> globalLfos;
    std::array<modulation::Lfo, parameters::lfoCount> fxNoteLfos;
    std::array<modulation::NativeLfoCurveRuntime, parameters::lfoCount> nativeCurveRuntimes;
    std::array<modulation::CurveNodesRuntime, parameters::lfoCount> fxNodeLfos;
    uint32_t fxNodeLfoMask = 0;
    int fxNodeCurveCounter = 0;
    void prepareFxNodeLfos() noexcept;
    void updateFxNodeLfos(const std::array<float, parameters::lfoCount>& sources) noexcept;
    std::array<modulation::HoldEnvelope, 3> fxModEnvelopes;
    std::vector<modulation::SourceValues> fxModSourceFrames;
    int fxModFrameStart = -1;
    int fxModFrameCount = 0;
    int fxModChannel = 1;
    int fxModNote = -1;
    float fxModRawVelocity = 0.0f;
    float fxModVelocity = 0.0f;
    float fxModReleaseVelocity = 0.0f;
    float fxModPolyPressure = 0.0f;
    bool fxMidi2Active=false,fxMidi2Detached=false,fxMidi2SlideActive=false;
    float fxMidi2Pitch=.5f,fxMidi2Slide=.5f;
    bool fxModControlsFrozen = false;
    VoiceCore::PerformanceState fxModReleasedPerformance;
    float fxModReleasedPitch=.5f;
    float fxModReleasedBendRange = 48.0f;
    bool fxModReleasePending = false;
    bool hasNativeFxRoutes = false;
    bool hasNativeFxClock = false;
    bool hasEqCurveModulation = false;
    bool hasNativeSustainRoutes = false;
    bool hasNativeDecayRoutes = false;
    bool hasNativeReleaseRoutes = false;
    uint32_t nativeRateRouteMask = 0;
    std::array<modulation::Lfo,parameters::lfoCount> modulatedGlobalLfos;
    std::array<std::vector<float>, parameters::lfoCount> globalLfoBlockStorage;
    std::array<bool, parameters::lfoCount> globalLfoUsed{};
    std::array<const float*, parameters::lfoCount> globalLfoBlockPointers{};
    struct MidiSourceState
    {
        std::array<VoiceCore::PerformanceState, 16> performanceStates;
        std::array<std::array<uint16_t, 128>, 16> heldNoteCounts{};
        std::array<bool, 16> sustainPedalDown{};
        std::array<int, 16> lastPitchWheelValues{};
        std::array<float, 16> pitchWheelBipolar{};
        std::array<midici::ChannelState, 16> ciReportValues{};
        std::array<float, 16> perNoteBendRanges{};
        std::array<float, 16> pitchBendRangeOverrides{};
        std::array<uint8_t, 16> rpnMsb{};
        std::array<uint8_t, 16> rpnLsb{};
        std::array<uint8_t, 16> bendRangeCoarse{};
        struct MpeZoneState
        {
            int members = 0;
            float managerBend = -1.0f, memberBend = -1.0f;
            uint8_t memberBendCoarse = 0;
        };
        std::array<MpeZoneState, 2> mpeZones{{{15}, {0}}};
        std::array<uint8_t, 16> ciProfileCounts{};
        std::array<float, 16> ciProfileBendRanges{};
        std::array<uint8_t, 16> ciProfileBendCoarse{};
        bool ciProfileActive = false;
        std::array<bool, 16> localSustainPedalDown{};
        std::array<bool, 16> localSostenutoPedalDown{};
        std::array<bool, 16> effectiveSostenutoPedalDown{};
        bool appliedMpeEnabledForPedals = false;
        std::array<float, 16> lastNativeNoteFrequency{};
        MidiSourceState() noexcept
        {
            lastPitchWheelValues.fill(8192);
            perNoteBendRanges.fill(48.0f);
            pitchBendRangeOverrides.fill(-1.0f);
            rpnMsb.fill(127); rpnLsb.fill(127);
        }
    };
    std::vector<MidiSourceState> midiSourceStates{midi::sourceCount};
    midi::Source activeMidiSource = midi::Source::host;
    midi::Source fxModSource = midi::Source::host;
    midi::Source currentMonoSource = midi::Source::host;
    MidiSourceState& midiState() noexcept { return midiSourceStates[static_cast<size_t>(activeMidiSource)]; }
    const MidiSourceState& midiState() const noexcept { return midiSourceStates[static_cast<size_t>(activeMidiSource)]; }
    bool mpeEnabledForCurrentSource() const noexcept
    { return activeMidiSource != midi::Source::touch && (midiState().ciProfileActive || parameterSnapshot.global.mpeEnabled); }
    bool ownsVoice(const VoiceCore* voice) const noexcept
    { return voice != nullptr && voice->getMidiSource() == activeMidiSource; }
    void attachVoiceSource(VoiceCore* voice) noexcept
    {
        if (voice->getMidiSource() != activeMidiSource) voice->setSostenutoPedalDown(false);
        voice->setMidiSource(activeMidiSource);
        voice->setPerformanceStates(&midiState().performanceStates);
    }
    bool isMpeMemberChannel(int midiChannel) const noexcept;
    int mpeManagerChannelFor(int midiChannel) const noexcept;
    bool isMpeManagerChannel(int midiChannel) const noexcept;
    void configureMpeZone(int managerChannel, int members) noexcept;
    float effectivePitchBendRange(int midiChannel) const noexcept;
    void applyMpeManagerControls(VoiceCore* voice, bool snap) noexcept;
    void refreshMpeManagerControls() noexcept;
    void applySustainController(int midiChannel, bool down) noexcept;
    void applySostenutoController(int midiChannel, bool down) noexcept;
    void refreshMpePedals() noexcept;
    std::atomic<int> totalHeldNoteCount{0};
    std::array<MonoHeldNote, 128> monoHeldNotes;
    size_t monoHeldNoteCount = 0;
    int currentMonoChannel = 1;
    int currentMonoNote = -1;
    int monoVoiceStartedChannel = 1;
    int monoVoiceStartedNote = -1;
    VoiceCore* activeMonoVoice = nullptr;
    uint64_t voiceStartCounter = 0;
    oscillators::SerumPhaseRandom nativePhaseRandom;
    modulation::NoteOnModulation noteOnModulation;
    uint32_t noteOnModulationSeed = 0;
    std::array<float,4> fxNoteOnValues{};
    parameters::VoiceMode activeVoiceMode = parameters::VoiceMode::poly;
    double tempoBpm = 120.0;
    bool wasPlaying = false;
    double lastTransportPpqPosition = 0.0;
    bool hasLastTransportPpqPosition = false;
    double lastTransportPpqAdvance = 0.0;
    fad::engine::math::LinearSmoother<float> outputGainSmoother;
    bool outputGainNeedsInitialSnapshot = true;
    std::atomic<float> renderPeakLeft{0.0f};
    std::atomic<float> renderPeakRight{0.0f};
    std::atomic<bool> realtimeProcessing{true};

    static constexpr size_t diagnosticsSnapshotSlotCount = 3;
    static constexpr int diagnosticsWriterClaim = -1;
    std::atomic<uint64_t> oscillatorUiNextOwner{0}, oscillatorUiOwner{0}, oscillatorUiRequest{0};
    std::atomic<uint64_t> oscillatorUiContext{1};
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::array<OscillatorUiFrame, diagnosticsSnapshotSlotCount> oscillatorUiFrames{};
    mutable std::array<std::atomic<int>, diagnosticsSnapshotSlotCount> oscillatorUiSlotStates{};
    std::atomic<int> oscillatorUiReadIndex{0};
    OscillatorUiFrame oscillatorUiWorking;
    uint64_t oscillatorUiServicedRequest = 0, oscillatorUiSequence = 0;
    int oscillatorUiCursor = 0;
    bool oscillatorUiCollecting = false, oscillatorUiPublishedActive = false;
    std::array<diagnostics::DiagnosticsSnapshot, diagnosticsSnapshotSlotCount>
        diagnosticsSnapshots;
    diagnostics::SignalFlowSnapshot cachedSignalFlowSnapshot;
    mutable std::array<std::atomic<int>, diagnosticsSnapshotSlotCount>
        diagnosticsSlotStates{};
    std::atomic<int> diagnosticsReadIndex{0};
    diagnostics::CpuMeter voicesCpuMeter;
    std::vector<diagnostics::CpuMeter> fxCpuMeters;
    diagnostics::CpuMeter totalCpuMeter;
    diagnostics::ClipDetector clipDetector;
    std::atomic<bool> diagnosticsEnabled{false};
    std::atomic<int> diagnosticsRefreshCountdown{0};
    std::atomic<bool> resetRequested{false};
    static constexpr size_t maxDeferredMidiEvents = 1024;
    std::array<DeferredMidiEvent, maxDeferredMidiEvents> resetDeferredMidi;
    struct DeferredResetNote
    {
        midi::Source source = midi::Source::host;
        int channel = 0;
        int note = 0;
        uint32_t count = 0;
    };
    std::array<DeferredResetNote, maxDeferredMidiEvents> resetDeferredNotes{};
    size_t resetDeferredNoteCount = 0;
    size_t resetDeferredMidiCount = 0;
    bool resetDeferredMidiOverflowed = false;
    midi::MidiEventSpan emptyRenderMidi;
    std::atomic<uint32_t> pendingResetDeferredMidiOverflowLogCount{0};
    std::atomic<uint32_t> pendingInputMidiOverflowLogCount{0};
    bool inputMidiOverflowPanicPending = false;
    std::atomic<int> lastResetDeferredMidiOverflowCount{0};
    std::atomic<int> lastResetDeferredMidiOverflowCapacity{0};
    std::atomic<int> lastResetDeferredMidiOverflowOrderKey{0};
    int resetDeferredMidiBlockBase = 0;
    int resetDeferredMidiReplayBase = 0;
    std::atomic<int> resetFadeRemainingSamples{0};
    int resetFadeTotalSamples = 0;
    int resetFadeTotalAtStartSamples = 0;
    std::atomic<bool> renderedAudibleState{false};
    int allSoundOffSamplesRemaining = 0;
    fad::engine::math::LinearSmoother<float> allSoundOffOutputGain{1.0f};
    static constexpr int diagnosticsRefreshIntervalBlocks = 8;
    std::atomic<bool> signalFlowDirty{true};
    double lastSampleRate = 48000.0;
    int lastBlockSize = 512;
    static constexpr int deferredReleaseStorageSize = 256;
    static constexpr int deferredReleaseOverflowStorageSize = 64;
    static constexpr int deferredReleaseUsableCapacity = deferredReleaseStorageSize - 1;
    static constexpr int deferredReleaseOverflowUsableCapacity = deferredReleaseOverflowStorageSize - 1;
    static constexpr int deferredReleaseQuarantineStorageSize = 32;
    static constexpr int deferredReleaseQuarantineUsableCapacity = deferredReleaseQuarantineStorageSize - 1;
    static constexpr int deferredReleaseEmergencyStorageSize = 8;
    static constexpr int deferredReleaseEmergencyUsableCapacity = deferredReleaseEmergencyStorageSize - 1;
    mutable fad::engine::rt::SpscCursor<deferredReleaseStorageSize> deferredReleaseFifo{};
    mutable std::array<DeferredReleaseEntry,
                       static_cast<size_t>(deferredReleaseStorageSize)> deferredReleaseBin{};
    mutable fad::engine::rt::SpscCursor<deferredReleaseOverflowStorageSize> deferredReleaseOverflowFifo{};
    mutable std::array<DeferredReleaseEntry,
                       static_cast<size_t>(deferredReleaseOverflowStorageSize)> deferredReleaseOverflowBin{};
    mutable fad::engine::rt::SpscCursor<deferredReleaseQuarantineStorageSize> deferredReleaseQuarantineFifo{};
    mutable std::array<DeferredReleaseEntry,
                       static_cast<size_t>(deferredReleaseQuarantineStorageSize)> deferredReleaseQuarantineBin{};
    mutable fad::engine::rt::SpscCursor<deferredReleaseEmergencyStorageSize> deferredReleaseEmergencyFifo{};
    mutable std::array<DeferredReleaseEntry,
                       static_cast<size_t>(deferredReleaseEmergencyStorageSize)> deferredReleaseEmergencyBin{};
    mutable std::vector<DeferredReleaseEntry> deferredReleaseSpillover;
    std::atomic<uint64_t> deferredReleaseSpilloverCount{0};
    mutable std::mutex deferredReleaseDrainMutex;
    mutable fad::engine::detail::EventSignal deferredReleaseWorkAvailable;
    mutable std::thread deferredReleaseThread;
    std::atomic<bool> deferredReleaseThreadShouldExit{false};
    std::atomic<bool> deferredReleaseWorkerBlocked{false};
    mutable fad::engine::detail::EventSignal deferredReleaseWorkerBlockedEvent;
    mutable fad::engine::detail::EventSignal deferredReleaseWorkerResumeEvent;
    const wavetable::RuntimeWavetableBank::PublishedOwnership* blockPublishedOwnership = nullptr;
    std::array<const wavetable::WavetableData*, 2> blockWavetablePointers{nullptr, nullptr};
    uint32_t blockWavetableGeneration = 0;
    std::atomic<const std::atomic<bool>*> stateRestoreGate{nullptr};
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    std::function<void(size_t)> resolveBlockWavetableHook;
    std::function<void()> deferredReleaseFirstGuardSampleHook;
#endif
    std::function<void()> deferredReleaseRotateHook;

    wavetable::RuntimeWavetableBank& wavetableBank;
    fad::engine::AudioSafetyStage audioSafetyStage;
    fad::engine::OutputPeakLimiter outputPeakLimiter;
    diagnostics::SessionMetrics sessionMetrics;
};
}
