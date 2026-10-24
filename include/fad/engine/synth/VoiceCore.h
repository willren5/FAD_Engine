// Copyright (c) 2024-2026 阿勒泰自由意文化服务有限责任公司 (FAD Records). All rights reserved.
// Licensed under the openFAD 0.1 or a separate Commercial License. See LICENSE.

#pragma once

#include <fad/engine/AudioView.h>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/math/Numeric.h>
#include <optional>
#include <cassert>
#include <numbers>
#include <type_traits>
#include <fad/engine/midi/MidiSource.h>
#include <fad/engine/oscillators/SerumPhaseRandom.h>
#include <fad/engine/modulation/HoldEnvelope.h>
#include <fad/engine/synth/NativeGlide.h>

#include <fad/engine/modulation/Lfo.h>
#include <fad/engine/modulation/ModMatrix.h>
#include <fad/engine/parameters/ParameterSnapshot.h>
#include <fad/engine/filters/FilterBlock.h>
#include <fad/engine/oscillators/OscillatorBlock.h>
#include <fad/engine/oscillators/NoiseSamplePlayer.h>
#include <fad/engine/diagnostics/ModulationTrace.h>
#include <fad/engine/assets/WavetableBank.h>

namespace fadmodel1::dsp::synth
{
// Actual per-oscillator stereo contributions before filter/envelope/FX. Fixed
// storage only: the audio writer never shares a live Voice or asset pointer.
struct OscillatorUiFrame
{
    static constexpr int sampleCount = 256;
    std::array<std::array<std::array<float, sampleCount>, 2>, 2> samples{};
    uint64_t owner = 0, sequence = 0, context = 0, voiceStartOrder = 0;
    std::array<float, 2> positions{};
    std::array<int, 2> slots{}, modes{};
    uint32_t tableGeneration = 0;
    double sampleRate = 48000.0;
    int note = -1, channel = 0;
    midi::Source source = midi::Source::host;
    bool active = false;
    bool finite = true;
};
static_assert(std::is_trivially_copyable_v<OscillatorUiFrame>);

class VoiceCore
{
public:
    // Shared read-only presentation uses the exact render-path reference.
    static constexpr float untrackedOscillatorBaseHz = 8.1757989156f;
    static float untrackedTunedBaseHz(const parameters::OscillatorSnapshot&,
                                      const parameters::GlobalSnapshot&) noexcept;
    // Deterministic stopped-state projection at A3 / velocity 1. Playback uses
    // actual captured Voice samples. No live voice or oscillator is advanced.
    static std::array<parameters::OscillatorSnapshot,2> referenceOscillators(
        const parameters::ParameterSnapshot&, std::array<float,2>* frequencies = nullptr) noexcept;
    struct PerformanceState
    {
        float modWheel = 0.0f;
        float aftertouch = 0.0f;
        float channelVolume = 1.0f;
        float expression = 1.0f;
        float slide = 0.0f;
        bool slideReceived = false;
        float combinedSlide(float member) const noexcept {
            return std::clamp(member + (slideReceived ? slide - 64.0f / 127.0f : 0.0f), 0.0f, 1.0f);
        }
    };

    VoiceCore();
    bool isKeyDown() const noexcept { return noteState.keyDown; }
    void setKeyDown(bool down) noexcept { noteState.keyDown = down; }
    bool isSustainPedalDown() const noexcept { return noteState.sustain; }
    void setSustainPedalDown(bool down) noexcept { noteState.sustain = down; }
    bool isSostenutoPedalDown() const noexcept { return noteState.sostenuto; }
    void setSostenutoPedalDown(bool down) noexcept { noteState.sostenuto = down; }


    void prepare(const fad::engine::ProcessSpec& spec);
    void reset();
    void hardReset() noexcept;
    void setParameterSnapshot(const parameters::ParameterSnapshot* snapshot) noexcept;
    void setTransportBpm(double bpm) noexcept { transportTempoBpm=std::clamp(std::isfinite(bpm)?bpm:120.0, 20.0, 400.0); }
    void setGlobalLfos(
        const std::array<modulation::Lfo, parameters::lfoCount>* lfos) noexcept;
    void setGlobalLfoBlockValues(
        const std::array<const float*, parameters::lfoCount>* blockValues) noexcept;
    void setPerformanceStates(const std::array<PerformanceState, 16>* states) noexcept;

    // Borrowed only for the synchronous audio render segment below.
    void setOscillatorUiCapture(OscillatorUiFrame* frame, int offset = 0) noexcept
    { oscillatorUiCapture = frame; oscillatorUiCaptureOffset = offset; }
    void setDiagnosticsEnabled(bool enabled) noexcept;
    const diagnostics::ModulationTraceSnapshot& getLastModulationTrace() const noexcept;
    void setBlockWavetablePointers(
        const std::array<const wavetable::WavetableData*, 2>* pointers,
        uint32_t generation) noexcept;
    void setBlockLfoCurveBanks(const std::array<const wavetable::WavetableData*, parameters::lfoCount>* banks) noexcept { blockLfoCurveBanks = banks; }
    void setNoiseSeed(uint32_t seed) noexcept;
    void setNativePhaseRandomValues(const oscillators::SerumPhaseRandomValues& values) noexcept
    { nativePhaseRandomValues = values; hasPendingNativePhaseRandomValues = true; }
    void setDirectOutBuffer(fad::engine::AudioBufferView buffer, int origin = 0) noexcept
    { directOutBuffer = buffer; directOutOrigin = origin; }
    void clearDirectOutBuffer() noexcept { directOutBuffer.reset(); directOutOrigin = 0; }
    bool needsDirectOutBus() const noexcept
    {
        if (! isVoiceActive()) return false;
        return continuousSubDirect > 0.0f || continuousNoiseDirect > 0.0f
            || (stealTailRemaining > 0 && (stealDirectTailSample[0] != 0.0f || stealDirectTailSample[1] != 0.0f))
            || (structuralFadeRemaining > 0 && (structuralDirectFadeStart[0] != 0.0f || structuralDirectFadeStart[1] != 0.0f));
    }
    void setBlockNoiseSample(const wavetable::WavetableData* sample,
        const wavetable::RuntimeWavetableBank::PublishedOwnership* owner = nullptr) noexcept
    { blockNoiseSample = sample; blockNoiseOwnership = owner; }
    void releaseNoiseAssetViews() noexcept { noiseSamplePlayer.releaseAssetViews(); }
    void resetRandomState() noexcept;
    void setRealtimeUnisonVoiceLimit(int maxVoices) noexcept;

    // Legato: change pitch without retriggering envelopes
    void glideToNote(int newMidiNoteNumber, float newVelocity) noexcept;
    void glideToRealtimeNote(int midiChannel, int newMidiNoteNumber, float newVelocity) noexcept;

    void initialiseNote(int newMidiNoteNumber,
                   float velocity,
                   int currentPitchWheelPosition);
    void stopNote(float velocity, bool allowTailOff);
    void pitchWheelMoved(int newPitchWheelValue);
    void controllerMoved(int controllerNumber, int newControllerValue);
    void aftertouchChanged(int newAftertouchValue);
    void channelPressureChanged(int newChannelPressureValue);
    void renderNextBlock(fad::engine::AudioBufferView outputBuffer,
                         int startSample,
                         int numSamples);
    static uint64_t classifyRenderRouteBits(const parameters::ModulationSnapshot&, bool) noexcept;
    struct RenderContext;
    void renderNextBlockWithContext(fad::engine::AudioBufferView, int, int, const RenderContext*);
    bool isPlayingChannel(int midiChannel) const;

    void startRealtimeNote(int midiChannel,
                           int newMidiNoteNumber,
                           float velocity,
                           int currentPitchWheelPosition,
                           uint64_t startOrder,
                           bool sustainPedalIsDown);
    void stopRealtimeNote(float velocity, bool allowTailOff);
    void recordNoteOffVelocity(float velocity) noexcept
    { releaseVelocity = std::clamp(velocity, 0.0f, 1.0f); }
    void releasePedalHeldNote() { stopRealtimeNote(releaseVelocity, true); }
    void resumeNativeReleasedNote(float velocity, uint64_t startOrder) noexcept;
    bool canResumeReleasedNote() const noexcept
    { return (ampEnvelope.isActive() || nativeReleaseResumeGraceRemaining > 0)
          && stealFadeRemaining == 0 && !isKeyDown()
          && !isSustainPedalDown() && !isSostenutoPedalDown(); }
    void resetRealtimeState() noexcept;
    bool matchesRealtimeNote(int midiChannel, int noteNumber) const noexcept;
    void setMidiSource(midi::Source source) noexcept { noteState.source = source; }
    midi::Source getMidiSource() const noexcept { return noteState.source; }
    int getRealtimeMidiNote() const noexcept { return noteState.number; }
    int getRealtimeMidiChannel() const noexcept { return noteState.channel; }
    uint64_t getRealtimeStartOrder() const noexcept { return noteState.startOrder; }

    void setPitchBendRange(float semitones) noexcept;
    void setMidi2ChannelPitch(float bipolar) noexcept;
    void setMidi2PolyPressure(float value) noexcept { if(!midi2Detached) polyAftertouchValue=std::clamp(value, 0.0f, 1.0f); }
    void setMidi2ChannelPressure(float value) noexcept { if(!mpeMember || isKeyDown()) channelPressureValue=std::clamp(value, 0.0f, 1.0f); }
    void syncMidi2ChannelControls() noexcept;
    void setMidi2Slide(float value) noexcept { if(!midi2Detached) { midi2SlideActive=true; midi2Slide=std::clamp(value, 0.0f, 1.0f); } }
    void setMidi2NotePitch(float bipolar) noexcept { if(!midi2Detached) { midi2PitchActive=true; midi2NoteBend=bipolar*midi2NoteBendRange; midi2PitchValue=.5f+.5f*bipolar; } }
    void setMidi2NoteBendRange(float range) noexcept
    { if(!midi2Detached) { midi2NoteBendRange=range; midi2NoteBend=(midi2PitchValue-.5f)*2.0f*range; } }
    void setMidi2Tuning(float pitch) noexcept { if(!midi2Detached) midi2TuningOffset=pitch-static_cast<float>(noteState.number); }
    void beginMidi2Note(float bendRange, float pitch) noexcept
    { midi2Detached=false; midi2PitchActive=false; midi2SlideActive=false; midi2ChannelControls=false; midi2NoteBend=0; midi2TuningOffset=0; learnedMidiControllers.fill(0);
      midi2NoteBendRange=bendRange; if(pitch>=0) setMidi2Tuning(pitch); }
    void setLearnedMidiController(int slot,float value) noexcept
    { if(!midi2Detached && slot>=0 && slot<32) learnedMidiControllers[static_cast<size_t>(slot)]=value; }
    void detachMidi2Note() noexcept { midi2Detached=true; }
    void resetMidi2NoteControllers() noexcept
    { if(!midi2Detached) { polyAftertouchValue=0; midi2Slide=.5f; midi2SlideActive=false;
      midi2PitchValue=.5f; midi2NoteBend=0; midi2TuningOffset=0; learnedMidiControllers.fill(0); } }

    void setMpeMember(bool member, int managerChannel = 1) noexcept
    { mpeMember = member; mpeManagerChannel = std::clamp(managerChannel, 1, 16); }
    void setMpeManagerControls(float volume, float expression,
                               float pitchSemitones, bool snap) noexcept;
    void setGlideTime(float timeMs) noexcept { glideTimeSeconds = timeMs * 0.001f; }
    void setGlideMode(parameters::GlideMode mode) noexcept { glideMode = mode; }
    void setNativeGlideStartFrequency(float frequency) noexcept { pendingNativeGlideFrequency = frequency; }
    void setNoteOnModulationSources(const std::array<float,4>& values) noexcept { nativeNoteOnValues = values; }

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    float testGetPrimaryOscillatorPhase01(size_t index) const noexcept
    { return oscillators[index].testGetPhase01(); }

    float testGetTargetFrequencyHz() const noexcept
    {
        return baseFrequencySmoother.getTargetValue();
    }

    const std::array<float, parameters::maxRenderedUnisonVoiceCount>& testGetUnisonRandomDetuneCents(size_t oscillatorIndex) const noexcept
    {
        assert(oscillatorIndex < unisonRandomDetuneCents.size());
        return unisonRandomDetuneCents[oscillatorIndex];
    }

    int testGetRenderedUnisonCount(int requestedVoiceCount) const noexcept
    {
        return getRenderedUnisonCount(requestedVoiceCount);
    }

    const std::array<int, 2>& testGetRenderedUnisonVoiceCounts() const noexcept
    {
        return renderedUnisonVoiceCounts;
    }

    void testRefreshRenderedUnisonVoiceCounts() noexcept
    {
        refreshRenderedUnisonVoiceCounts();
    }

    float testGetUnisonOscillatorPhase01(size_t oscillatorIndex, size_t unisonIndex) const noexcept
    {
        assert(oscillatorIndex < unisonOscillators.size());
        assert(unisonIndex < unisonOscillators[oscillatorIndex].size());
        return unisonOscillators[oscillatorIndex][unisonIndex].testGetPhase01();
    }

    int testGetFilterCoefficientUpdateCount(size_t channelIndex) const noexcept
    {
        assert(channelIndex < filters.size());
        return filters[channelIndex].testGetCoefficientUpdateCount();
    }

    void testResetFilterCoefficientUpdateCounts() noexcept
    {
        for (auto& filter : filters)
            filter.testResetCoefficientUpdateCount();
    }

    parameters::OscillatorSnapshot testCreateRenderedOscillatorSnapshot(
        size_t oscillatorIndex,
        const modulation::DestinationValues& modulationValues) const noexcept
    {
        return createRenderedOscillatorSnapshot(oscillatorIndex, modulationValues);
    }

    const modulation::DestinationValues& testGetSmoothedModulationValues() const noexcept
    {
        return smoothedModValues;
    }

    parameters::OscillatorSnapshot testGetContinuousOscillatorParameters(
        size_t oscillatorIndex) const noexcept
    {
        parameters::OscillatorSnapshot snapshot;
        applyContinuousOscillatorParameters(oscillatorIndex, snapshot);
        return snapshot;
    }

    float testGetContinuousSubLevel() const noexcept { return continuousSubLevel; }
    float testGetContinuousNoiseLevel() const noexcept { return continuousNoiseLevel; }
    float testGetSubFrequencyHz() const noexcept
    {
        return subOscillator.testGetCurrentFrequencyHz();
    }
    float testGetSubPhase01() const noexcept { return subOscillator.testGetPhase01(); }
    float testGetReleaseVelocity() const noexcept { return releaseVelocity; }
    modulation::SourceValues testGetSourceValues() const noexcept { return makeModulationSources(0,0,0,0,true); }
    const modulation::SourceValues& testGetLastRenderedSourceValues() const noexcept { return lastRenderedSourceValuesForTest; }
    void testSetStaticUnisonSpanEnabled(bool enabled) noexcept
    { staticUnisonSpanEnabledForTest = enabled; }
    void testSetUnmodulatedPitchFastPathEnabled(bool enabled) noexcept
    { unmodulatedPitchFastPathEnabledForTest = enabled; }
    bool testGetUnmodulatedPitchFastPathUsed() const noexcept
    { return lastUnmodulatedPitchFastPathUsedForTest; }
    float testGetPolyAftertouchValue() const noexcept { return polyAftertouchValue; }
    float testGetChannelPressureValue() const noexcept { return channelPressureValue; }
    float testGetCombinedAftertouchValue() const noexcept
    {
        return currentAftertouchValue();
    }
#endif

    /// Returns true while the voice is producing audio (including release + steal fade).
    bool isVoiceActive() const
    {
        return noteState.active
            || stealFadeRemaining > 0
            || stealTailRemaining > 0;
    }

private:
    // Sole note/pedal authority. pitchNoteNumber and cachedMidiChannel below
    // are DSP/control caches retained through tails, not another active-note map.
    struct NoteState {
        int number = -1, channel = 1;
        uint64_t startOrder = 0;
        midi::Source source = midi::Source::host;
        bool active = false, keyDown = false, sustain = false, sostenuto = false;
    } noteState;
    void clearCurrentNote() noexcept {
        noteState.number = -1; noteState.channel = 1; noteState.startOrder = 0;
        noteState.active = noteState.keyDown = noteState.sustain = noteState.sostenuto = false;
    }
    // Quick fade-out when a voice is stolen (prevents click)
    // Computed from sample rate in prepare() — ~2 ms at any rate
    int stealFadeSamples = 88;
    int stealFadeRemaining = 0;
    int stealTailRemaining = 0;
    std::array<float, 2> lastRenderedVoiceSample{};
    std::array<float, 2> lastRenderedVoiceBodySample{};
    std::array<float, 2> stealTailSample{};
    std::array<float, 2> structuralFadeStartSample{};
    std::array<float, 2> lastRenderedDirectBody{};
    std::array<float, 2> stealDirectTailSample{};
    std::array<float, 2> structuralDirectFadeStart{};
    std::optional<fad::engine::AudioBufferView> directOutBuffer;
    int directOutOrigin = 0;
    OscillatorUiFrame* oscillatorUiCapture = nullptr;
    int oscillatorUiCaptureOffset = 0;
    int structuralFadeSamples = 132;
    int structuralFadeRemaining = 0;
    bool hasRenderedVoiceBodySample = false;
    std::array<int, 2> renderedCrossModes{};
    bool renderedSerumAmplitude = false;
    int filterCoefficientSampleCounter = 0;
    void updateEnvelopeParameters() noexcept;
    parameters::OscillatorSnapshot createRenderedOscillatorSnapshot(
        size_t oscillatorIndex,
        const modulation::DestinationValues& modulationValues) const noexcept;
    void updateRenderedOscillatorSnapshot(
        size_t oscillatorIndex,
        const modulation::DestinationValues& modulationValues,
        const parameters::OscillatorSnapshot& source,
        parameters::OscillatorSnapshot& target) const noexcept;
    static void updateRenderedOscillatorSnapshot(const parameters::ParameterSnapshot&,
        size_t, const modulation::DestinationValues&, const parameters::OscillatorSnapshot&,
        parameters::OscillatorSnapshot&) noexcept;
    static void resolveNativeTuning(const parameters::ParameterSnapshot&,
        modulation::DestinationValues&) noexcept;
    parameters::FilterSnapshot createRenderedFilterSnapshot(
        float filterEnvelopeValue,
        const modulation::DestinationValues& modulationValues) const noexcept;
    void syncPerformanceStateFromChannel() noexcept;
    float currentAftertouchValue() const noexcept
    {
        return std::max(polyAftertouchValue, channelPressureValue);
    }
    static std::array<float, 2> applyPan(float sample, float pan, bool serumAmplitude = false, bool vitalAmplitude = false) noexcept;
    float renderNoiseSample() noexcept;
    void seedModulationStateFromCurrentSources() noexcept;
    struct ContinuousOscillatorParameters
    {
        float timbre = 0.5f;
        float level = 0.0f;
        float pan = 0.0f;
        float analogShape = 0.5f;
        float analogPulseWidth = 0.5f;
        float wavetablePosition = 0.0f;
        float wavetableWarp = 0.0f;
        float fmIndex = 0.0f;
        float fmFeedback = 0.0f;
        float fmLevel = 0.5f;
    };
public:
    // Borrowed preparation is valid only for this immutable render segment.
    struct RenderContext
    {
        const parameters::ParameterSnapshot* source = nullptr;
        uint64_t routeBits = 0;
        bool preserveSmallAmounts = false;
        std::array<ContinuousOscillatorParameters, 2> oscillatorTargets;
        float filterMix = 1.0f, subPan = 0.0f, subLevel = 0.0f, noiseLevel = 0.0f;
        double untrackedNoiseRate = 1.0;
        std::array<float, 2> noisePanGains{};
        std::array<float, 2> zeroBendTuningRatios{};
    };
    static RenderContext prepareRenderContext(const parameters::ParameterSnapshot&) noexcept;
private:
    static ContinuousOscillatorParameters captureContinuousOscillatorParameters(
        const parameters::OscillatorSnapshot& snapshot) noexcept;
    void seedContinuousParameterStateFromSnapshot() noexcept;
    void updateContinuousParameterTargetsFromSnapshot(const RenderContext* = nullptr) noexcept;
    bool continuousParameterSmoothingActive() const noexcept;
    void advanceContinuousParameterState() noexcept;
    void applyContinuousOscillatorParameters(
        size_t oscillatorIndex,
        parameters::OscillatorSnapshot& snapshot) const noexcept;
    void seedUnisonRandomDetuneForNote() noexcept;
    uint32_t createUnisonRandomState(size_t oscillatorIndex) const noexcept;
    int getRenderedUnisonCount(int requestedVoiceCount) const noexcept;
    struct UnisonVoiceInitialisationRange
    {
        int firstVoiceToInitialise = 1;
        int currentVoiceCount = 1;
    };
    void initialiseNewlyActivatedUnisonVoices(size_t oscillatorIndex,
                                              UnisonVoiceInitialisationRange range) noexcept;
    void beginStructuralFade() noexcept;
    void refreshRenderedUnisonCounts() noexcept;
    void refreshRenderedUnisonVoiceCounts() noexcept;
    float getGlobalLfoSourceValue(size_t lfoIndex, int sampleOffset) const noexcept;
    modulation::SourceValues makeModulationSources(float amp, float filter, float mod, int offset,
                                                   bool includeNative = true) const noexcept;
    modulation::DestinationValues renderCurrentModulationSeed() const noexcept;
    void resolveNativeTuning(modulation::DestinationValues& values) const noexcept;

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    modulation::SourceValues lastRenderedSourceValuesForTest;
    bool staticUnisonSpanEnabledForTest = true;
    bool unmodulatedPitchFastPathEnabledForTest = true;
    bool lastUnmodulatedPitchFastPathUsedForTest = false;
#endif
    const std::array<const wavetable::WavetableData*, parameters::lfoCount>* blockLfoCurveBanks = nullptr;
    std::array<modulation::Lfo, parameters::lfoCount> noteLfos;
    std::array<modulation::BandLimitedNodeCurve,2> nodeWaves;
    mutable std::array<modulation::CurveNodesRuntime,parameters::lfoCount> nodeLfos;
    parameters::CurveNodes nodeFilterCurve;
    int nodeControlCounter=0;
    bool nodeRuntimesInitialised=false;
    mutable std::array<modulation::NativeLfoCurveRuntime, parameters::lfoCount> nativeCurveRuntimes;
    uint32_t nativeRateRouteMask=0, ownedRateLfoMask=0;
    double transportTempoBpm=120.0;
    const parameters::ParameterSnapshot* parameterSnapshot = nullptr;
    std::array<oscillators::OscillatorBlock, 2> oscillators;
    // Current oscillator-level unison render support for the public osc*.unison
    // parameters. This is not the future v0.7 voice-stacking engine; realtime
    // and offline renders use the same public voice count contract.
    static constexpr int maxExtraUnison = parameters::maxRenderedUnisonVoiceCount - 1;
    struct UnisonRenderPlan
    {
        std::array<float, parameters::maxRenderedUnisonVoiceCount> frequencyRatios{1.0f};
        std::array<float, parameters::maxRenderedUnisonVoiceCount> mipLevelOffsets{};
        std::array<float, parameters::maxRenderedUnisonVoiceCount> panOffsets{};
        std::array<float, parameters::maxRenderedUnisonVoiceCount> gainScales{1.0f};
        std::array<float, parameters::maxRenderedUnisonVoiceCount> nativePanLeft{};
        std::array<float, parameters::maxRenderedUnisonVoiceCount> nativePanRight{};
    };
    struct UnisonRenderCache
    {
        UnisonRenderPlan plan;
        std::array<float, parameters::maxRenderedUnisonVoiceCount> randomDetuneCents{};
        std::array<float, parameters::maxRenderedUnisonVoiceCount> staticPanGainLeft{};
        std::array<float, parameters::maxRenderedUnisonVoiceCount> staticPanGainRight{};
        float detuneRange = 0.0f;
        float width = 0.0f;
        float blend = 0.0f;
        float staticLevel = 0.0f;
        float staticPan = 0.0f;
        float staticMixScale = 0.0f;
        int voiceCount = 0;
        int staticPanVoiceCount = 0;
        bool planValid = false;
        bool staticPanValid = false;
        bool serumAmplitude = false;
        bool staticSerumAmplitude = false;
        bool staticVitalAmplitude = false;
    };
    std::array<std::array<oscillators::OscillatorBlock, maxExtraUnison>, 2> unisonOscillators;
    std::array<const wavetable::WavetableData*, 2> appliedWavetablePointers{};
    uint32_t appliedWavetableGeneration = 0;
    bool wavetablePointersApplied = false;
    bool appliedNativeSyncWarp = false;
    std::array<std::array<float, parameters::maxRenderedUnisonVoiceCount>, 2> unisonRandomDetuneCents{};
    std::array<UnisonRenderCache, 2> unisonRenderCaches;
    std::array<int, 2> renderedUnisonVoiceCounts{1, 1};
    int realtimeUnisonVoiceLimit = parameters::maxRenderedUnisonVoiceCount;
    oscillators::OscillatorBlock subOscillator;
    std::array<filters::FilterBlock, 2> filters;
    std::array<fad::engine::math::LinearSmoother<float>, 4> filterInputGains;
    fad::engine::math::LinearSmoother<float> filterWetGain;
    fad::engine::math::LinearSmoother<float> filterMixBaseGain;
    fad::engine::math::LinearSmoother<float> filterEnabledGain;
    modulation::HoldEnvelope ampEnvelope;
    modulation::HoldEnvelope filterEnvelope;
    modulation::HoldEnvelope modEnvelope;
    fad::engine::EnvelopeParameters ampEnvelopeParameters;
    fad::engine::EnvelopeParameters filterEnvelopeParameters;
    fad::engine::EnvelopeParameters modEnvelopeParameters;
    const std::array<modulation::Lfo, parameters::lfoCount>* globalLfos = nullptr;
    const std::array<const float*, parameters::lfoCount>* globalLfoBlockValues = nullptr;
    const std::array<PerformanceState, 16>* performanceStates = nullptr;
    double sampleRate = 44100.0;
    float baseFrequencyHz = 440.0f;
    float pendingNativeGlideFrequency = 0.0f;
    int nativeReleaseResumeGraceRemaining = 0;
    float noteVelocity = 0.0f;
    float rawNoteVelocity = 0.0f;
    float activeVelocityCurve = 0.5f;
    fad::engine::math::LinearSmoother<float> baseFrequencySmoother;
    NativeGlide nativePitchRamp;
    bool usesNativePitchRamp() const noexcept
    { return parameterSnapshot != nullptr && parameterSnapshot->global.serumAmplitude && parameterSnapshot->global.nativeGlide; }
    fad::engine::math::LinearSmoother<float> velocitySmoother;
    fad::engine::math::LinearSmoother<float> velocitySensitivitySmoother;
    fad::engine::math::LinearSmoother<float> channelVolumeSmoother;
    fad::engine::math::LinearSmoother<float> expressionSmoother;
    fad::engine::math::LinearSmoother<float> mpeManagerVolumeSmoother;
    fad::engine::math::LinearSmoother<float> mpeManagerExpressionSmoother;
    fad::engine::math::LinearSmoother<float> mpeManagerPitchSmoother;
    bool mpeMember = false;
    int mpeManagerChannel = 1;
    float modWheelValue = 0.0f;
    float slideValue = 0.0f;
    float releaseVelocity = 0.0f;
    float polyAftertouchValue = 0.0f;
    float channelPressureValue = 0.0f;
    float pitchBendSemitones = 0.0f;
    float nativePitchBend = .5f;
    float pitchBendRange = 2.0f;  // updated from snapshot each render block
    int lastPitchWheelValue = 8192;
    float pitchWheelBipolar=0;
    float midi2NoteBend=0, midi2TuningOffset=0, midi2NoteBendRange=48, midi2PitchValue=.5f, midi2Slide=.5f;
    std::array<float,32> learnedMidiControllers{};
    bool midi2Detached=false, midi2PitchActive=false, midi2SlideActive=false, midi2ChannelControls=false;

    float glideTimeSeconds = 0.0f;
    parameters::GlideMode glideMode = parameters::GlideMode::off;
    int pitchNoteNumber = 60;
    int cachedMidiChannel = 1;
    const wavetable::WavetableData* blockNoiseSample = nullptr;
    const wavetable::RuntimeWavetableBank::PublishedOwnership* blockNoiseOwnership = nullptr;
    NoiseSamplePlayer noiseSamplePlayer;
    float nativeNoiseModulationValue = 0.0f;
    int nativeNoiseModulationPhase = 0;
    uint32_t noiseSeed = 0x6d2b79f5u;
    oscillators::SerumPhaseRandom nativeFallbackPhaseRandom;
    oscillators::SerumPhaseRandomValues nativePhaseRandomValues{};
    bool hasPendingNativePhaseRandomValues = false;
    std::array<float,4> nativeNoteOnValues{};
    std::array<std::array<float, parameters::maxRenderedUnisonVoiceCount>, 2> nativeRememberedPhases{};
    bool hasRememberedNativePhases = false;
    uint32_t noiseState = noiseSeed;
    // Per-voice note sequence, independent of noise rendering. Soft resets keep
    // it; hard reset/reseed replays it. Cache the seed for live Unison expansion.
    uint32_t unisonRandomState = noiseSeed;
    uint32_t unisonNoteRandomState = noiseSeed;

    // Per-oscillator analog drift state (slow random frequency deviation)
    struct DriftState
    {
        float phase = 0.0f;      // slow LFO phase (0..1)
        float rate = 0.0f;       // Hz, randomized per voice
        float currentValue = 0.0f;
    };
    std::array<DriftState, 2> driftStates;

    // One-pole smoother for modulation destinations (avoids zipper noise)
    modulation::DestinationValues smoothedModValues{};
    // Derived predicates remain valid until one of the modulation-value writes.
    uint8_t modulationResidualMask = 0;
    bool modulationResidualMaskValid = false;
    float modSmoothCoeff = 0.004524887f;
    std::array<ContinuousOscillatorParameters, 2> continuousOscillatorState{};
    std::array<ContinuousOscillatorParameters, 2> continuousOscillatorTargets{};
    float continuousSubLevel = 0.0f;
    float continuousSubLevelTarget = 0.0f;
    float continuousSubEnabled = 1.0f, continuousSubEnabledTarget = 1.0f;
    float continuousSubPan = 0.0f, continuousSubPanTarget = 0.0f;
    float continuousSubDirect = 0.0f, continuousSubDirectTarget = 0.0f;
    const wavetable::WavetableData* nativeSubTable = nullptr;
    float continuousNoiseLevel = 0.0f;
    float continuousNoiseLevelTarget = 0.0f;
    float continuousNoiseEnabled = 1.0f, continuousNoiseEnabledTarget = 1.0f;
    float continuousNoiseDirect = 0.0f, continuousNoiseDirectTarget = 0.0f;
    float continuousOscillatorMixScale = 1.0f;
    float continuousOscillatorMixScaleTarget = 1.0f;

    std::atomic<bool> diagnosticsEnabled{false};  // T4 fix: atomic to prevent data race
    diagnostics::ModulationTraceSnapshot lastModulationTrace{};
    const std::array<const wavetable::WavetableData*, 2>* blockWavetablePointers = nullptr;
    uint32_t blockWavetableGeneration = 0;
};
}
