#include <fad/engine/synth/RenderEngine.h>
#include <fad/engine/synth/OfflineSeed.h>
#include <fad/engine/synth/PerformanceCurve.h>
#include <fad/engine/modulation/NativeLfoRate.h>
#include <fad/engine/modulation/NativeLfoBus.h>
#include <fad/engine/parameters/NativeModulationTargets.h>
#include <fad/engine/fx/WideFxControls.h>
#include <fad/engine/math/RuntimeRange.h>
#include <fad/engine/detail/Scope.h>
#include <fad/engine/rt/ScopedNoDenormals.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <numbers>

namespace fadmodel1::dsp::synth
{
namespace
{
static_assert(std::bit_cast<uint32_t>(
                  diagnostics::SessionMetrics::emergencyLimitThreshold)
              == std::bit_cast<uint32_t>(
                  fad::engine::AudioSafetyStage::emergencyKneeStart));

constexpr std::array<float, 12> tempoSyncCycleLengthsInBeats{
    4.0f,
    3.0f,
    2.0f,
    1.5f,
    1.0f,
    0.75f,
    0.5f,
    0.375f,
    0.25f,
    0.1875f,
    0.125f,
    0.0625f};

// Pin the resource cohort before accepting its Noise parameters. A later bulk
// transaction may gate block resolution or publish a newer cohort in between.
bool acquireNoiseSnapshotOwnership(
    const wavetable::RuntimeWavetableBank& bank,
    const parameters::ParameterSnapshot& snapshot,
    const wavetable::RuntimeWavetableBank::PublishedOwnership*& pinned) noexcept
{
    pinned = nullptr;
    if (snapshot.noise.sampleSlot <= 0 || snapshot.wavetableAssetGeneration == 0)
        return true;

    auto resolutionGuard = bank.scopedPublishedOwnershipResolution();
    const auto* ownership = bank.getPublishedOwnershipPointer();
    if (ownership == nullptr || ownership->generation != snapshot.wavetableAssetGeneration)
        return false;

    ownership->audioReaderCount.fetch_add(1, std::memory_order_acq_rel);
    pinned = ownership;
    return true;
}

void adoptNoiseSnapshotOwnership(
    const wavetable::RuntimeWavetableBank::PublishedOwnership*& active,
    const wavetable::RuntimeWavetableBank::PublishedOwnership*& pinned) noexcept
{
    if (pinned == nullptr)
        return;

    if (active == pinned)
        pinned->audioReaderCount.fetch_sub(1, std::memory_order_acq_rel);
    else if (const auto* previous = std::exchange(active, pinned))
        previous->audioReaderCount.fetch_sub(1, std::memory_order_acq_rel);
    pinned = nullptr;
}

bool nearlyEqual(float lhs, float rhs) noexcept
{
    return std::abs(lhs - rhs) <= 1.0e-6f;
}

template <typename VoiceContainer>
int countActiveVoices(const VoiceContainer& voices) noexcept
{
    return static_cast<int>(std::count_if(
        voices.begin(),
        voices.end(),
        [](const VoiceCore* voice)
        {
            return voice != nullptr && voice->isVoiceActive();
        }));
}

std::optional<midi::ShortMessage> decodeSupportedHostMidiMessage(const midi::RenderMidiEvent& event) noexcept
{
    return midi::ShortMessage::decode(event.message.bytes(), event.message.getTimeStamp());
}

constexpr std::array<fx::FxType, 4> canonicalLegacyFxTypes{
    fx::FxType::drive,
    fx::FxType::chorus,
    fx::FxType::delay,
    fx::FxType::reverb};

constexpr uint64_t pendingSnapshotTokenSlotBits = 8;
constexpr uint64_t pendingSnapshotTokenSlotMask = (uint64_t{1} << pendingSnapshotTokenSlotBits) - 1;

uint64_t makePendingSnapshotToken(uint64_t sequence, size_t slot) noexcept
{
    assert(slot < pendingSnapshotTokenSlotMask);
    return (sequence << pendingSnapshotTokenSlotBits) | static_cast<uint64_t>(slot + 1);
}

uint64_t makePendingSnapshotBarrierToken(uint64_t sequence) noexcept
{
    return sequence << pendingSnapshotTokenSlotBits;
}

uint64_t pendingSnapshotTokenSequence(uint64_t token) noexcept
{
    return token >> pendingSnapshotTokenSlotBits;
}

int pendingSnapshotTokenSlot(uint64_t token) noexcept
{
    const auto encodedSlot = token & pendingSnapshotTokenSlotMask;
    return encodedSlot == 0 ? -1 : static_cast<int>(encodedSlot - 1);
}

std::array<float, parameters::ids::maxFxParamsPerSlot> buildLegacySlotParams(const parameters::FxSnapshot& snapshot,
                                           fx::FxType type) noexcept
{
    std::array<float, parameters::ids::maxFxParamsPerSlot> params{};

    switch (type)
    {
        case fx::FxType::drive:
            params[0] = fx::FxCatalog::normalizeParamValue(type, 0, snapshot.drive.amount);
            params[1] = fx::FxCatalog::normalizeParamValue(type, 1, snapshot.drive.tone);
            params[2] = fx::FxCatalog::normalizeParamValue(type, 2, snapshot.drive.mix);
            params[3] = fx::FxCatalog::normalizeParamValue(
                type,
                3,
                static_cast<float>(static_cast<int>(snapshot.drive.mode)));
            break;

        case fx::FxType::chorus:
            params[0] = fx::FxCatalog::normalizeParamValue(type, 0, snapshot.chorus.rateHz);
            params[1] = fx::FxCatalog::normalizeParamValue(type, 1, snapshot.chorus.depth);
            params[2] = fx::FxCatalog::normalizeParamValue(type, 2, snapshot.chorus.delayMs);
            params[3] = fx::FxCatalog::normalizeParamValue(type, 3, snapshot.chorus.feedback);
            params[4] = fx::FxCatalog::normalizeParamValue(type, 4, snapshot.chorus.mix);
            params[5] = fx::FxCatalog::normalizeParamValue(type, 5, snapshot.chorus.highCutHz);
            params[6] = fx::FxCatalog::normalizeParamValue(type, 6, snapshot.chorus.lowCutHz);
            break;

        case fx::FxType::delay:
            params[0] = fx::FxCatalog::normalizeParamValue(
                type,
                0,
                static_cast<float>(snapshot.delay.timeIndex));
            params[1] = fx::FxCatalog::normalizeParamValue(type, 1, snapshot.delay.feedback);
            params[2] = fx::FxCatalog::normalizeParamValue(type, 2, snapshot.delay.highCutHz);
            params[3] = fx::FxCatalog::normalizeParamValue(type, 3, snapshot.delay.lowCutHz);
            params[4] = fx::FxCatalog::normalizeParamValue(type, 4, snapshot.delay.mix);
            break;

        case fx::FxType::reverb:
            params[0] = fx::FxCatalog::normalizeParamValue(type, 0, snapshot.reverb.size);
            params[1] = fx::FxCatalog::normalizeParamValue(type, 1, snapshot.reverb.damping);
            params[2] = fx::FxCatalog::normalizeParamValue(type, 2, snapshot.reverb.wet);
            params[3] = fx::FxCatalog::normalizeParamValue(type, 3, snapshot.reverb.width);
            params[4] = fx::FxCatalog::normalizeParamValue(type, 4, snapshot.reverb.highCutHz);
            params[5] = fx::FxCatalog::normalizeParamValue(type, 5, snapshot.reverb.lowCutHz);
            params[6] = fx::FxCatalog::normalizeParamValue(type, 6, snapshot.reverb.preDelayMs);
            break;

        case fx::FxType::none:
        case fx::FxType::eq3Band:
        case fx::FxType::compressor:
        case fx::FxType::phaser:
        case fx::FxType::flanger:
        case fx::FxType::distortion:
        case fx::FxType::bitcrusher:
        case fx::FxType::filterFx:
        case fx::FxType::stereo:
        case fx::FxType::limiter:
        default:
            break;
    }

    return params;
}

parameters::DynamicFxSnapshot buildCanonicalLegacyFxSnapshot(const parameters::FxSnapshot& legacyFx) noexcept
{
    parameters::DynamicFxSnapshot snapshot{};
    snapshot.slotCount = static_cast<int>(canonicalLegacyFxTypes.size());

    for (size_t slotIndex = 0; slotIndex < canonicalLegacyFxTypes.size(); ++slotIndex)
    {
        auto& slot = snapshot.slots[slotIndex];
        const auto type = canonicalLegacyFxTypes[slotIndex];
        slot.type = static_cast<int>(type);
        slot.order = static_cast<int>(slotIndex);
        slot.params = buildLegacySlotParams(legacyFx, type);

        switch (type)
        {
            case fx::FxType::drive:  slot.enabled = legacyFx.drive.enabled; break;
            case fx::FxType::chorus: slot.enabled = legacyFx.chorus.enabled; break;
            case fx::FxType::delay:  slot.enabled = legacyFx.delay.enabled; break;
            case fx::FxType::reverb: slot.enabled = legacyFx.reverb.enabled; break;
            case fx::FxType::none:
            case fx::FxType::eq3Band:
            case fx::FxType::compressor:
            case fx::FxType::phaser:
            case fx::FxType::flanger:
            case fx::FxType::distortion:
            case fx::FxType::bitcrusher:
            case fx::FxType::filterFx:
            case fx::FxType::stereo:
            case fx::FxType::limiter:
                slot.enabled = false;
                break;
        }
    }

    return snapshot;
}

parameters::DynamicFxSnapshot resolvedDynamicFxSnapshot(
    const parameters::ParameterSnapshot& snapshot) noexcept
{
    const auto& dynamicFx = snapshot.dynamicFx;
    const auto slotCount = std::clamp(dynamicFx.slotCount, 0, parameters::ids::maxFxSlots);
    bool dynamicFxPopulated = dynamicFx.hasExplicitTopology;
    for (int slot = 0; slot < slotCount && ! dynamicFxPopulated; ++slot)
    {
        dynamicFxPopulated =
            fx::FxCatalog::sanitizeTypeValue(
                dynamicFx.slots[static_cast<size_t>(slot)].type)
            != fx::FxType::none;
    }

    return dynamicFxPopulated
        ? dynamicFx
        : buildCanonicalLegacyFxSnapshot(snapshot.fx);
}

bool dynamicFxTopologyDiffers(const parameters::DynamicFxSnapshot& lhs,
                              const parameters::DynamicFxSnapshot& rhs) noexcept
{
    const auto lhsCount = std::clamp(lhs.slotCount, 0, parameters::ids::maxFxSlots);
    const auto rhsCount = std::clamp(rhs.slotCount, 0, parameters::ids::maxFxSlots);
    if (lhsCount != rhsCount)
        return true;

    if (parameters::sanitizeDynamicFxSlotOrder(lhs, lhsCount)
        != parameters::sanitizeDynamicFxSlotOrder(rhs, rhsCount))
    {
        return true;
    }

    for (int slotIndex = 0; slotIndex < lhsCount; ++slotIndex)
    {
        const auto& lhsSlot = lhs.slots[static_cast<size_t>(slotIndex)];
        const auto& rhsSlot = rhs.slots[static_cast<size_t>(slotIndex)];
        if (fx::FxCatalog::sanitizeTypeValue(lhsSlot.type)
                != fx::FxCatalog::sanitizeTypeValue(rhsSlot.type)
            || lhsSlot.enabled != rhsSlot.enabled)
        {
            return true;
        }
        if (parameters::resolvedFxRoutingStage(slotIndex, lhsSlot.routingStage)
                != parameters::resolvedFxRoutingStage(slotIndex, rhsSlot.routingStage)
            || lhsSlot.routingLane != rhsSlot.routingLane)
        {
            return true;
        }
    }

    return false;
}

bool dynamicFxSnapshotsEquivalent(const parameters::DynamicFxSnapshot& lhs,
                                  const parameters::DynamicFxSnapshot& rhs) noexcept
{
    const auto lhsCount = std::clamp(lhs.slotCount, 0, parameters::ids::maxFxSlots);
    const auto rhsCount = std::clamp(rhs.slotCount, 0, parameters::ids::maxFxSlots);
    if (lhs.hasExplicitTopology != rhs.hasExplicitTopology
        || dynamicFxTopologyDiffers(lhs, rhs))
    {
        return false;
    }

    for (int slotIndex = 0; slotIndex < lhsCount; ++slotIndex)
    {
        const auto& a=lhs.slots[static_cast<size_t>(slotIndex)];
        const auto& b=rhs.slots[static_cast<size_t>(slotIndex)];
        if(a.wideRateHz!=b.wideRateHz || a.wideLeftMs!=b.wideLeftMs || a.wideRightMs!=b.wideRightMs) return false;
        const auto& lhsParams = lhs.slots[static_cast<size_t>(slotIndex)].params;
        const auto& rhsParams = rhs.slots[static_cast<size_t>(slotIndex)].params;
        for (size_t paramIndex = 0; paramIndex < lhsParams.size(); ++paramIndex)
        {
            const auto lhsValue = fadmodel1::dsp::utils::sanitizeUnitInterval(lhsParams[paramIndex]);
            const auto rhsValue = fadmodel1::dsp::utils::sanitizeUnitInterval(rhsParams[paramIndex]);
            if (! nearlyEqual(lhsValue, rhsValue))
                return false;
        }
    }

    return lhsCount == rhsCount;
}

fad::engine::math::RuntimeRange lfoRateRange() noexcept
{
    return {0.05f, 40.0f, 0.0f, 0.3f};
}
}



RenderEngine::RenderEngine(wavetable::RuntimeWavetableBank& bank, fx::FxBackend& fx, fad::engine::RenderServiceHooks hooks)
    : fxChain(fx), serviceHooks(hooks), wavetableBank(bank)
{

    for (int voiceIndex = 0; voiceIndex < parameters::defaultVoiceCount; ++voiceIndex)
    {
        auto voice = std::make_unique<VoiceCore>();
        voice->setNoiseSeed(offlineVoiceSeed(0,static_cast<uint32_t>(voiceIndex)));
        voiceCache.push_back(voice.get());
        ownedVoices.push_back(std::move(voice));
    }

    midiState().lastPitchWheelValues.fill(8192);
    midiState().pitchBendRangeOverrides.fill(-1.0f);
    midiState().rpnMsb.fill(127);
    midiState().rpnLsb.fill(127);
}

RenderEngine::~RenderEngine() { shutdownAssetRetirement(); }

void RenderEngine::shutdownAssetRetirement() noexcept
{
    releaseBlockWavetableOwnership(true);
    deferredReleaseThreadShouldExit.store(true, std::memory_order_release);
    deferredReleaseWorkAvailable.signal();
    deferredReleaseWorkerResumeEvent.signal();
    if (deferredReleaseThread.joinable()) deferredReleaseThread.join();
    collectGarbage();
}

void RenderEngine::ensureDeferredReleaseWorkerStarted() noexcept
{
    if (deferredReleaseThread.joinable())
        return;

    try
    {
        deferredReleaseThread = std::thread([this]
        {
            bool pollUntilReady = false;
            while (! deferredReleaseThreadShouldExit.load(std::memory_order_acquire))
            {
                deferredReleaseWorkAvailable.wait(pollUntilReady ? 1 : -1);

                if (deferredReleaseWorkerBlocked.load(std::memory_order_acquire))
                {
                    deferredReleaseWorkerBlockedEvent.signal();
                    while (deferredReleaseWorkerBlocked.load(std::memory_order_acquire)
                           && ! deferredReleaseThreadShouldExit.load(std::memory_order_acquire))
                    {
                        deferredReleaseWorkerResumeEvent.wait(-1);
                    }
                }

                pollUntilReady = collectGarbage();
            }

            while (collectGarbage())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    }
    catch (...)
    {
        // Control-thread collection remains available if the host refuses a worker thread.
    }
}



void RenderEngine::prepare(const fad::engine::ProcessSpec& spec,uint32_t offlineRenderSeed)
{
    noteOnModulationSeed = offlineRenderSeed;
    const auto safeSampleRate = std::isfinite(spec.sampleRate) && spec.sampleRate > 0.0
        ? std::clamp(spec.sampleRate, 1.0, 384000.0)
        : 44100.0;
    const fad::engine::ProcessSpec safeSpec{safeSampleRate, spec.maximumBlockSize, spec.numChannels};

    lastSampleRate = safeSampleRate;
    lastBlockSize = static_cast<int>(spec.maximumBlockSize);
    for (size_t lfoIndex = 0; lfoIndex < globalLfoUsed.size(); ++lfoIndex)
        globalLfoUsed[lfoIndex] = modulationUsesLfo(lfoIndex);
    signalFlowDirty = true;

    // Configure DynamicFxChain with default 4 slots (Drive/Chorus/Delay/Reverb)
    {
        fx::FxBackend::ScopedBatchUpdate batchUpdate(fxChain);
        fxChain.setSlotCount(4);
        fxChain.setSlotType(0, fx::FxType::drive);
        fxChain.setSlotType(1, fx::FxType::chorus);
        fxChain.setSlotType(2, fx::FxType::delay);
        fxChain.setSlotType(3, fx::FxType::reverb);
        for (int slotIndex = 0; slotIndex < 4; ++slotIndex)
            fxChain.setSlotEnabled(slotIndex, false);
        fxChain.setSlotOrder(std::array<int, parameters::ids::maxFxSlots>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 4);
    }
    fxChain.prepare(safeSpec);
    wasPlaying = false;
    hasLastTransportPpqPosition = false;
    lastTransportPpqAdvance = 0.0;
    directAuxScratch.setSize(2, std::max(1, static_cast<int>(spec.maximumBlockSize)), false, false, true);
    bypassAdvanceScratchBuffer.setSize(std::max(1, static_cast<int>(std::min(2u, spec.numChannels))),
                                       static_cast<int>(spec.maximumBlockSize),
                                       false,
                                       false,
                                       true);
    hasAppliedDynamicFxSnapshot = false;
    resetDeferredMidiCount = 0;
    resetDeferredNoteCount = 0;
    resetDeferredMidiReplayBase = 0;
    resetDeferredMidiOverflowed = false;

    // Preallocate diagnostics meters for the maximum FX slot count so the
    // audio thread never needs to resize this vector.
    fxCpuMeters.resize(static_cast<size_t>(parameters::ids::maxFxSlots));

    outputGainNeedsInitialSnapshot = true;
    outputGainSmoother.reset(safeSampleRate, 0.02);
    outputGainSmoother.setCurrentAndTargetValue(parameterSnapshot.global.outputGainLinear * parameterSnapshot.global.presetGain);
    allSoundOffOutputGain.reset(std::max(8, static_cast<int>(safeSampleRate * 0.002)));
    allSoundOffOutputGain.setCurrentAndTargetValue(1.0f);

    for (auto& lfo : globalLfos)
        lfo.prepare(safeSampleRate);
    for (auto& lfo : fxNoteLfos)
        lfo.prepare(safeSampleRate);
    for (auto& envelope : fxModEnvelopes)
        envelope.setSampleRate(safeSampleRate);
    fxModSourceFrames.resize(static_cast<size_t>(std::max(1u, safeSpec.maximumBlockSize)));
    for (auto& lfoBlock : globalLfoBlockStorage)
        lfoBlock.assign(static_cast<size_t>(std::max(1u, safeSpec.maximumBlockSize)),
                        0.0f);
    globalLfoBlockPointers.fill(nullptr);

    for(size_t i=0;i<voiceCache.size();++i)
    {
        voiceCache[i]->setNoiseSeed(offlineVoiceSeed(offlineRenderSeed,static_cast<uint32_t>(i)));
        voiceCache[i]->prepare(safeSpec);
    }

    if (serviceHooks.prepare) serviceHooks.prepare(serviceHooks.owner, safeSpec);
    audioSafetyStage.prepare(safeSampleRate);
    outputPeakLimiter.prepare(safeSampleRate);
    audioSafetyStage.setNativeCompatibility(parameterSnapshot.global.serumAmplitude);
    resetFadeRemainingSamples.store(0, std::memory_order_release);
    resetFadeTotalSamples = std::max(1, static_cast<int>(safeSampleRate * 0.03));
    resetFadeTotalAtStartSamples = resetFadeTotalSamples;
    for (auto& pendingState : pendingParameterSnapshotStates)
        pendingState.store(PendingParameterSnapshotState::empty, std::memory_order_release);
    for (auto& pendingSequence : pendingParameterSnapshotSequences)
        pendingSequence.store(0, std::memory_order_release);
    for (auto& mayApplyControlThreadFxRebuild : pendingParameterSnapshotMayApplyControlThreadFxRebuild)
        mayApplyControlThreadFxRebuild.store(false, std::memory_order_release);
    for (auto& preparedFxSequence : pendingParameterSnapshotPreparedFxSequences)
        preparedFxSequence.store(0, std::memory_order_release);
    pendingParameterSnapshotNextSequence.store(1, std::memory_order_release);
    latestPendingParameterSnapshotToken.store(0, std::memory_order_release);
    resetInternal(false);
    if (stagedIsolatedInitialSnapshotPending)
    {
        stagedIsolatedInitialSnapshotPending = false;
        // prepare/reset establish the default graph first. Reuse the normal
        // control-side publication to prepare the explicitly staged graph.
        applyParameterSnapshotNowOnControlThread(parameterSnapshot);
    }
}

void RenderEngine::reset()
{
    resetInternal(true);
}

void RenderEngine::resetInternal(bool drainDeferredRelease) noexcept
{
    inputMidiOverflowPanicPending = false;
    invalidateOscillatorUiCapture();
    for (auto& curve : nativeCurveRuntimes) curve.invalidate();
    fxNodeLfoMask = 0;
    fxNodeCurveCounter = 0;
    releaseBlockWavetableOwnership(true);
    if (drainDeferredRelease)
        collectGarbage();
    signalFlowDirty = true;
    blockWavetablePointers = {nullptr, nullptr};
    if (serviceHooks.reset) serviceHooks.reset(serviceHooks.owner);
    resetDeferredMidiCount = 0;
    resetDeferredNoteCount = 0;
    resetDeferredMidiBlockBase = 0;
    resetDeferredMidiReplayBase = 0;
    resetDeferredMidiOverflowed = false;
    resetFadeRemainingSamples.store(0, std::memory_order_release);
    resetFadeTotalAtStartSamples = resetFadeTotalSamples;
    renderedAudibleState.store(false, std::memory_order_relaxed);
    for (auto& pendingState : pendingParameterSnapshotStates)
        pendingState.store(PendingParameterSnapshotState::empty, std::memory_order_release);
    for (auto& pendingSequence : pendingParameterSnapshotSequences)
        pendingSequence.store(0, std::memory_order_release);
    for (auto& mayApplyControlThreadFxRebuild : pendingParameterSnapshotMayApplyControlThreadFxRebuild)
        mayApplyControlThreadFxRebuild.store(false, std::memory_order_release);
    for (auto& preparedFxSequence : pendingParameterSnapshotPreparedFxSequences)
        preparedFxSequence.store(0, std::memory_order_release);
    pendingParameterSnapshotNextSequence.store(1, std::memory_order_release);
    latestPendingParameterSnapshotToken.store(0, std::memory_order_release);

    // Trigger a fast release on all active voices instead of hard-cutting them.
    // This produces a ~50ms fade-out that prevents clicks on preset switches.
    resetVoiceState();
}

void RenderEngine::requestReset()
{
    if (serviceHooks.requestReset) serviceHooks.requestReset(serviceHooks.owner);
    resetRequested.store(true, std::memory_order_release);
}

bool RenderEngine::hasPendingFxRebuildForControlThread() const noexcept
{
    return fxChain.hasPendingRebuildForControlThread();
}

void RenderEngine::tryApplyPendingFxRebuildOnControlThread() noexcept
{
    fxChain.tryApplyPendingRebuildOnControlThread();
}



void RenderEngine::resetVoiceState() noexcept
{
    nativePhaseRandom.reset();
    noteOnModulation.reset(noteOnModulationSeed);
    fxNoteOnValues = {};
    resetMonoState();
    resetHeldNoteState();
    audioSafetyStage.reset();
    outputPeakLimiter.reset();
    for (auto& source : midiSourceStates)
    {
        const auto counts = source.ciProfileCounts;
        const auto ranges = source.ciProfileBendRanges;
        const auto coarse = source.ciProfileBendCoarse;
        const auto active = source.ciProfileActive;
        source = MidiSourceState{};
        source.ciProfileCounts = counts; source.ciProfileBendRanges = ranges;
        source.ciProfileBendCoarse = coarse; source.ciProfileActive = active;
    }
    for(size_t source=0;source<midi::sourceCount;++source)
        if(midi2SourcesUsed[source])
        {
            std::fill_n(midi2NoteStates.begin()+static_cast<ptrdiff_t>(source*16*128),16*128,Midi2NoteState{});
            if(midiSourceResetter)midiSourceResetter(midiObserverOwner,static_cast<midi::Source>(source));
        }
    midi2SourcesUsed.fill(false);
    activeMidiSource = fxModSource = currentMonoSource = midi::Source::host;
    instrumentPitchActive = false;
    allSoundOffSamplesRemaining = 0;
    allSoundOffOutputGain.setCurrentAndTargetValue(1.0f);

    for (auto& lfo : globalLfos)
        lfo.reset();
    for (auto& lfo : fxNoteLfos) lfo.reset();
    for (auto& envelope : fxModEnvelopes) envelope.reset();
    fxNoiseState = 0x6d2b79f5u; fxNoisePhase = 0; fxNoiseValue = 0; fxNoisePlayer.reset();
    fxModNote = -1;
    fxModControlsFrozen = false;
    fxModRawVelocity = 0.0f;
    fxModVelocity = 0.0f;
    fxModReleaseVelocity = 0.0f;
    fxModReleasePending = false;
    fxModFrameStart = -1;
    fxModFrameCount = 0;
    fxChain.setNativeModulationInput(nullptr, nullptr, 0);

    fxChain.reset();

    for (auto* synthVoice : voiceCache)
        synthVoice->hardReset();

    sessionMetrics.recordActiveNoteCount(0);
    renderedAudibleState.store(false, std::memory_order_relaxed);
}

int RenderEngine::mpeManagerChannelFor(int channel) const noexcept
{
    if (!mpeEnabledForCurrentSource()) return 0;
    if (midiState().ciProfileActive)
    {
        for (int manager = 0; manager < channel - 1; ++manager)
            if (midiState().ciProfileCounts[static_cast<size_t>(manager)] > 0
                && channel <= manager + midiState().ciProfileCounts[static_cast<size_t>(manager)]) return manager + 1;
        return 0;
    }
    if (channel >= 2 && channel <= midiState().mpeZones[0].members + 1) return 1;
    if (channel <= 15 && channel >= 16 - midiState().mpeZones[1].members) return 16;
    return 0;
}

bool RenderEngine::isMpeMemberChannel(int channel) const noexcept
{ return mpeManagerChannelFor(channel) != 0; }

bool RenderEngine::isMpeManagerChannel(int channel) const noexcept
{
    if (midiState().ciProfileActive) return channel >= 1 && channel <= 16
        && midiState().ciProfileCounts[static_cast<size_t>(channel - 1)] > 0;
    return mpeEnabledForCurrentSource()
        && ((channel == 1 && midiState().mpeZones[0].members > 0)
            || (channel == 16 && midiState().mpeZones[1].members > 0));
}

void RenderEngine::configureMpeZone(int managerChannel, int members) noexcept
{
    std::array<int, 16> previousManagers{};
    for (int channel = 1; channel <= 16; ++channel)
        previousManagers[static_cast<size_t>(channel - 1)] = mpeManagerChannelFor(channel);
    const auto zoneIndex = managerChannel == 16 ? 1u : 0u;
    auto& zone = midiState().mpeZones[zoneIndex];
    zone = {std::clamp(members, 0, 15)};
    auto& other = midiState().mpeZones[1u - zoneIndex];
    if (zone.members > 0 && zone.members + other.members >= 15)
        other.members = std::max(0, 14 - zone.members);
    const bool changedFxOwner=fxModSource==activeMidiSource && fxModNote>=0
        && previousManagers[static_cast<size_t>(std::clamp(fxModChannel, 1, 16)-1)]!=mpeManagerChannelFor(fxModChannel);
    for (int channel = 1; channel <= 16; ++channel)
    {
        if (previousManagers[static_cast<size_t>(channel - 1)] == mpeManagerChannelFor(channel)) continue;
        resetHeldNoteStateForChannel(channel);
        if (parameterSnapshot.global.voiceMode == parameters::VoiceMode::mono)
            resetMonoStateForChannel(channel);
        stopVoicesForChannel(channel, false);
    }
    if(changedFxOwner)
    {
        if(parameterSnapshot.global.voiceMode==parameters::VoiceMode::mono && monoHeldNoteCount>0)releaseFxModNote();
        else retireFxModNote();
    }
    refreshMpePedals();
    for (auto* voice : voiceCache)
        if (ownsVoice(voice)) voice->setPitchBendRange(effectivePitchBendRange(voice->getRealtimeMidiChannel()));
    refreshMpeManagerControls();
}

float RenderEngine::effectivePitchBendRange(int midiChannel) const noexcept
{
    const auto channel = std::clamp(midiChannel, 1, 16);
    const auto manager = mpeManagerChannelFor(channel);
    if (midiState().ciProfileActive && (manager != 0 || isMpeManagerChannel(channel)))
        return midiState().ciProfileBendRanges[static_cast<size_t>((manager != 0 ? manager : channel) - 1)];
    if (manager != 0 || isMpeManagerChannel(channel))
    {
        const auto& zone = midiState().mpeZones[(manager != 0 ? manager : channel) == 16 ? 1 : 0];
        return manager != 0
            ? (zone.memberBend >= 0.0f ? zone.memberBend : parameterSnapshot.global.mpePitchBendRange)
            : (zone.managerBend >= 0.0f ? zone.managerBend : 2.0f);
    }
    const auto overrideRange = midiState().pitchBendRangeOverrides[static_cast<size_t>(channel - 1)];
    return overrideRange >= 0.0f ? overrideRange : parameterSnapshot.global.pitchBendRange;
}

void RenderEngine::applyMpeManagerControls(VoiceCore* voice, bool snap) noexcept
{
    if (voice == nullptr) return;
    const bool member = isMpeMemberChannel(voice->getRealtimeMidiChannel());
    voice->setMpeMember(member, std::max(1, mpeManagerChannelFor(voice->getRealtimeMidiChannel())));
    if (!member)
    {
        voice->setMpeManagerControls(1.0f, 1.0f, 0.0f, snap);
        return;
    }
    const auto managerChannel = mpeManagerChannelFor(voice->getRealtimeMidiChannel());
    const auto managerIndex = static_cast<size_t>(managerChannel - 1);
    const auto& manager = midiState().performanceStates[managerIndex];
    const auto bend = effectivePitchBendRange(managerChannel)
        * midiState().pitchWheelBipolar[managerIndex];
    voice->setMpeManagerControls(manager.channelVolume, manager.expression, bend, snap);
}

void RenderEngine::refreshMpeManagerControls() noexcept
{
    for (auto* voice : voiceCache)
        if (ownsVoice(voice))
            applyMpeManagerControls(voice, false);
}

void RenderEngine::applySustainController(int midiChannel, bool down) noexcept
{
    const auto channel = std::clamp(midiChannel, 1, 16);
    midiState().localSustainPedalDown[static_cast<size_t>(channel - 1)] = down;
    const auto refresh = [this](int ch) noexcept
    {
        const bool effective = midiState().localSustainPedalDown[static_cast<size_t>(ch - 1)]
            || (isMpeMemberChannel(ch) && midiState().localSustainPedalDown[static_cast<size_t>(mpeManagerChannelFor(ch) - 1)]);
        handleSustainPedal(ch, effective);
    };
    refresh(channel);
    if (isMpeManagerChannel(channel))
        for (int member = 1; member <= 16; ++member)
            if (mpeManagerChannelFor(member) == channel) refresh(member);
}

void RenderEngine::applySostenutoController(int midiChannel, bool down) noexcept
{
    const auto channel = std::clamp(midiChannel, 1, 16);
    midiState().localSostenutoPedalDown[static_cast<size_t>(channel - 1)] = down;
    const auto refresh = [this](int ch) noexcept
    {
        const auto index = static_cast<size_t>(ch - 1);
        const bool effective = midiState().localSostenutoPedalDown[index]
            || (isMpeMemberChannel(ch) && midiState().localSostenutoPedalDown[static_cast<size_t>(mpeManagerChannelFor(ch) - 1)]);
        if (midiState().effectiveSostenutoPedalDown[index] != effective)
        {
            midiState().effectiveSostenutoPedalDown[index] = effective;
            handleSostenutoPedal(ch, effective);
        }
    };
    refresh(channel);
    if (isMpeManagerChannel(channel))
        for (int member = 1; member <= 16; ++member)
            if (mpeManagerChannelFor(member) == channel) refresh(member);
}

void RenderEngine::refreshMpePedals() noexcept
{
    for (int channel = 1; channel <= 16; ++channel)
    {
        const auto index = static_cast<size_t>(channel - 1);
        const bool master = isMpeMemberChannel(channel);
        handleSustainPedal(channel,
            midiState().localSustainPedalDown[index] || (master && midiState().localSustainPedalDown[static_cast<size_t>(mpeManagerChannelFor(channel) - 1)]));
        const bool sostenuto = midiState().localSostenutoPedalDown[index]
            || (master && midiState().localSostenutoPedalDown[static_cast<size_t>(mpeManagerChannelFor(channel) - 1)]);
        if (midiState().effectiveSostenutoPedalDown[index] != sostenuto)
        {
            midiState().effectiveSostenutoPedalDown[index] = sostenuto;
            handleSostenutoPedal(channel, sostenuto);
        }
    }
}

void RenderEngine::clearBypassInactiveAudioTails() noexcept
{
    audioSafetyStage.reset();
    outputPeakLimiter.reset();
    for (auto& envelope : fxModEnvelopes) envelope.reset();
    fxNoiseState = 0x6d2b79f5u; fxNoisePhase = 0; fxNoiseValue = 0; fxNoisePlayer.reset();
    fxModNote = -1;
    fxModControlsFrozen = false;
    fxModReleasePending = false;
    fxChain.reset();

    for (auto* synthVoice : voiceCache)
    {
        synthVoice->resetRealtimeState();
        synthVoice->reset();
    }

    renderedAudibleState.store(false, std::memory_order_relaxed);
}

void RenderEngine::resolveBlockWavetables() noexcept
{
    const auto* restoreGate = stateRestoreGate.load(std::memory_order_acquire);
    const bool stateRestoreInProgress =
        restoreGate != nullptr && restoreGate->load(std::memory_order_acquire);
    if (! stateRestoreInProgress)
    {
        auto ownershipResolutionGuard = wavetableBank.scopedPublishedOwnershipResolution();
        const auto* publishedOwnership = wavetableBank.getPublishedOwnershipPointer();
        const bool snapshotCanSeePublishedOwnership =
            publishedOwnership == nullptr
            || publishedOwnership->generation <= parameterSnapshot.wavetableAssetGeneration;
        if (snapshotCanSeePublishedOwnership && publishedOwnership != blockPublishedOwnership)
        {
            if (publishedOwnership != nullptr)
                publishedOwnership->audioReaderCount.fetch_add(1, std::memory_order_acq_rel);

            auto* previousOwnership = std::exchange(blockPublishedOwnership, publishedOwnership);
            if (previousOwnership != nullptr)
                previousOwnership->audioReaderCount.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    else
    {
        // A bulk restore gates parameter snapshots and wavetable publication as one
        // transaction. Keep ownership already held by this block.
        // do not acquire newly published assets while parameterSnapshot is still old.
    }

    for(size_t owner=0;owner<parameters::curveOwnerCount;++owner)
    {
        auto& curve=parameterSnapshot.curves[owner];curve.history=nullptr;
        const auto slot=curve.historySlot;
        const auto* asset=blockPublishedOwnership && slot>0 && slot<=wavetable::RuntimeWavetableBank::maxUserSlots
            ?blockPublishedOwnership->userTables[size_t(slot-1)].get():nullptr;
        if(asset && parameters::compatibleCurveHistoryOwner(asset->historyOwner,int(owner)) && !asset->historyTimes.empty())
        {curve.history=asset;curve.enabled=true;}
    }
    parameterSnapshot.filter.curve=parameterSnapshot.curves[10];
    if (blockPublishedOwnership == nullptr)
    {
        blockWavetablePointers = {nullptr, nullptr};
        blockWavetableGeneration = 0;
        blockLfoCurveBanks.fill(nullptr);
        blockNoiseSample = nullptr;
        return;
    }

    blockWavetableGeneration = blockPublishedOwnership->signalGeneration;
    const auto noiseSlot = parameterSnapshot.noise.sampleSlot;
    blockNoiseSample = noiseSlot > 0 && noiseSlot <= wavetable::RuntimeWavetableBank::maxUserSlots
        ? blockPublishedOwnership->userTables[static_cast<size_t>(noiseSlot - 1)].get() : nullptr;
    if (blockNoiseSample != nullptr && blockNoiseSample->sampleLength == 0) blockNoiseSample = nullptr;
    for (size_t i = 0; i < blockLfoCurveBanks.size(); ++i) {
        const auto slot = parameterSnapshot.modulation.lfos[i].curveSlot;
        blockLfoCurveBanks[i] = slot > 0 && slot <= wavetable::RuntimeWavetableBank::maxUserSlots
            ? blockPublishedOwnership->userTables[static_cast<size_t>(slot - 1)].get() : nullptr;
    }

    for (size_t oscillatorIndex = 0; oscillatorIndex < blockWavetablePointers.size(); ++oscillatorIndex)
    {
        const int wtSlot = parameterSnapshot.oscillators[oscillatorIndex].wavetableSlot;
        if (wtSlot <= 0 || wtSlot > wavetable::RuntimeWavetableBank::maxUserSlots)
            blockWavetablePointers[oscillatorIndex] = blockPublishedOwnership->builtinTable.get();
        else
            blockWavetablePointers[oscillatorIndex] =
                blockPublishedOwnership->userTables[static_cast<size_t>(wtSlot - 1)]
                    ? blockPublishedOwnership->userTables[static_cast<size_t>(wtSlot - 1)].get()
                    : blockPublishedOwnership->builtinTable.get();

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
        if (resolveBlockWavetableHook)
            resolveBlockWavetableHook(oscillatorIndex);
#endif
    }
}

void RenderEngine::applyParameterSnapshotNow(const parameters::ParameterSnapshot& snapshot,
                                            bool updateReadableActiveFxProcessors) noexcept
{
    const auto targetDynamicFx = resolvedDynamicFxSnapshot(snapshot);

    if (snapshot.global.voiceMode != activeVoiceMode)
    {
        stopVoicesForChannel(0, false);
        resetMonoState();
        resetHeldNoteState();
        retireFxModNote();
        sessionMetrics.recordActiveNoteCount(countActiveVoices(voiceCache));
        activeVoiceMode = snapshot.global.voiceMode;
        // Mode changes retire notes, not physical controller state.
        refreshMpePedals();
    }

    parameterSnapshot = snapshot;
    voiceRenderContext.reset();
    for(size_t slot=0;slot<parameterSnapshot.modulation.slots.size();++slot)
        parameterSnapshot.modulation.slots[slot].midiLearnSlot=static_cast<int>(slot);
    for (size_t lfoIndex = 0; lfoIndex < globalLfoUsed.size(); ++lfoIndex)
        globalLfoUsed[lfoIndex] = modulationUsesLfo(lfoIndex);
    audioSafetyStage.setNativeCompatibility(snapshot.global.serumAmplitude);
    nativeRateRouteMask=modulation::nativeLfoRateRouteMask(snapshot.modulation);
    hasNativeFxRoutes = std::any_of(snapshot.modulation.slots.begin(), snapshot.modulation.slots.end(),
        [](const auto& route) { return parameters::nativeFxModulationTarget(route.nativeTarget) && route.hasSource()
                                      && route.hasEffectiveAmount(); });
    hasNativeFxClock = false;
    hasEqCurveModulation = false;
    for (int index = 0; index < targetDynamicFx.slotCount; ++index)
    {
        const auto& slot = targetDynamicFx.slots[static_cast<size_t>(index)];
        if(slot.enabled && slot.type==static_cast<int>(fx::FxType::eq3Band))
        {
            const auto& curve=snapshot.curves[parameters::eqCurveOwnerBegin+size_t(index)];
            // A history frame owns its local node/LFO routes too. They need
            // sources even when the static parameter nodes have no bindings.
            if(curve.historySlot>0)globalLfoUsed.fill(true);
            hasEqCurveModulation=hasEqCurveModulation || curve.historySlot>0
                || ((curve.enabled || curve.historySlot>0) && std::any_of(curve.mods.begin(),curve.mods.end(),
                    [](const auto& route){return route.source>0 && route.amount!=0.f;}));
        }
        if (slot.enabled && slot.type == static_cast<int>(fx::FxType::filterFx)
            && fad::engine::math::roundToNearestInt(slot.params[6] * 96.0f) - 1 == 79)
            hasNativeFxClock = true;
        if (slot.enabled && slot.type == static_cast<int>(fx::FxType::distortion)
            && ((slot.params[14] >= .25f && (slot.params[11] >= .5f || slot.params[12] >= .5f || slot.params[17] >= .5f))
                || slot.params[13] >= .5f || (slot.params[8] >= .5f && slot.params[9] >= .5f)))
            hasNativeFxClock = true;
        if (slot.enabled && slot.type == static_cast<int>(fx::FxType::phaser) && slot.params[8] >= .5f)
            hasNativeFxClock = true;
    }
    hasNativeSustainRoutes = std::any_of(snapshot.modulation.slots.begin(), snapshot.modulation.slots.end(),
        [](const auto& route) { return route.nativeTarget==38 && route.hasSource() && route.hasEffectiveAmount(); });
    hasNativeDecayRoutes = std::any_of(snapshot.modulation.slots.begin(), snapshot.modulation.slots.end(),
        [](const auto& route) { return route.nativeTarget==37 && route.hasSource() && route.hasEffectiveAmount(); });
    hasNativeReleaseRoutes = std::any_of(snapshot.modulation.slots.begin(), snapshot.modulation.slots.end(),
        [](const auto& route) { return route.nativeTarget==39 && route.hasSource() && route.hasEffectiveAmount(); });
    fxModEnvelopes[0].setSustainModulationEnabled(hasNativeSustainRoutes);
    fxModEnvelopes[0].setDecayModulationEnabled(hasNativeDecayRoutes);
    fxModEnvelopes[0].setReleaseModulationEnabled(hasNativeReleaseRoutes);
    const std::array<const parameters::EnvelopeSnapshot*, 3> fxEnvelopeSettings{
        &snapshot.ampEnvelope, &snapshot.filterEnvelope, &snapshot.modEnvelope};
    for (size_t i = 0; i < fxModEnvelopes.size(); ++i) {
        const auto& settings = *fxEnvelopeSettings[i];
        fxModEnvelopes[i].setHoldSeconds(settings.holdSeconds);
        fxModEnvelopes[i].setCurves(settings.curves);
        fxModEnvelopes[i].setParameters({settings.attackSeconds, settings.decaySeconds,
                                      settings.sustainLevel, settings.releaseSeconds});
    }
    signalFlowDirty = true;

    if (! hasAppliedDynamicFxSnapshot
        || ! dynamicFxSnapshotsEquivalent(targetDynamicFx, appliedDynamicFxSnapshot))
    {
        applyDynamicFxSnapshot(targetDynamicFx,
                               false,
                               updateReadableActiveFxProcessors);
    }

    assert(fxCpuMeters.empty()
            || static_cast<int>(fxCpuMeters.size()) >= parameters::ids::maxFxSlots);

    outputGainSmoother.setTargetValue(parameterSnapshot.global.outputGainLinear * parameterSnapshot.global.presetGain);
}

void RenderEngine::applyParameterSnapshotNowOnControlThread(
    const parameters::ParameterSnapshot& snapshot) noexcept
{
    applyParameterSnapshotNow(snapshot, false);
    fxChain.tryApplyPendingRebuildOnControlThread();
}

uint64_t RenderEngine::prepareDynamicFxSnapshotForDeferredAcceptanceOnControlThread(
    const parameters::DynamicFxSnapshot& snapshot) noexcept
{
    applyDynamicFxSnapshot(snapshot,
                           true,
                           false,
                           false,
                           true);
    fxChain.tryApplyPendingRebuildOnControlThread();
    const auto sequence = fxChain.getLatestDesiredStateSequence();
    return (sequence & 1u) == 0u ? sequence : 0u;
}

bool RenderEngine::tryQueuePendingParameterSnapshot(
    const parameters::ParameterSnapshot& snapshot,
    bool mayApplyControlThreadFxRebuild) noexcept
{
    const auto publishSequence =
        pendingParameterSnapshotNextSequence.fetch_add(1, std::memory_order_acq_rel);

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    if (blockPendingParameterSnapshotSequenceForTest != nullptr
        && blockPendingParameterSnapshotSequenceForTest->load(std::memory_order_acquire))
    {
        if (pendingParameterSnapshotSequenceEnteredForTest != nullptr)
            pendingParameterSnapshotSequenceEnteredForTest->signal();
        if (allowPendingParameterSnapshotSequenceForTest != nullptr)
            allowPendingParameterSnapshotSequenceForTest->wait();
    }
#endif

    constexpr int maxPublishAttempts = static_cast<int>(pendingParameterSnapshotSlotCount) * 2;
    for (int publishAttempt = 0; publishAttempt < maxPublishAttempts; ++publishAttempt)
    {
        const auto latestSlot =
            pendingSnapshotTokenSlot(latestPendingParameterSnapshotToken.load(std::memory_order_acquire));
        for (size_t slotOffset = 0; slotOffset < pendingParameterSnapshotSlotCount; ++slotOffset)
        {
            const auto startSlot = latestSlot >= 0 ? latestSlot + 1 : 0;
            const auto candidateSlot =
                static_cast<size_t>((startSlot + static_cast<int>(slotOffset))
                                    % static_cast<int>(pendingParameterSnapshotSlotCount));
            auto expectedState = pendingParameterSnapshotStates[candidateSlot].load(std::memory_order_acquire);
            if (expectedState == PendingParameterSnapshotState::writing
                || expectedState == PendingParameterSnapshotState::reading)
            {
                continue;
            }

            const auto latestBeforeReserve =
                latestPendingParameterSnapshotToken.load(std::memory_order_acquire);
            if (expectedState == PendingParameterSnapshotState::ready
                && pendingSnapshotTokenSlot(latestBeforeReserve) == static_cast<int>(candidateSlot))
            {
                continue;
            }

            if (! pendingParameterSnapshotStates[candidateSlot].compare_exchange_strong(
                    expectedState,
                    PendingParameterSnapshotState::writing,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                continue;
            }

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
            if (blockPendingParameterSnapshotWriteForTest != nullptr
                && blockPendingParameterSnapshotWriteForTest->load(std::memory_order_acquire))
            {
                if (pendingParameterSnapshotWriteEnteredForTest != nullptr)
                    pendingParameterSnapshotWriteEnteredForTest->signal();
                if (allowPendingParameterSnapshotWriteForTest != nullptr)
                    allowPendingParameterSnapshotWriteForTest->wait();
            }
#endif

            const auto latestAfterReserve =
                latestPendingParameterSnapshotToken.load(std::memory_order_acquire);
            if (pendingSnapshotTokenSlot(latestAfterReserve) == static_cast<int>(candidateSlot)
                && pendingSnapshotTokenSequence(latestAfterReserve) > publishSequence)
            {
                pendingParameterSnapshotStates[candidateSlot].store(
                    expectedState,
                    std::memory_order_release);
                continue;
            }

            const auto targetDynamicFx = resolvedDynamicFxSnapshot(snapshot);
            const auto requiresControlThreadFxBuild =
                mayApplyControlThreadFxRebuild
                && (! hasAppliedDynamicFxSnapshot
                    || dynamicFxTopologyDiffers(targetDynamicFx,
                                                appliedDynamicFxSnapshot));
            uint64_t preparedFxSequence = 0;
            // A parameter/asset update may supersede a control-prepared snapshot
            // before its first render. Preserve an identical prepared FX graph
            // while borrowing the old slot through the existing reader claim.
            if (! mayApplyControlThreadFxRebuild)
            {
                const auto previousToken = latestPendingParameterSnapshotToken.load(std::memory_order_acquire);
                const auto previousSlot = pendingSnapshotTokenSlot(previousToken);
                if (previousSlot >= 0 && previousSlot < static_cast<int>(pendingParameterSnapshotSlotCount)
                    && previousSlot != static_cast<int>(candidateSlot))
                {
                    const auto previous = static_cast<size_t>(previousSlot);
                    auto readyState = PendingParameterSnapshotState::ready;
                    if (pendingParameterSnapshotStates[previous].compare_exchange_strong(
                            readyState, PendingParameterSnapshotState::reading,
                            std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        const auto sequence = pendingParameterSnapshotPreparedFxSequences[previous].load(std::memory_order_acquire);
                        if (pendingParameterSnapshotMayApplyControlThreadFxRebuild[previous].load(std::memory_order_acquire)
                            && sequence != 0
                            && pendingParameterSnapshotSequences[previous].load(std::memory_order_acquire) == pendingSnapshotTokenSequence(previousToken)
                            && latestPendingParameterSnapshotToken.load(std::memory_order_acquire) == previousToken
                            && fxChain.getLatestDesiredStateSequence() == sequence
                            && dynamicFxSnapshotsEquivalent(targetDynamicFx,
                                resolvedDynamicFxSnapshot(pendingParameterSnapshots[previous])))
                            preparedFxSequence = sequence;
                        pendingParameterSnapshotStates[previous].store(
                            PendingParameterSnapshotState::ready, std::memory_order_release);
                    }
                }
            }
            uint64_t displacedToken = 0;
            if (requiresControlThreadFxBuild)
            {
                const auto barrierToken = makePendingSnapshotBarrierToken(publishSequence);
                bool ownsBarrier = false;
                for (;;)
                {
                    auto currentToken = latestPendingParameterSnapshotToken.load(
                        std::memory_order_acquire);
                    if (pendingSnapshotTokenSequence(currentToken) > publishSequence)
                        break;

                    displacedToken = currentToken;
                    if (latestPendingParameterSnapshotToken.compare_exchange_weak(
                            currentToken,
                            barrierToken,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire))
                    {
                        ownsBarrier = true;
                        break;
                    }
                }

                if (! ownsBarrier)
                {
                    pendingParameterSnapshotStates[candidateSlot].store(
                        PendingParameterSnapshotState::empty,
                        std::memory_order_release);
                    signalFlowDirty = true;
                    return true;
                }

                preparedFxSequence =
                    prepareDynamicFxSnapshotForDeferredAcceptanceOnControlThread(targetDynamicFx);
                if (preparedFxSequence == 0)
                {
                    auto expectedBarrier = barrierToken;
                    latestPendingParameterSnapshotToken.compare_exchange_strong(
                        expectedBarrier,
                        displacedToken,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire);
                    pendingParameterSnapshotStates[candidateSlot].store(
                        PendingParameterSnapshotState::empty,
                        std::memory_order_release);
                    signalFlowDirty = true;
                    return false;
                }
            }

            pendingParameterSnapshots[candidateSlot] = snapshot;
            pendingParameterSnapshotMayApplyControlThreadFxRebuild[candidateSlot].store(
                preparedFxSequence != 0,
                std::memory_order_release);
            pendingParameterSnapshotPreparedFxSequences[candidateSlot].store(
                preparedFxSequence,
                std::memory_order_release);
            pendingParameterSnapshotSequences[candidateSlot].store(publishSequence,
                                                                   std::memory_order_release);
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
            if (blockPendingParameterSnapshotPublishForTest != nullptr
                && blockPendingParameterSnapshotPublishForTest->load(std::memory_order_acquire))
            {
                if (pendingParameterSnapshotPublishEnteredForTest != nullptr)
                    pendingParameterSnapshotPublishEnteredForTest->signal();
                if (allowPendingParameterSnapshotPublishForTest != nullptr)
                    allowPendingParameterSnapshotPublishForTest->wait();
            }
#endif
            const auto publishToken = makePendingSnapshotToken(publishSequence, candidateSlot);
            bool publishedAsLatest = false;
            for (;;)
            {
                auto currentToken = latestPendingParameterSnapshotToken.load(std::memory_order_acquire);
                if (pendingSnapshotTokenSequence(currentToken) > publishSequence)
                    break;
                if (latestPendingParameterSnapshotToken.compare_exchange_weak(
                        currentToken,
                        publishToken,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    publishedAsLatest = true;
                    break;
                }
            }

            // Keep exclusive ownership until token publication has finished.
            // A ready slot whose token is not yet published could otherwise be
            // reclaimed by another writer, then emptied by this writer's cleanup.
            if (publishedAsLatest)
            {
                pendingParameterSnapshotStates[candidateSlot].store(
                    PendingParameterSnapshotState::ready, std::memory_order_release);
            }
            else
            {
                pendingParameterSnapshotMayApplyControlThreadFxRebuild[candidateSlot].store(
                    false, std::memory_order_release);
                pendingParameterSnapshotPreparedFxSequences[candidateSlot].store(
                    0, std::memory_order_release);
                pendingParameterSnapshotSequences[candidateSlot].store(0, std::memory_order_release);
                pendingParameterSnapshotStates[candidateSlot].store(
                    PendingParameterSnapshotState::empty, std::memory_order_release);
            }

            signalFlowDirty = true;
            return true;
        }
    }

    signalFlowDirty = true;
    return false;
}

void RenderEngine::invalidatePendingParameterSnapshots() noexcept
{
    const auto barrierSequence =
        pendingParameterSnapshotNextSequence.fetch_add(1, std::memory_order_acq_rel);
    latestPendingParameterSnapshotToken.store(makePendingSnapshotBarrierToken(barrierSequence),
                                              std::memory_order_release);
}

void RenderEngine::applyPendingParameterSnapshotFromAudioThread() noexcept
{
    const auto latestToken = latestPendingParameterSnapshotToken.load(std::memory_order_acquire);
    const auto slotIndex = pendingSnapshotTokenSlot(latestToken);
    if (slotIndex < 0
        || slotIndex >= static_cast<int>(pendingParameterSnapshotSlotCount))
    {
        return;
    }

    const auto slot = static_cast<size_t>(slotIndex);
    auto expectedState = PendingParameterSnapshotState::ready;
    if (! pendingParameterSnapshotStates[slot].compare_exchange_strong(
            expectedState,
            PendingParameterSnapshotState::reading,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return;
    }

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    if (blockPendingParameterSnapshotReadForTest != nullptr
        && blockPendingParameterSnapshotReadForTest->load(std::memory_order_acquire))
    {
        if (pendingParameterSnapshotReadEnteredForTest != nullptr)
            pendingParameterSnapshotReadEnteredForTest->signal();
        if (allowPendingParameterSnapshotReadForTest != nullptr)
            allowPendingParameterSnapshotReadForTest->wait();
    }
#endif

    const auto expectedSequence = pendingSnapshotTokenSequence(latestToken);
    const auto slotSequence = pendingParameterSnapshotSequences[slot].load(std::memory_order_acquire);
    const auto snapshotToApply = pendingParameterSnapshots[slot];
    const bool tokenMatchesSlot = slotSequence == expectedSequence;
    const auto preparedOnControlThread =
        pendingParameterSnapshotMayApplyControlThreadFxRebuild[slot].load(
            std::memory_order_acquire);
    const auto preparedFxSequence =
        pendingParameterSnapshotPreparedFxSequences[slot].load(std::memory_order_acquire);

    const wavetable::RuntimeWavetableBank::PublishedOwnership* pinnedOwnership = nullptr;
    if (tokenMatchesSlot
        && ! acquireNoiseSnapshotOwnership(wavetableBank, snapshotToApply, pinnedOwnership))
    {
        pendingParameterSnapshotStates[slot].store(PendingParameterSnapshotState::ready,
                                                   std::memory_order_release);
        return;
    }
    const auto releaseOwnership = fad::engine::detail::ScopeExit{[&pinnedOwnership] {
        if (pinnedOwnership != nullptr)
            pinnedOwnership->audioReaderCount.fetch_sub(1, std::memory_order_acq_rel);
    }};

    if (tokenMatchesSlot && preparedOnControlThread)
    {
        auto expectedLatestToken = latestToken;
        const auto barrierToken = makePendingSnapshotBarrierToken(expectedSequence);
        if (! latestPendingParameterSnapshotToken.compare_exchange_strong(
                expectedLatestToken,
                barrierToken,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            pendingParameterSnapshotMayApplyControlThreadFxRebuild[slot].store(
                false,
                std::memory_order_release);
            pendingParameterSnapshotPreparedFxSequences[slot].store(
                0,
                std::memory_order_release);
            pendingParameterSnapshotSequences[slot].store(0, std::memory_order_release);
            pendingParameterSnapshotStates[slot].store(PendingParameterSnapshotState::empty,
                                                       std::memory_order_release);
            return;
        }

        if (preparedFxSequence == 0
            || ! fxChain.tryAcceptPreparedState(preparedFxSequence))
        {
            auto expectedBarrier = barrierToken;
            if (latestPendingParameterSnapshotToken.compare_exchange_strong(
                    expectedBarrier,
                    latestToken,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                pendingParameterSnapshotStates[slot].store(PendingParameterSnapshotState::ready,
                                                           std::memory_order_release);
            }
            else
            {
                pendingParameterSnapshotMayApplyControlThreadFxRebuild[slot].store(
                    false,
                    std::memory_order_release);
                pendingParameterSnapshotPreparedFxSequences[slot].store(
                    0,
                    std::memory_order_release);
                pendingParameterSnapshotSequences[slot].store(0, std::memory_order_release);
                pendingParameterSnapshotStates[slot].store(PendingParameterSnapshotState::empty,
                                                           std::memory_order_release);
            }
            return;
        }

        recordAppliedDynamicFxSnapshot(resolvedDynamicFxSnapshot(snapshotToApply));
        pendingParameterSnapshotMayApplyControlThreadFxRebuild[slot].store(
            false,
            std::memory_order_release);
        pendingParameterSnapshotPreparedFxSequences[slot].store(
            0,
            std::memory_order_release);
        pendingParameterSnapshotSequences[slot].store(0, std::memory_order_release);
        pendingParameterSnapshotStates[slot].store(PendingParameterSnapshotState::empty,
                                                   std::memory_order_release);
        adoptNoiseSnapshotOwnership(blockPublishedOwnership, pinnedOwnership);
        applyParameterSnapshotNow(snapshotToApply, true);
        return;
    }

    if (tokenMatchesSlot
        && latestPendingParameterSnapshotToken.load(std::memory_order_acquire) == latestToken
        && ! tryPrepareDynamicFxSnapshotForAudioThread(
            resolvedDynamicFxSnapshot(snapshotToApply)))
    {
        pendingParameterSnapshotStates[slot].store(PendingParameterSnapshotState::ready,
                                                   std::memory_order_release);
        return;
    }

    pendingParameterSnapshotMayApplyControlThreadFxRebuild[slot].store(
        false,
        std::memory_order_release);
    pendingParameterSnapshotPreparedFxSequences[slot].store(0, std::memory_order_release);
    auto expectedLatestToken = latestToken;
    const bool isStillLatest =
        tokenMatchesSlot
        && latestPendingParameterSnapshotToken.compare_exchange_strong(
            expectedLatestToken,
            makePendingSnapshotBarrierToken(expectedSequence),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    pendingParameterSnapshotSequences[slot].store(0, std::memory_order_release);
    pendingParameterSnapshotStates[slot].store(PendingParameterSnapshotState::empty,
                                               std::memory_order_release);
    if (! isStillLatest)
        return;

    adoptNoiseSnapshotOwnership(blockPublishedOwnership, pinnedOwnership);
    applyParameterSnapshotNow(snapshotToApply, true);
}

bool RenderEngine::setParameterSnapshot(const parameters::ParameterSnapshot& snapshot) noexcept
{
    if (resetRequested.load(std::memory_order_acquire)
        || resetFadeRemainingSamples.load(std::memory_order_acquire) > 0)
    {
        return tryQueuePendingParameterSnapshot(snapshot, true);
    }

    invalidatePendingParameterSnapshots();
    for (auto& mayApplyControlThreadFxRebuild : pendingParameterSnapshotMayApplyControlThreadFxRebuild)
        mayApplyControlThreadFxRebuild.store(false, std::memory_order_release);
    for (auto& preparedFxSequence : pendingParameterSnapshotPreparedFxSequences)
        preparedFxSequence.store(0, std::memory_order_release);
    applyParameterSnapshotNowOnControlThread(snapshot);
    return true;
}

bool RenderEngine::setParameterSnapshotFromAudioThread(const parameters::ParameterSnapshot& snapshot) noexcept
{
    if (resetRequested.load(std::memory_order_acquire)
        || resetFadeRemainingSamples.load(std::memory_order_acquire) > 0)
    {
        return tryQueuePendingParameterSnapshot(snapshot, false);
    }

    const wavetable::RuntimeWavetableBank::PublishedOwnership* pinnedOwnership = nullptr;
    if (! acquireNoiseSnapshotOwnership(wavetableBank, snapshot, pinnedOwnership))
        return false;
    const auto releaseOwnership = fad::engine::detail::ScopeExit{[&pinnedOwnership] {
        if (pinnedOwnership != nullptr)
            pinnedOwnership->audioReaderCount.fetch_sub(1, std::memory_order_acq_rel);
    }};

    if (! tryPrepareDynamicFxSnapshotForAudioThread(resolvedDynamicFxSnapshot(snapshot)))
        return false;

    invalidatePendingParameterSnapshots();
    adoptNoiseSnapshotOwnership(blockPublishedOwnership, pinnedOwnership);
    applyParameterSnapshotNow(snapshot, true);
    return true;
}

void RenderEngine::applyDynamicFxSnapshot(const parameters::DynamicFxSnapshot& snapshot,
                                         bool forceAll,
                                         bool updateReadableActiveFxProcessors,
                                         bool recordAsApplied,
                                         bool requireExplicitPublicationAcceptance)
{
    fx::FxBackend::ScopedBatchUpdate batchUpdate(
        fxChain,
        requireExplicitPublicationAcceptance);
    const auto slotCount = std::clamp(snapshot.slotCount, 0, parameters::ids::maxFxSlots);
    const auto previousCount =
        hasAppliedDynamicFxSnapshot
            ? std::clamp(appliedDynamicFxSnapshot.slotCount, 0, parameters::ids::maxFxSlots)
            : 0;

    const auto slotsToCheck = forceAll
        ? parameters::ids::maxFxSlots
        : std::max(slotCount, previousCount);
    std::array<bool, parameters::ids::maxFxSlots> slotTypeChanged{};
    for (int slotIndex = 0; slotIndex < slotsToCheck; ++slotIndex)
    {
        const auto desiredType = slotIndex < slotCount
                                     ? fx::FxCatalog::sanitizeTypeValue(
                                           snapshot.slots[static_cast<size_t>(slotIndex)].type)
                                     : fx::FxType::none;
        const auto previousType =
            hasAppliedDynamicFxSnapshot && slotIndex < previousCount
                ? fx::FxCatalog::sanitizeTypeValue(
                      appliedDynamicFxSnapshot.slots[static_cast<size_t>(slotIndex)].type)
                : fx::FxType::none;

        slotTypeChanged[static_cast<size_t>(slotIndex)] = desiredType != previousType;
        if (forceAll || slotTypeChanged[static_cast<size_t>(slotIndex)])
            fxChain.setSlotType(slotIndex, desiredType);
    }

    bool orderChanged = forceAll || ! hasAppliedDynamicFxSnapshot || slotCount != previousCount;
    std::array<int, parameters::ids::maxFxSlots> slotOrder =
        parameters::sanitizeDynamicFxSlotOrder(snapshot, slotCount);
    for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
    {
        const auto order = snapshot.slots[static_cast<size_t>(slotIndex)].order;
        if (! orderChanged
            && order != appliedDynamicFxSnapshot.slots[static_cast<size_t>(slotIndex)].order)
        {
            orderChanged = true;
        }
    }

    if (orderChanged)
        fxChain.setSlotOrder(slotOrder, slotCount);

    if (forceAll || ! hasAppliedDynamicFxSnapshot || slotCount != previousCount)
        fxChain.setSlotCount(slotCount);

    for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
    {
        const auto& desiredSlot = snapshot.slots[static_cast<size_t>(slotIndex)];
        const auto& previousSlot = appliedDynamicFxSnapshot.slots[static_cast<size_t>(slotIndex)];

        if (forceAll || !hasAppliedDynamicFxSnapshot
            || parameters::resolvedFxRoutingStage(slotIndex, desiredSlot.routingStage)
                != parameters::resolvedFxRoutingStage(slotIndex, previousSlot.routingStage)
            || desiredSlot.routingLane != previousSlot.routingLane)
            fxChain.setSlotRouting(slotIndex, desiredSlot.routingStage, desiredSlot.routingLane);

        if (forceAll || ! hasAppliedDynamicFxSnapshot || desiredSlot.enabled != previousSlot.enabled)
            fxChain.setSlotEnabled(slotIndex, desiredSlot.enabled);

        const auto replacementParamCount = fx::FxCatalog::getParamCountForType(
            fx::FxCatalog::sanitizeTypeValue(desiredSlot.type));
        for (int paramIndex = 0; paramIndex < replacementParamCount; ++paramIndex)
        {
            const auto desiredValue = desiredSlot.params[static_cast<size_t>(paramIndex)];
            const auto previousValue = previousSlot.params[static_cast<size_t>(paramIndex)];

            if (forceAll
                || slotTypeChanged[static_cast<size_t>(slotIndex)]
                || ! hasAppliedDynamicFxSnapshot
                || ! nearlyEqual(desiredValue, previousValue))
            {
                fxChain.setSlotParam(slotIndex,
                                     paramIndex,
                                     desiredValue,
                                     updateReadableActiveFxProcessors);
            }
        }
    }

    for(int slotIndex=0;slotIndex<std::clamp(snapshot.slotCount, 0, parameters::ids::maxFxSlots);++slotIndex) {
        const auto& s=snapshot.slots[static_cast<size_t>(slotIndex)];
        fxChain.setSlotParam(slotIndex,26,fx::wideFxEncode(26,s.wideRateHz),updateReadableActiveFxProcessors);
        fxChain.setSlotParam(slotIndex,27,fx::wideFxEncode(27,s.wideLeftMs),updateReadableActiveFxProcessors);
        fxChain.setSlotParam(slotIndex,28,fx::wideFxEncode(28,s.wideRightMs),updateReadableActiveFxProcessors);
    }
    if (recordAsApplied)
        recordAppliedDynamicFxSnapshot(snapshot);
}

void RenderEngine::recordAppliedDynamicFxSnapshot(
    const parameters::DynamicFxSnapshot& snapshot) noexcept
{
    appliedDynamicFxSnapshot = snapshot;
    appliedDynamicFxSnapshot.slotCount =
        std::clamp(snapshot.slotCount, 0, parameters::ids::maxFxSlots);
    for (int slotIndex = appliedDynamicFxSnapshot.slotCount;
         slotIndex < parameters::ids::maxFxSlots;
         ++slotIndex)
    {
        appliedDynamicFxSnapshot.slots[static_cast<size_t>(slotIndex)] = {};
    }

    hasAppliedDynamicFxSnapshot = true;
}

bool RenderEngine::tryPrepareDynamicFxSnapshotForAudioThread(
    const parameters::DynamicFxSnapshot& snapshot) noexcept
{
    const auto targetChangedWhilePending = hasPendingAudioThreadDynamicFxSnapshot
        && ! dynamicFxSnapshotsEquivalent(snapshot, pendingAudioThreadDynamicFxSnapshot);
    const auto needsStructuralPublication = ! hasPendingAudioThreadDynamicFxSnapshot
        && (! hasAppliedDynamicFxSnapshot
            || dynamicFxTopologyDiffers(snapshot, appliedDynamicFxSnapshot));

    if (targetChangedWhilePending || needsStructuralPublication)
    {
        applyDynamicFxSnapshot(snapshot,
                               true,
                               false,
                               false,
                               true);
        pendingAudioThreadDynamicFxSnapshot = snapshot;
        hasPendingAudioThreadDynamicFxSnapshot = true;
    }

    if (! hasPendingAudioThreadDynamicFxSnapshot)
        return true;

    if (! fxChain.tryAcceptLatestPreparedState())
        return false;

    recordAppliedDynamicFxSnapshot(pendingAudioThreadDynamicFxSnapshot);
    hasPendingAudioThreadDynamicFxSnapshot = false;
    return true;
}

void RenderEngine::setTransportBpm(double newTempoBpm) noexcept
{
    tempoBpm = std::clamp(std::isfinite(newTempoBpm) ? newTempoBpm : 120.0, 20.0, 400.0);
    fxChain.setTransportBpm(tempoBpm);
    sessionMetrics.lastBpm.store(static_cast<float>(tempoBpm), std::memory_order_relaxed);
}

void RenderEngine::alignSyncedLfosToTransport(double ppqPosition) noexcept
{
    if (! std::isfinite(ppqPosition))
        return;

    for (size_t i = 0; i < globalLfos.size(); ++i)
    {
        if (i >= parameterSnapshot.modulation.lfos.size())
            break;

        const auto& lfoSnap = parameterSnapshot.modulation.lfos[i];
        if (! lfoSnap.syncToTempo || ! lfoSnap.anchor || lfoSnap.triggerMode > 0)
            continue;

        const auto safeRateHz = std::isfinite(lfoSnap.rateHz)
            ? std::clamp(lfoSnap.rateHz, 0.01f, 40.0f)
            : 1.0f;
        const auto normRate = std::clamp(lfoRateRange().normalise(safeRateHz), 0.0f, 1.0f);
        const auto syncIndex = std::clamp(fad::engine::math::roundToNearestInt(normRate * static_cast<float>(tempoSyncCycleLengthsInBeats.size() - 1)), 0, static_cast<int>(tempoSyncCycleLengthsInBeats.size() - 1));
        const double cycleBeats = lfoSnap.cycleBeats > 0.0f && std::isfinite(lfoSnap.cycleBeats)
            ? std::clamp(static_cast<double>(lfoSnap.cycleBeats), 0.0078125, 128.0)
            : static_cast<double>(tempoSyncCycleLengthsInBeats[static_cast<size_t>(syncIndex)]);
        if (! std::isfinite(cycleBeats) || cycleBeats <= 0.0)
            continue;

        const auto phaseRatio = static_cast<float>(
            std::fmod(ppqPosition, cycleBeats) / cycleBeats);
        if (! std::isfinite(phaseRatio))
            continue;
        globalLfos[i].setPhase(phaseRatio * (2.0f * std::numbers::pi_v<float>));
    }
}

void RenderEngine::setTransportPosition(double ppqPosition, bool isPlaying, int blockSize) noexcept
{
    const bool hasPosition = std::isfinite(ppqPosition);
    if (! hasPosition)
        ppqPosition = lastTransportPpqPosition;

    // Session metrics: track transport start events
    if (isPlaying && ! wasPlaying)
        sessionMetrics.transportStartCount.fetch_add(1, std::memory_order_relaxed);

    // The distance between block starts belongs to the preceding block,
    // including that block's tempo and sample rate.
    const auto expectedPpqAdvance = lastTransportPpqAdvance;
    const auto observedPpqAdvance = ppqPosition - lastTransportPpqPosition;
    const auto ppqTolerance = std::max(0.01, expectedPpqAdvance * 4.0 + 0.01);
    const bool transportJumped =
        isPlaying
        && wasPlaying
        && hasLastTransportPpqPosition
        && hasPosition
        && std::abs(observedPpqAdvance - expectedPpqAdvance) > ppqTolerance;

    if (isPlaying && hasPosition
        && (! wasPlaying || ! hasLastTransportPpqPosition || transportJumped))
        alignSyncedLfosToTransport(ppqPosition);

    wasPlaying = isPlaying;
    lastTransportPpqPosition = ppqPosition;
    hasLastTransportPpqPosition = hasPosition;
    lastTransportPpqAdvance = tempoBpm / 60.0
        * static_cast<double>(std::max(0, blockSize))
        / std::max(1.0, lastSampleRate);
}

void RenderEngine::render(fad::engine::AudioBufferView audioBuffer,
                         midi::MidiEventSpan midiBuffer,
                         const SupplementalMidiEvent* supplementalMidiEvents,
                         size_t supplementalMidiEventCount)
{
    renderInternal(audioBuffer,
                   midiBuffer,
                   audioBuffer.getNumSamples(),
                   supplementalMidiEvents,
                   supplementalMidiEventCount);
}

void RenderEngine::renderWithParameterTimeline(
    fad::engine::AudioBufferView audioBuffer, midi::MidiEventSpan midiBuffer,
    RenderParameterTimeline& parameters,
    const SupplementalMidiEvent* supplementalMidiEvents, size_t supplementalMidiEventCount)
{
    renderInternal(audioBuffer, midiBuffer, audioBuffer.getNumSamples(),
                   supplementalMidiEvents, supplementalMidiEventCount, &parameters);
}

void RenderEngine::renderBypassTransition(
    fad::engine::AudioBufferView audioBuffer,
    midi::MidiEventSpan midiBuffer,
    int wetSampleCount,
    const SupplementalMidiEvent* supplementalMidiEvents,
    size_t supplementalMidiEventCount)
{
    renderInternal(audioBuffer,
                   midiBuffer,
                   wetSampleCount,
                   supplementalMidiEvents,
                   supplementalMidiEventCount);
}

void RenderEngine::renderInternal(fad::engine::AudioBufferView audioBuffer,
                                 midi::MidiEventSpan midiBuffer,
                                 int requestedWetSampleCount,
                                 const SupplementalMidiEvent* supplementalMidiEvents,
                                 size_t supplementalMidiEventCount,
                                 RenderParameterTimeline* renderParameters)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;
    const bool ownsAudioCallback = ! fxChain.isAudioCallbackActive();
    if (ownsAudioCallback)
        fxChain.beginAudioCallback();
    const auto closeCallback = fad::engine::detail::ScopeExit{[this, ownsAudioCallback] {
        if (ownsAudioCallback) fxChain.endAudioCallback();
    }};
    const auto suppliedSupplementalCount = supplementalMidiEvents != nullptr ? supplementalMidiEventCount : 0;
    if (midiBuffer.size() > midiEventCapacity || suppliedSupplementalCount > midiEventCapacity)
        reportMidiInputOverflow();
    midiBuffer = midiBuffer.first(std::min(midiBuffer.size(), midiEventCapacity));
    const auto supplementalCount = std::min(suppliedSupplementalCount, midiEventCapacity);
    const auto totalSampleCount = audioBuffer.getNumSamples();
    const auto wetSampleCount = std::clamp(requestedWetSampleCount, 0, totalSampleCount);

    // A running fade already services reset requests at its end. Restarting
    // it postpones captured MIDI indefinitely. Finish replay before accepting
    // another reset, retaining that request for the next block.
    if (resetFadeRemainingSamples.load(std::memory_order_acquire) > 0)
        resetRequested.exchange(false, std::memory_order_acq_rel);
    else if (resetDeferredMidiCount == 0
             && resetRequested.exchange(false, std::memory_order_acq_rel))
    {
        resetDeferredMidiBlockBase = 0;
        resetDeferredMidiReplayBase = 0;
        resetDeferredNoteCount = 0;
        resetDeferredMidiOverflowed = false;

        if (hasActiveVoices()
            || renderedAudibleState.load(std::memory_order_relaxed))
        {
            resetFadeRemainingSamples.store(resetFadeTotalSamples, std::memory_order_release);
            resetFadeTotalAtStartSamples = resetFadeTotalSamples;
        }
        else
        {
            resetFadeRemainingSamples.store(0, std::memory_order_release);
            resetFadeTotalAtStartSamples = resetFadeTotalSamples;
            resetVoiceState();
            applyPendingParameterSnapshotFromAudioThread();
        }
    }

    const auto resetFadeRemainingAtBlockStart =
        resetFadeRemainingSamples.load(std::memory_order_acquire);
    bool resetFadeActive = resetFadeRemainingAtBlockStart > 0;
    const auto resetFadeEndSample = std::min(totalSampleCount, resetFadeRemainingAtBlockStart);
    if (! resetFadeActive)
        applyPendingParameterSnapshotFromAudioThread();
    fxChain.closeAudioAcceptanceBoundary();

    // The processor publishes its restored snapshot after prepare(). No audio
    // has sounded yet: start at that gain, rather than ramping from Init/last
    // session gain for the first 20 ms. Later automation keeps its smoothing.
    if (outputGainNeedsInitialSnapshot && totalSampleCount > 0)
    {
        outputGainSmoother.setCurrentAndTargetValue(
            parameterSnapshot.global.outputGainLinear * parameterSnapshot.global.presetGain);
        outputGainNeedsInitialSnapshot = false;
    }

    const bool diagEnabled = diagnosticsEnabled.load(std::memory_order_relaxed);
    const int refreshCountdown = diagnosticsRefreshCountdown.load(std::memory_order_relaxed);
    const bool refreshDiagnostics = diagEnabled && refreshCountdown <= 0;

    if (refreshDiagnostics)
        totalCpuMeter.beginMeasurement();

    const auto refreshGlobalLfoRates = [&]
    {
        for (size_t lfoIndex = 0; lfoIndex < globalLfos.size(); ++lfoIndex)
        {
            const auto rate = resolveLfoRateHz(parameterSnapshot.modulation.lfos[lfoIndex]);
            if((nativeRateRouteMask&(uint32_t{1}<<lfoIndex))==0) {
                globalLfos[lfoIndex].setRate(rate);
                fxNoteLfos[lfoIndex].setRate(rate);
            }
        }
    };
    refreshGlobalLfoRates();
    fxModFrameStart = -1;
    fxModFrameCount = 0;

    audioBuffer.clear();
    if (refreshDiagnostics)
        diagnosticsRefreshCountdown.store(diagnosticsRefreshIntervalBlocks - 1, std::memory_order_relaxed);
    else if (diagEnabled)
        diagnosticsRefreshCountdown.store(std::max(0, refreshCountdown - 1), std::memory_order_relaxed);

    propagateSnapshotToVoices(refreshDiagnostics);
    int renderPosition = 0;
    int wetProcessingPosition = 0;
    float wetPeakLeft = 0.0f;
    float wetPeakRight = 0.0f;
    bool wetPrefixFinalized = false;
    const auto needsDirectAuxBus = [&] { return parameterSnapshot.subDirectOut || parameterSnapshot.noise.directOut
        || std::any_of(voiceCache.begin(), voiceCache.end(), [](const VoiceCore* voice) {
            return voice != nullptr && voice->needsDirectOutBus();
        }); };
    bool useDirectAuxBus = needsDirectAuxBus();
    int directAuxStart = -1;
    int directAuxEnd = -1;

    const auto processWetAudioThrough = [&](int endSample)
    {
        endSample = std::clamp(endSample, wetProcessingPosition, wetSampleCount);
        while (wetProcessingPosition < endSample)
        {
            const auto panicRemaining = allSoundOffSamplesRemaining;
            const auto count = panicRemaining > 0
                ? std::min(endSample - wetProcessingPosition, panicRemaining)
                : endSample - wetProcessingPosition;
            auto wetSegment = audioBuffer.subview(wetProcessingPosition, count);
            const auto sourceOffset = wetProcessingPosition - fxModFrameStart;
            const bool hasSources = (hasNativeFxRoutes || hasNativeFxClock || hasEqCurveModulation) && fxModFrameStart >= 0 && sourceOffset >= 0
                && sourceOffset + count <= fxModFrameCount;
            fxChain.setNativeModulationInput(hasSources ? &parameterSnapshot.modulation : nullptr,
                hasSources ? fxModSourceFrames.data() + sourceOffset : nullptr, hasSources ? count : 0);
            fxChain.setEqCurveInput(&parameterSnapshot.curves);
            processFx(wetSegment, refreshDiagnostics);
            fxChain.setEqCurveInput(nullptr);
            fxChain.setNativeModulationInput(nullptr, nullptr, 0);
            if (directAuxStart >= 0 && wetProcessingPosition >= directAuxStart
                && wetProcessingPosition + count <= directAuxEnd) {
                const auto directOffset = wetProcessingPosition - directAuxStart;
                for (int channel=0; channel<std::min(2, wetSegment.getNumChannels()); ++channel)
                    wetSegment.addFrom(channel,0,directAuxScratch,channel,directOffset,count);
            }
            applyOutputGain(wetSegment);
            sessionMetrics.recordEmergencyLimitedSamples(audioSafetyStage.process(wetSegment));
            // The complete instrument bus (including direct sources and user
            // FX) needs musical output headroom after the emergency/DC guard.
            // Dry host bypass remains transparent and never enters this stage.
            auto outputPeaks = outputPeakLimiter.process(wetSegment);
            if (allSoundOffOutputGain.isSmoothing()
                || allSoundOffOutputGain.getCurrentValue() < 1.0f)
            {
                // Panic retires the complete wet path, including the safety
                // filter's residual state, on the same sample timeline as the
                // voice fade. Never clear these states at a host-block edge.
                outputPeaks = {};
                for (int sample = 0; sample < count; ++sample)
                {
                    const auto panicGain = allSoundOffOutputGain.getCurrentValue();
                    (void) (allSoundOffOutputGain.getNextValue());
                    for (int channel = 0; channel < wetSegment.getNumChannels(); ++channel)
                    {
                        auto& value = wetSegment.getWritePointer(channel)[sample];
                        value *= panicGain;
                        if (channel < 2)
                            outputPeaks[static_cast<size_t>(channel)] = std::max(outputPeaks[static_cast<size_t>(channel)], std::abs(value));
                    }
                }
                if (wetSegment.getNumChannels() == 1)
                    outputPeaks[1] = outputPeaks[0];
            }
            if (panicRemaining > 0)
            {
                allSoundOffSamplesRemaining -= count;
                if (allSoundOffSamplesRemaining == 0)
                {
                    fxChain.reset();
                    audioSafetyStage.reset();
                    outputPeakLimiter.reset();
                }
            }
            wetPeakLeft = std::max(wetPeakLeft, outputPeaks[0]);
            wetPeakRight = std::max(wetPeakRight, outputPeaks[1]);
            // Fade the complete old wet bus before clearing its DSP state.
            // Absolute sample indices keep the gain independent of host and
            // parameter-timeline segmentation.
            const auto fadeCount = std::min(count, resetFadeEndSample - wetProcessingPosition);
            if (fadeCount > 0)
            {
                const auto fadeScale = 1.0f / static_cast<float>(std::max(1, resetFadeTotalAtStartSamples));
                for (int channel = 0; channel < wetSegment.getNumChannels(); ++channel)
                {
                    auto* samples = wetSegment.getWritePointer(channel);
                    for (int sample = 0; sample < fadeCount; ++sample)
                        samples[sample] *= static_cast<float>(resetFadeRemainingAtBlockStart
                                                              - wetProcessingPosition - sample) * fadeScale;
                }
            }
            wetProcessingPosition += count;
        }
    };

    const auto finalizeWetPrefix = [&]
    {
        if (wetPrefixFinalized)
            return;

        wetPrefixFinalized = true;
        if (wetSampleCount <= 0)
        {
            renderPeakLeft.store(0.0f, std::memory_order_relaxed);
            renderPeakRight.store(0.0f, std::memory_order_relaxed);
            return;
        }

        processWetAudioThrough(wetSampleCount);
        renderPeakLeft.store(wetPeakLeft, std::memory_order_relaxed);
        renderPeakRight.store(wetPeakRight, std::memory_order_relaxed);
    };

    const auto processTimelineSegment = [&](int startSample, int numSamples)
    {
        if (numSamples <= 0)
            return;

        const auto segmentEnd = startSample + numSamples;
        const auto renderEnd = std::min(segmentEnd, wetSampleCount);
        if (startSample < renderEnd)
        {
            if ((useDirectAuxBus || hasNativeFxRoutes || hasNativeFxClock || hasEqCurveModulation) && directAuxScratch.getNumSamples() > 0)
            {
                // Drain each bounded scratch segment before reuse, including
                // hosts that supply blocks larger than the prepared capacity.
                for (int position=startSample; position<renderEnd;)
                {
                    processWetAudioThrough(position);
                    const auto count=std::min(directAuxScratch.getNumSamples(), renderEnd-position);
                    directAuxScratch.clear();
                    directAuxStart=position;directAuxEnd=position+count;
                    for(auto* voice:voiceCache) if(voice!=nullptr) voice->setDirectOutBuffer(directAuxScratch.view(),position);
                    renderChunk(audioBuffer,position,count);
                    for(auto* voice:voiceCache) if(voice!=nullptr) voice->clearDirectOutBuffer();
                    processWetAudioThrough(position+count);
                    position+=count;
                }
            }
            else renderChunk(audioBuffer, startSample, renderEnd - startSample);
        }
        if (allSoundOffSamplesRemaining > 0)
            processWetAudioThrough(renderEnd);

        if (segmentEnd > wetSampleCount)
        {
            finalizeWetPrefix();
            const auto advanceStart = std::max(startSample, wetSampleCount);
            advanceBypassRuntimeState(segmentEnd - advanceStart);
        }
    };

    const midi::MidiEventSpan* sourceMidi = &midiBuffer;
    const auto dispatchTimelineMessage = [&](const midi::ShortMessage& message, int eventSample, midi::Source source = midi::Source::host, midi::UmpChannelVoice ump = {})
    {
        // CC120 mutates FX/safety history. Finish its already-rendered prefix
        // before applying that mutation, including nonzero MIDI offsets.
        if (message.isAllSoundOff()
            || (message.isNoteOn() && allSoundOffOutputGain.getTargetValue() < 1.0f))
            processWetAudioThrough(std::min(eventSample, wetSampleCount));
        dispatchMidiMessage(message, source, ump);
    };

    if (resetFadeActive)
    {
        for (const auto metadata : *sourceMidi)
        {
            const auto message = decodeSupportedHostMidiMessage(metadata);
            if (! message.has_value())
                continue;

            const auto replaySamplePosition = std::clamp(metadata.samplePosition, 0, totalSampleCount);
            if (replaySamplePosition >= resetFadeRemainingAtBlockStart)
                continue;
            const auto captureOrderKey = resetDeferredMidiBlockBase + replaySamplePosition;
            if (message->isAllNotesOff() || message->isAllSoundOff())
                removeDeferredResetNoteOns(message->getChannel());

            deferResetMidiMessage(*message, captureOrderKey);
        }

        for (size_t supplementalIndex = 0; supplementalIndex < supplementalCount; ++supplementalIndex)
        {
            const auto& event = supplementalMidiEvents[supplementalIndex];
            const auto replaySamplePosition = std::clamp(event.samplePosition, 0, totalSampleCount);
            if (replaySamplePosition >= resetFadeRemainingAtBlockStart)
                continue;
            const auto captureOrderKey =
                resetDeferredMidiBlockBase + replaySamplePosition;
            if (event.message.isAllNotesOff() || event.message.isAllSoundOff())
                removeDeferredResetNoteOns(event.message.getChannel(), event.source);

            deferResetMidiMessage(event.message, captureOrderKey, event.source, event.ump);
        }
    }

    size_t deferredIndex = 0;
    auto midiIterator = sourceMidi->begin();
    const auto midiEnd = sourceMidi->end();
    size_t supplementalIndex = 0;
    auto replayBlockStart = resetDeferredMidiReplayBase;
    auto replayBlockEnd = replayBlockStart + totalSampleCount;
    if (resetFadeActive)
    {
        // These events were captured above. Events at/after the fade boundary
        // stay on the current host timeline and must be dispatched only once.
        while (midiIterator != midiEnd
               && std::clamp((*midiIterator).samplePosition, 0, totalSampleCount)
                   < resetFadeRemainingAtBlockStart)
            ++midiIterator;
        while (supplementalIndex < supplementalCount
               && std::clamp(supplementalMidiEvents[supplementalIndex].samplePosition, 0, totalSampleCount)
                   < resetFadeRemainingAtBlockStart)
            ++supplementalIndex;
    }
    const auto deferCurrentResetNote = [&](const midi::ShortMessage& message, int eventSample,
                                           midi::Source source = midi::Source::host, midi::UmpChannelVoice ump = {})
    {
        if (message.isAllNotesOff() || message.isAllSoundOff())
            removeDeferredResetNoteOns(message.getChannel(), source, deferredIndex);
        if (!hasDeferredResetNote(message, source))
            return false;
        if (deferredIndex > 0)
        {
            size_t writeIndex = 0;
            for (size_t readIndex = deferredIndex; readIndex < resetDeferredMidiCount; ++readIndex)
                resetDeferredMidi[writeIndex++] = resetDeferredMidi[readIndex];
            resetDeferredMidiCount = writeIndex;
            deferredIndex = 0;
        }
        // Continue the same delay for this captured key's NoteOff/repeated
        // NoteOn even after the fade ends, so a release cannot precede its key.
        deferResetMidiMessage(message, replayBlockStart + eventSample + resetFadeTotalAtStartSamples,
                              source, ump);
        return true;
    };
    const auto renderMidiRange = [&](int startSample, int numSamples, bool includeEnd = false)
    {
        const auto rangeEnd = startSample + numSamples;
        if (resetFadeActive)
        {
            const auto fadeRenderEnd = std::min(rangeEnd, resetFadeEndSample);
            if (renderPosition < fadeRenderEnd)
                processTimelineSegment(renderPosition, fadeRenderEnd - renderPosition);
            renderPosition = fadeRenderEnd;
            if (resetFadeRemainingAtBlockStart > totalSampleCount || rangeEnd < resetFadeEndSample)
                return;

            processWetAudioThrough(std::min(resetFadeEndSample, wetSampleCount));
            resetFadeRemainingSamples.store(0, std::memory_order_release);
            resetDeferredMidiBlockBase = 0;
            // Offsets remain relative to capture start. A negative base lets
            // the remaining part of this block replay them at boundary+offset.
            replayBlockStart = -resetFadeEndSample;
            replayBlockEnd = replayBlockStart + totalSampleCount;
            resetDeferredMidiReplayBase = replayBlockStart;
            resetFadeTotalAtStartSamples = resetFadeTotalSamples;
            resetVoiceState();
            // The old wet bus is drained and reset. Accept the prepared
            // restored graph/snapshot before any deferred note starts.
            fxChain.beginAudioCallback();
            applyPendingParameterSnapshotFromAudioThread();
            fxChain.closeAudioAcceptanceBoundary();
            refreshGlobalLfoRates();
            propagateSnapshotToVoices(refreshDiagnostics);
            useDirectAuxBus = needsDirectAuxBus();
            resetFadeActive = false;
        }
        while (deferredIndex < resetDeferredMidiCount
               || midiIterator != midiEnd
               || supplementalIndex < supplementalCount)
        {
            const bool hasDeferred = deferredIndex < resetDeferredMidiCount
                && resetDeferredMidi[deferredIndex].absoluteSampleOffset < replayBlockEnd;
            const bool hasCurrentMidi = midiIterator != midiEnd;
            const bool hasSupplementalMidi = supplementalIndex < supplementalCount;
            if (! hasDeferred && ! hasCurrentMidi && ! hasSupplementalMidi)
                break;

            const int deferredSample = hasDeferred
                ? std::clamp(resetDeferredMidi[deferredIndex].absoluteSampleOffset - replayBlockStart, 0, audioBuffer.getNumSamples())
                : audioBuffer.getNumSamples();
            const int currentSample = hasCurrentMidi
                ? std::clamp((*midiIterator).samplePosition, 0, audioBuffer.getNumSamples())
                : audioBuffer.getNumSamples();
            const int supplementalSample = hasSupplementalMidi
                ? std::clamp(supplementalMidiEvents[supplementalIndex].samplePosition, 0, audioBuffer.getNumSamples())
                : audioBuffer.getNumSamples();
            const bool takeDeferred = hasDeferred
                && (! hasCurrentMidi || deferredSample <= currentSample)
                && (! hasSupplementalMidi || deferredSample <= supplementalSample);
            const bool takeCurrentMidi = ! takeDeferred
                && hasCurrentMidi
                && (! hasSupplementalMidi || currentSample <= supplementalSample);
            const auto eventSample = std::max(renderPosition, takeDeferred ? deferredSample
                                                             : (takeCurrentMidi ? currentSample
                                                                                : supplementalSample));

            if (eventSample > rangeEnd || (eventSample == rangeEnd && !includeEnd))
                break;

            if (eventSample > renderPosition)
                processTimelineSegment(renderPosition, eventSample - renderPosition);
            if (! wetPrefixFinalized && eventSample >= wetSampleCount)
                finalizeWetPrefix();

            if (takeDeferred)
            {
                releaseDeferredResetNote(resetDeferredMidi[deferredIndex].message, resetDeferredMidi[deferredIndex].source);
                dispatchTimelineMessage(resetDeferredMidi[deferredIndex].message, eventSample, resetDeferredMidi[deferredIndex].source, resetDeferredMidi[deferredIndex].ump);
                ++deferredIndex;
            }
            else if (takeCurrentMidi)
            {
                if (const auto message = decodeSupportedHostMidiMessage(*midiIterator))
                    if (!deferCurrentResetNote(*message, eventSample))
                        dispatchTimelineMessage(*message, eventSample);

                ++midiIterator;
            }
            else
            {
                const auto& event = supplementalMidiEvents[supplementalIndex];
                if (!deferCurrentResetNote(event.message, eventSample, event.source, event.ump))
                    dispatchTimelineMessage(event.message, eventSample, event.source, event.ump);
                ++supplementalIndex;
            }
            renderPosition = eventSample;
        }

        if (renderPosition < rangeEnd)
            processTimelineSegment(renderPosition, rangeEnd - renderPosition);
        renderPosition = rangeEnd;
    };

    if (renderParameters != nullptr)
    {
        auto& parameters = *renderParameters;
        const auto beginPoint = [&](int offset) noexcept
        {
            processWetAudioThrough(offset); // Complete old FX/gain before any write.
            if (parameters.timeline.synchroniseEpoch(parameters.source.getRenderRestoreEpoch()))
                return false;
            return parameters.source.tryBeginRenderParameterBatch();
        };
        const auto endPoint = [&]() noexcept
        {
            auto scratch = parameters.cachedSnapshot;
            if (parameters.source.captureRenderParameterBatch(scratch, parameters.cachedVersion))
            {
                parameters.cachedSnapshot = scratch;
                parameters.needsEngineApply = true;
            }
            if (parameters.needsEngineApply)
            {
                parameters.needsEngineApply = !setParameterSnapshotFromAudioThread(parameters.cachedSnapshot);
                if (!parameters.needsEngineApply)
                {
                    refreshGlobalLfoRates();
                    propagateSnapshotToVoices(refreshDiagnostics);
                    useDirectAuxBus = needsDirectAuxBus();
                }
            }
            parameters.source.endRenderParameterBatch();
        };
        parameters.timeline.renderControlled(
            [&](int index) noexcept { return parameters.source.readRenderParameter(index); },
            [&](int index, float value) noexcept {
                parameters.wroteParameter |= parameters.source.writeRenderParameter(index, value);
            },
            [&](int offset, int count) noexcept { renderMidiRange(offset, count); }, beginPoint, endPoint);
    }
    else
    {
        renderMidiRange(0, totalSampleCount);
    }
    // An event at block end has no audio in this block; preserve the existing
    // MIDI lifecycle while making same-sample parameter writes precede it.
    renderMidiRange(totalSampleCount, 0, true);
    if (!resetFadeActive)
    {
        size_t writeIndex = 0;
        for (size_t readIndex = deferredIndex; readIndex < resetDeferredMidiCount; ++readIndex)
        {
            auto event = resetDeferredMidi[readIndex];
            event.absoluteSampleOffset -= replayBlockEnd;
            event.orderKey = event.absoluteSampleOffset;
            resetDeferredMidi[writeIndex++] = event;
        }
        resetDeferredMidiCount = writeIndex;
        resetDeferredMidiReplayBase = 0;
    }

    if (renderPosition < totalSampleCount)
        processTimelineSegment(renderPosition, totalSampleCount - renderPosition);
    finalizeWetPrefix();

    if (std::exchange(inputMidiOverflowPanicPending, false)) silenceAllChannelsFromAudioThread();
    releaseBlockWavetableOwnership();
    const auto activeVoiceCount = countActiveVoices(voiceCache);
    sessionMetrics.recordActiveNoteCount(activeVoiceCount);
    if (activeVoiceCount == 0 && (oscillatorUiCollecting || oscillatorUiPublishedActive))
        invalidateOscillatorUiCapture();

    if (resetFadeActive)
    {
        resetFadeRemainingSamples.store(resetFadeRemainingAtBlockStart - totalSampleCount,
                                        std::memory_order_release);
        resetDeferredMidiBlockBase += totalSampleCount;
    }

    if (refreshDiagnostics)
    {
        clipDetector.checkBuffer(diagnostics::ModuleId::masterOutput, audioBuffer);
        clipDetector.decayClipFlags();
        totalCpuMeter.endMeasurement();
        buildDiagnosticsSnapshot(audioBuffer);
    }
    else if (! diagEnabled
             && diagnosticsSnapshots[static_cast<size_t>(diagnosticsReadIndex.load(std::memory_order_acquire))].enabled)
    {
        // Audio thread clears diagnostics state when disabled
        if (const auto writeIndex = tryClaimDiagnosticsWriteSlot(); writeIndex >= 0)
        {
            diagnosticsSnapshots[static_cast<size_t>(writeIndex)] = {};
            publishDiagnosticsWriteSlot(writeIndex);
        }
        clipDetector.reset();
    }
}

void RenderEngine::advanceBypassed(midi::MidiEventSpan midiBuffer,
                                  int numSamples,
                                  const SupplementalMidiEvent* supplementalMidiEvents,
                                  size_t supplementalMidiEventCount)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;
    const bool ownsAudioCallback = ! fxChain.isAudioCallbackActive();
    if (ownsAudioCallback)
        fxChain.beginAudioCallback();
    const auto closeCallback = fad::engine::detail::ScopeExit{[this, ownsAudioCallback] {
        if (ownsAudioCallback) fxChain.endAudioCallback();
    }};
    const auto suppliedSupplementalCount = supplementalMidiEvents != nullptr ? supplementalMidiEventCount : 0;
    if (midiBuffer.size() > midiEventCapacity || suppliedSupplementalCount > midiEventCapacity)
        reportMidiInputOverflow();
    midiBuffer = midiBuffer.first(std::min(midiBuffer.size(), midiEventCapacity));
    const auto supplementalCount = std::min(suppliedSupplementalCount, midiEventCapacity);

    const auto samplesToAdvance = std::max(0, numSamples);

    // Keep bypass on the same reset/replay timeline as normal rendering.
    if (resetFadeRemainingSamples.load(std::memory_order_acquire) > 0)
        resetRequested.exchange(false, std::memory_order_acq_rel);
    else if (resetDeferredMidiCount == 0
             && resetRequested.exchange(false, std::memory_order_acq_rel))
    {
        resetDeferredMidiBlockBase = 0;
        resetDeferredMidiReplayBase = 0;
        resetDeferredNoteCount = 0;
        resetDeferredMidiOverflowed = false;

        if (hasActiveVoices()
            || renderedAudibleState.load(std::memory_order_relaxed))
        {
            resetFadeRemainingSamples.store(resetFadeTotalSamples, std::memory_order_release);
            resetFadeTotalAtStartSamples = resetFadeTotalSamples;
        }
        else
        {
            resetFadeRemainingSamples.store(0, std::memory_order_release);
            resetFadeTotalAtStartSamples = resetFadeTotalSamples;
            resetVoiceState();
            applyPendingParameterSnapshotFromAudioThread();
        }
    }

    const auto resetFadeRemainingAtBlockStart =
        resetFadeRemainingSamples.load(std::memory_order_acquire);
    bool resetFadeActive = resetFadeRemainingAtBlockStart > 0;
    int advancePosition = 0;
    if (resetFadeActive)
    {
        for (const auto metadata : midiBuffer)
        {
            const auto message = decodeSupportedHostMidiMessage(metadata);
            if (! message.has_value())
                continue;

            const auto replaySamplePosition = std::clamp(metadata.samplePosition, 0, samplesToAdvance);
            if (replaySamplePosition >= resetFadeRemainingAtBlockStart)
                continue;
            const auto captureOrderKey = resetDeferredMidiBlockBase + replaySamplePosition;
            if (message->isAllNotesOff() || message->isAllSoundOff())
                removeDeferredResetNoteOns(message->getChannel());

            deferResetMidiMessage(*message, captureOrderKey);
        }

        for (size_t supplementalIndex = 0; supplementalIndex < supplementalCount; ++supplementalIndex)
        {
            const auto& event = supplementalMidiEvents[supplementalIndex];
            const auto replaySamplePosition = std::clamp(event.samplePosition, 0, samplesToAdvance);
            if (replaySamplePosition >= resetFadeRemainingAtBlockStart)
                continue;
            const auto captureOrderKey = resetDeferredMidiBlockBase + replaySamplePosition;
            if (event.message.isAllNotesOff() || event.message.isAllSoundOff())
                removeDeferredResetNoteOns(event.message.getChannel(), event.source);

            deferResetMidiMessage(event.message, captureOrderKey, event.source, event.ump);
        }

        advancePosition = std::min(samplesToAdvance, resetFadeRemainingAtBlockStart);
        advanceBypassRuntimeState(advancePosition);

        const auto fadeEnd = resetFadeRemainingAtBlockStart - advancePosition;
        resetFadeRemainingSamples.store(fadeEnd, std::memory_order_release);
        resetDeferredMidiBlockBase = fadeEnd > 0
            ? resetDeferredMidiBlockBase + advancePosition
            : 0;
        if (fadeEnd <= 0)
        {
            resetDeferredMidiReplayBase = -advancePosition;
            resetFadeTotalAtStartSamples = resetFadeTotalSamples;
            resetVoiceState();
            fxChain.beginAudioCallback();
            applyPendingParameterSnapshotFromAudioThread();
            fxChain.closeAudioAcceptanceBoundary();
            resetFadeActive = false;
        }
    }

    if (! resetFadeActive)
        applyPendingParameterSnapshotFromAudioThread();
    fxChain.closeAudioAcceptanceBoundary();

    propagateSnapshotToVoices(false);
    for (size_t index = 0; index < globalLfos.size(); ++index)
        if ((nativeRateRouteMask & (uint32_t{1} << index)) == 0)
        {
            const auto rate = resolveLfoRateHz(parameterSnapshot.modulation.lfos[index]);
            globalLfos[index].setRate(rate);
            fxNoteLfos[index].setRate(rate);
        }

    auto midiIterator = midiBuffer.begin();
    const auto midiEnd = midiBuffer.end();
    size_t deferredIndex = 0;
    size_t supplementalIndex = 0;
    const auto replayBlockStart = resetDeferredMidiReplayBase;
    const auto replayBlockEnd = replayBlockStart + samplesToAdvance;
    if (resetFadeRemainingAtBlockStart > 0)
    {
        while (midiIterator != midiEnd
               && std::clamp((*midiIterator).samplePosition, 0, samplesToAdvance)
                   < resetFadeRemainingAtBlockStart)
            ++midiIterator;
        while (supplementalIndex < supplementalCount
               && std::clamp(supplementalMidiEvents[supplementalIndex].samplePosition, 0, samplesToAdvance)
                   < resetFadeRemainingAtBlockStart)
            ++supplementalIndex;
    }
    const auto deferCurrentResetNote = [&](const midi::ShortMessage& message, int eventSample,
                                           midi::Source source = midi::Source::host, midi::UmpChannelVoice ump = {})
    {
        if (message.isAllNotesOff() || message.isAllSoundOff())
            removeDeferredResetNoteOns(message.getChannel(), source, deferredIndex);
        if (!hasDeferredResetNote(message, source))
            return false;
        if (deferredIndex > 0)
        {
            size_t writeIndex = 0;
            for (size_t readIndex = deferredIndex; readIndex < resetDeferredMidiCount; ++readIndex)
                resetDeferredMidi[writeIndex++] = resetDeferredMidi[readIndex];
            resetDeferredMidiCount = writeIndex;
            deferredIndex = 0;
        }
        deferResetMidiMessage(message, replayBlockStart + eventSample + resetFadeTotalAtStartSamples,
                              source, ump);
        return true;
    };

    while (! resetFadeActive
           && (deferredIndex < resetDeferredMidiCount
               || midiIterator != midiEnd
               || supplementalIndex < supplementalCount))
    {
        const bool hasDeferred = deferredIndex < resetDeferredMidiCount
            && resetDeferredMidi[deferredIndex].absoluteSampleOffset < replayBlockEnd;
        const bool hasCurrentMidi = midiIterator != midiEnd;
        const bool hasSupplementalMidi = supplementalIndex < supplementalCount;
        if (! hasDeferred && ! hasCurrentMidi && ! hasSupplementalMidi)
            break;

        const int deferredSample = hasDeferred
            ? std::clamp(resetDeferredMidi[deferredIndex].absoluteSampleOffset - replayBlockStart, 0, samplesToAdvance)
            : samplesToAdvance;
        const int currentSample = hasCurrentMidi
            ? std::clamp((*midiIterator).samplePosition, 0, samplesToAdvance)
            : samplesToAdvance;
        const int supplementalSample = hasSupplementalMidi
            ? std::clamp(supplementalMidiEvents[supplementalIndex].samplePosition, 0, samplesToAdvance)
            : samplesToAdvance;
        const bool takeDeferred = hasDeferred
            && (! hasCurrentMidi || deferredSample <= currentSample)
            && (! hasSupplementalMidi || deferredSample <= supplementalSample);
        const bool takeCurrentMidi = ! takeDeferred
            && hasCurrentMidi
            && (! hasSupplementalMidi || currentSample <= supplementalSample);
        const auto eventSample = takeDeferred ? deferredSample
                                               : (takeCurrentMidi ? currentSample
                                                                  : supplementalSample);

        if (eventSample > advancePosition)
        {
            advanceBypassRuntimeState(eventSample - advancePosition);
            advancePosition = eventSample;
        }

        if (takeDeferred)
        {
            releaseDeferredResetNote(resetDeferredMidi[deferredIndex].message, resetDeferredMidi[deferredIndex].source);
            dispatchMidiMessage(resetDeferredMidi[deferredIndex].message, resetDeferredMidi[deferredIndex].source, resetDeferredMidi[deferredIndex].ump);
            ++deferredIndex;
        }
        else if (takeCurrentMidi)
        {
            if (const auto message = decodeSupportedHostMidiMessage(*midiIterator))
                if (!deferCurrentResetNote(*message, eventSample))
                    dispatchMidiMessage(*message);

            ++midiIterator;
        }
        else
        {
            const auto& event = supplementalMidiEvents[supplementalIndex];
            if (!deferCurrentResetNote(event.message, eventSample, event.source, event.ump))
                dispatchMidiMessage(event.message, event.source, event.ump);
            ++supplementalIndex;
        }
    }

    if (! resetFadeActive && advancePosition < samplesToAdvance)
        advanceBypassRuntimeState(samplesToAdvance - advancePosition);

    if (! resetFadeActive)
    {
        size_t writeIndex = 0;
        for (size_t readIndex = deferredIndex; readIndex < resetDeferredMidiCount; ++readIndex)
        {
            auto event = resetDeferredMidi[readIndex];
            event.absoluteSampleOffset -= replayBlockEnd;
            event.orderKey = event.absoluteSampleOffset;
            resetDeferredMidi[writeIndex++] = event;
        }
        resetDeferredMidiCount = writeIndex;
        resetDeferredMidiReplayBase = 0;
    }

    if (! hasHeldOrSustainedNotes())
    {
        resetMonoState();
    }

    if (std::exchange(inputMidiOverflowPanicPending, false)) silenceAllChannelsFromAudioThread();
    sessionMetrics.recordActiveNoteCount(countActiveVoices(voiceCache));
    releaseBlockWavetableOwnership();
}

void RenderEngine::processExternalOutputSafety(fad::engine::AudioBufferView buffer,
                                              bool clampToUnity) noexcept
{
    const auto channelCount = std::min(2, buffer.getNumChannels());
    const auto sampleCount = buffer.getNumSamples();

    for (int channel = 0; channel < channelCount; ++channel)
    {
        auto* samples = buffer.getWritePointer(channel);
        for (int sample = 0; sample < sampleCount; ++sample)
        {
            auto value = samples[sample];
            if (! std::isfinite(value))
                value = 0.0f;
            if (clampToUnity)
                value = std::clamp(value, -1.0f, 1.0f);
            samples[sample] = value;
        }
    }

    for (int channel = channelCount; channel < buffer.getNumChannels(); ++channel)
        buffer.clear(channel, 0, sampleCount);
}

void RenderEngine::propagateSnapshotToVoices(bool diagnosticsForThisBlock) noexcept
{
    resolveBlockWavetables();
    for (size_t sourceIndex=0;sourceIndex<midi::sourceCount;++sourceIndex)
    {
        const auto source=static_cast<midi::Source>(sourceIndex);
        const fad::engine::detail::ScopedValue<midi::Source> owner(activeMidiSource, source);
        if (midiState().appliedMpeEnabledForPedals != mpeEnabledForCurrentSource())
        {
            midiState().appliedMpeEnabledForPedals = mpeEnabledForCurrentSource();
            refreshMpePedals();
        }
    }
    if (fxModNote >= 0)
        fxModVelocity = shapeVelocity(fxModRawVelocity, parameterSnapshot.global.velocityCurve);
    (void) (realtimeProcessing);
    constexpr auto unisonVoiceLimit = realtimeUnisonRenderLimit;

    for (auto* synthVoice : voiceCache)
    {
        const fad::engine::detail::ScopedValue<midi::Source> voiceSource(activeMidiSource, synthVoice->getMidiSource());
        synthVoice->setParameterSnapshot(&parameterSnapshot);
        synthVoice->setGlobalLfos(&globalLfos);
        synthVoice->setTransportBpm(tempoBpm);
        synthVoice->setBlockLfoCurveBanks(&blockLfoCurveBanks);
        synthVoice->setBlockNoiseSample(blockNoiseSample, blockPublishedOwnership);
        synthVoice->setPerformanceStates(&midiState().performanceStates);
        synthVoice->setGlobalLfoBlockValues(&globalLfoBlockPointers);
        synthVoice->setDiagnosticsEnabled(diagnosticsForThisBlock);
        synthVoice->setBlockWavetablePointers(
            &blockWavetablePointers, blockWavetableGeneration);
        synthVoice->setPitchBendRange(effectivePitchBendRange(synthVoice->getRealtimeMidiChannel()));
        applyMpeManagerControls(synthVoice, false);
        synthVoice->setGlideTime(parameterSnapshot.global.glideTimeMs);
        synthVoice->setGlideMode(parameterSnapshot.global.glideMode);
        synthVoice->setRealtimeUnisonVoiceLimit(unisonVoiceLimit);
    }
}

bool RenderEngine::modulationUsesLfo(size_t lfoIndex) const noexcept
{
    const auto expectedSource = [lfoIndex]() noexcept
    {
        switch (lfoIndex)
        {
            case 0: return parameters::ModSource::lfo1;
            case 1: return parameters::ModSource::lfo2;
            case 2: return parameters::ModSource::lfo3;
            case 3: return parameters::ModSource::lfo4;
            default: return parameters::ModSource::none;
        }
    }();

    if (lfoIndex >= parameterSnapshot.modulation.lfos.size()
        || parameterSnapshot.modulation.lfos[lfoIndex].depth <= 0.0001f)
    {
        return false;
    }

    for (const auto& slot : parameterSnapshot.modulation.slots)
    {
        if (((slot.nativeSource<0 && expectedSource!=parameters::ModSource::none && slot.source==expectedSource)
             || slot.nativeSource==static_cast<int>(5+lfoIndex)
             || (slot.nativeSource>0 && slot.nativeAuxSource==static_cast<int>(5+lfoIndex)))
            && (slot.destination != parameters::ModDestination::none || slot.nativeTarget >= 0)
            && slot.hasSource() && slot.hasEffectiveAmount(parameterSnapshot.global.vitalAmplitude))
        {
            return true;
        }
    }

    // Parameter node modulation is a real consumer even without a matrix
    // route to a knob. Its source must remain available on the global clock.
    for (const auto& curve : parameterSnapshot.curves)
    {
        if(curve.historySlot>0 && curve.historySource==int(lfoIndex+1) && curve.historyDepth!=0.f)return true;
        if (curve.enabled)
            for (const auto& route : curve.mods)
                if (route.source == static_cast<int>(lfoIndex + 1) && route.target >= 0
                    && route.target < static_cast<int>(parameters::curveNodeCount * 3)
                    && std::isfinite(route.amount) && route.amount != 0.0f)
                    return true;
    }
    return false;
}

void RenderEngine::renderGlobalLfoBlock(int numSamples) noexcept
{
    globalLfoBlockPointers.fill(nullptr);
    if (numSamples <= 0)
        return;

    for (size_t lfoIndex = 0; lfoIndex < globalLfos.size(); ++lfoIndex)
    {
        auto& lfoBlock = globalLfoBlockStorage[lfoIndex];
        if (static_cast<size_t>(numSamples) > lfoBlock.size())
            continue;

        if (parameterSnapshot.modulation.lfos[lfoIndex].triggerMode > 0)
            continue; // Each active voice owns its triggered phase.

        if (! globalLfoUsed[lfoIndex])
        {
            std::fill_n(lfoBlock.begin(), numSamples, 0.0f);
            globalLfoBlockPointers[lfoIndex] = lfoBlock.data();
            continue;
        }

        const auto shape = parameterSnapshot.modulation.lfos[lfoIndex].shape;
        const auto* curve = blockLfoCurveBanks[lfoIndex];
        const auto* samples = curve != nullptr && curve->frames.size() > lfoIndex && curve->frames[lfoIndex].size() >= 2
            ? curve->frames[lfoIndex].data() : nullptr;
        const auto count = samples != nullptr ? curve->frames[lfoIndex].size() : 0;
        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
            lfoBlock[static_cast<size_t>(sampleIndex)] =
                samples != nullptr ? globalLfos[lfoIndex].renderCurveSampleAtOffset(samples, count, sampleIndex, false, curve->curveEndpoint(lfoIndex), curve->curveLoopStart(lfoIndex))
                    : globalLfos[lfoIndex].renderSampleAtOffset(shape, sampleIndex);

        globalLfoBlockPointers[lfoIndex] = lfoBlock.data();
    }
}

float RenderEngine::renderFxNoiseSource(bool active) noexcept
{
    if (!active || !parameterSnapshot.noise.enabled) { fxNoiseValue = 0; return 0; }
    float sample = 0;
    if (fxNoisePlayer.needsBuiltinSample()) {
        fxNoiseState ^= fxNoiseState << 13; fxNoiseState ^= fxNoiseState >> 17; fxNoiseState ^= fxNoiseState << 5;
        sample = static_cast<float>(static_cast<int32_t>(fxNoiseState)) / 2147483648.0f;
    }
    sample = fxNoisePlayer.nextStereoSample({sample, sample})[0];
    if (fxNoisePhase == 0) fxNoiseValue = utils::sanitizeUnitInterval(.5f + .5f * sample);
    fxNoisePhase = (fxNoisePhase + 1) & 3;
    return fxNoiseValue;
}

void RenderEngine::prepareFxNodeLfos() noexcept
{
    uint32_t enabledMask = 0;
    for (size_t i = 0; i < fxNodeLfos.size(); ++i)
    {
        if (!parameterSnapshot.curves[i + 2].enabled) continue;
        const auto bit = uint32_t{1} << i;
        enabledMask |= bit;
        if ((fxNodeLfoMask & bit) == 0 || parameterSnapshot.curves[i+2].history || fxNodeLfos[i].history)
            fxNodeLfos[i].prepare(modulation::modulatedCurve(parameterSnapshot.curves[i + 2],{}));
    }
    fxNodeLfoMask = enabledMask;
}

void RenderEngine::updateFxNodeLfos(const std::array<float, parameters::lfoCount>& sources) noexcept
{
    // Match the VoiceCore curve control cadence. Fixed-capacity runtimes retain
    // phase ownership in the existing global/triggered LFO clocks.
    if (fxNodeCurveCounter == 0)
        for (size_t i = 0; i < fxNodeLfos.size(); ++i)
            if ((fxNodeLfoMask & (uint32_t{1} << i)) != 0)
                fxNodeLfos[i].prepare(modulation::modulatedCurve(parameterSnapshot.curves[i + 2], sources));
    fxNodeCurveCounter = (fxNodeCurveCounter + 1) & 63;
}

void RenderEngine::renderNativeFxModulation(int startSample, int numSamples) noexcept
{
    const fad::engine::detail::ScopedValue<midi::Source> sourceScope(activeMidiSource, fxModSource);
    const auto& noise = parameterSnapshot.noise;
    fxNoisePlayer.configure(blockNoiseSample, lastSampleRate,
        NoiseSamplePlayer::playbackRate(noise.pitch, noise.fine, noise.keytrack, fxModNote), noise.oneShot, blockPublishedOwnership, noise.sampleSlot == 0);
    fxModFrameStart = startSample;
    fxModFrameCount = std::min(numSamples, static_cast<int>(fxModSourceFrames.size()));
    if (!hasNativeFxRoutes && !hasNativeSustainRoutes && !hasNativeDecayRoutes
        && !hasNativeReleaseRoutes && !hasNativeFxClock && !hasEqCurveModulation) {
        // Envelopes still follow the note timeline if a route is enabled mid-note.
        for (auto& envelope : fxModEnvelopes)
            if (! envelope.canSkipSilentSamples())
                for (int sample = 0; sample < numSamples; ++sample)
                    (void) (envelope.getNextSample());
        return;
    }
    const auto baseSustainPosition=std::sqrt(utils::sanitizeUnitInterval(parameterSnapshot.ampEnvelope.sustainLevel));
    const auto baseDecayPosition=hasNativeDecayRoutes
        ? modulation::HoldEnvelope::serumDecayPosition(parameterSnapshot.ampEnvelope.decaySeconds) : 0.0f;
    const auto baseReleasePosition=hasNativeReleaseRoutes
        ? modulation::HoldEnvelope::serumReleasePosition(parameterSnapshot.ampEnvelope.releaseSeconds) : 0.0f;
    prepareFxNodeLfos();
    auto activeCurveBanks = blockLfoCurveBanks;
    for (size_t i = 0; i < activeCurveBanks.size(); ++i)
        if ((fxNodeLfoMask & (uint32_t{1} << i)) != 0) activeCurveBanks[i] = nullptr;
    const auto fxChannel = std::clamp(fxModChannel, 1, 16);
    const auto fxIndex = static_cast<size_t>(fxChannel - 1);
    const bool mpeMember = isMpeMemberChannel(fxChannel);
    const bool frozen = mpeMember && fxModControlsFrozen;
    const auto& notePerformance = frozen ? fxModReleasedPerformance : midiState().performanceStates[fxIndex];
    const auto managerChannel = std::max(1, mpeManagerChannelFor(fxChannel));
    const auto managerIndex = static_cast<size_t>(managerChannel - 1);
    const auto& managerPerformance = midiState().performanceStates[managerIndex];
    const auto notePitch = (frozen ? fxModReleasedPitch : .5f+.5f*midiState().pitchWheelBipolar[fxIndex])
        + (fxMidi2Active ? fxMidi2Pitch-.5f : 0.0f);
    const auto noteSlide = fxMidi2SlideActive ? fxMidi2Slide
        : fxMidi2Active && !notePerformance.slideReceived ? .5f : notePerformance.slide;
    const auto managerPitchOffset = mpeMember
        ? .5f*midiState().pitchWheelBipolar[managerIndex]
            * effectivePitchBendRange(managerChannel) / std::max(1.0f, frozen ? fxModReleasedBendRange : effectivePitchBendRange(fxChannel))
        : 0.0f;
    for (int sample = 0; sample < fxModFrameCount; ++sample) {
        auto& values = fxModSourceFrames[static_cast<size_t>(sample)];
        values = {};
        values.nativeVoiceActive = fxModEnvelopes[0].isActive();
        values.nativeNoiseOsc = renderFxNoiseSource(values.nativeVoiceActive);
        values.ampEnvelope = fxModEnvelopes[0].getNextSample();
        values.filterEnvelope = fxModEnvelopes[1].getNextSample();
        values.modEnvelope = fxModEnvelopes[2].getNextSample();
        values.velocity = fxModVelocity;
        values.macros = parameterSnapshot.macroValues;
        values.learnedMidiControllers=fxLearnedMidiControllers;
        values.modWheel = mpeMember ? managerPerformance.modWheel : notePerformance.modWheel;
        values.aftertouch = mpeMember
            ? std::min(1.0f, notePerformance.aftertouch + managerPerformance.aftertouch)
            : notePerformance.aftertouch;
        values.nativeChannelPressure=values.aftertouch;
        values.nativePolyPressure=fxModPolyPressure;
        values.nativeNoteOn=fxNoteOnValues;
        values.nativeSlide=mpeMember
            ? managerPerformance.combinedSlide(noteSlide) : noteSlide;
        values.nativeReleaseVelocity=fxModReleaseVelocity;
        values.nativePitchBend=std::clamp(notePitch + managerPitchOffset, .0001f, .9999f);
        values.nativeNoteNumber=static_cast<float>(std::clamp(fxModNote, 0, 127))/127.0f;
        const std::array<float*, 4> outputs{&values.lfo1, &values.lfo2, &values.lfo3, &values.lfo4};
        for (size_t i = 0; i < fxNoteLfos.size(); ++i) {
            const auto& settings = parameterSnapshot.modulation.lfos[i];
            float value = 0.0f;
            const auto* bank = blockLfoCurveBanks[i];
            const bool nodeCurve = (fxNodeLfoMask & (uint32_t{1} << i)) != 0;
            if (nodeCurve) {
                const auto& lfo = settings.triggerMode == 0 ? globalLfos[i] : fxNoteLfos[i];
                value = fxNodeLfos[i].sample(lfo.nodePhaseAtOffset(sample, settings.triggerMode == 2,
                    fxNodeLfos[i].history ? fxNodeLfos[i].loopStart : bank != nullptr ? bank->curveLoopStart(i) : -1.f));
            } else if (settings.triggerMode == 0) {
                if (globalLfoBlockPointers[i] != nullptr) value = globalLfoBlockPointers[i][sample];
            } else {
                const auto* curve = blockLfoCurveBanks[i];
                const auto* points = curve != nullptr && curve->frames.size() > i && curve->frames[i].size() >= 2
                    ? curve->frames[i].data() : nullptr;
                value = points != nullptr
                    ? fxNoteLfos[i].renderCurveSampleAtOffset(points, curve->frames[i].size(), sample, settings.triggerMode == 2, curve->curveEndpoint(i), curve->curveLoopStart(i))
                    : fxNoteLfos[i].renderSampleAtOffset(settings.shape, sample, settings.triggerMode == 2);
            }
            const bool curved = nodeCurve || (bank != nullptr && bank->frames.size() > i && bank->frames[i].size() >= 2);
            values.nativeLfos[i]=curved ? value : .5f*(value+1.0f);
            values.lfoDepth[i]=settings.depth;
            values.fadLfos[i]=value;
            if (curved) values.unipolarLfoMask |= uint32_t{1} << i;
            if(i<outputs.size())*outputs[i] = value * settings.depth;
        }
        modulation::applyNativeLfoBuses(parameterSnapshot.modulation, values, activeCurveBanks,
            nativeCurveRuntimes, blockWavetableGeneration,
            [&](size_t i, const modulation::NativeLfoCurveRuntime& curve) noexcept {
                const auto mode = parameterSnapshot.modulation.lfos[i].triggerMode;
                return (mode == 0 ? globalLfos[i] : fxNoteLfos[i]).renderPointCurveAtOffset(curve, sample, mode == 2);
            });
        if (fxNodeLfoMask != 0) updateFxNodeLfos(values.nativeLfos);
        if (hasNativeSustainRoutes) {
            float sustain=baseSustainPosition;
            for (const auto& route:parameterSnapshot.modulation.slots)
                if (route.nativeTarget==38) sustain+=modulation::ModMatrix::resolveRouteSourceValue(route,values)*route.amount;
            sustain=std::clamp(sustain, 0.0f, 1.0f);
            fxModEnvelopes[0].setModulatedSustain(sustain*sustain);
        }
        if (hasNativeDecayRoutes) {
            float offset=0.0f;
            for (const auto& route : parameterSnapshot.modulation.slots)
                if (route.nativeTarget==37)
                    offset+=modulation::ModMatrix::resolveRouteSourceValue(route,values)*route.amount;
            fxModEnvelopes[0].setModulatedSerumDecay(baseDecayPosition,offset);
        }
        if (hasNativeReleaseRoutes) {
            float offset=0.0f;
            for (const auto& route : parameterSnapshot.modulation.slots)
                if (route.nativeTarget==39)
                    offset+=modulation::ModMatrix::resolveRouteSourceValue(route,values)*route.amount;
            fxModEnvelopes[0].setModulatedSerumRelease(baseReleasePosition,offset);
        }
    }
}

void RenderEngine::renderRateModulatedLfos(int startSample,int numSamples) noexcept
{
    const fad::engine::detail::ScopedValue<midi::Source> sourceScope(activeMidiSource, fxModSource);
    const auto& noise = parameterSnapshot.noise;
    fxNoisePlayer.configure(blockNoiseSample, lastSampleRate,
        NoiseSamplePlayer::playbackRate(noise.pitch, noise.fine, noise.keytrack, fxModNote), noise.oneShot, blockPublishedOwnership, noise.sampleSlot == 0);
    modulatedGlobalLfos=globalLfos;
    globalLfoBlockPointers.fill(nullptr);
    fxModFrameStart=startSample;
    fxModFrameCount=std::min(numSamples, static_cast<int>(fxModSourceFrames.size()));
    for(size_t i=0;i<globalLfos.size();++i)
        if(parameterSnapshot.modulation.lfos[i].triggerMode==0)globalLfoBlockPointers[i]=globalLfoBlockStorage[i].data();
    const auto sustainBase=std::sqrt(utils::sanitizeUnitInterval(parameterSnapshot.ampEnvelope.sustainLevel));
    const auto decayBase=hasNativeDecayRoutes
        ? modulation::HoldEnvelope::serumDecayPosition(parameterSnapshot.ampEnvelope.decaySeconds) : 0.0f;
    const auto releaseBase=hasNativeReleaseRoutes
        ? modulation::HoldEnvelope::serumReleasePosition(parameterSnapshot.ampEnvelope.releaseSeconds) : 0.0f;
    prepareFxNodeLfos();
    auto activeCurveBanks = blockLfoCurveBanks;
    for (size_t i = 0; i < activeCurveBanks.size(); ++i)
        if ((fxNodeLfoMask & (uint32_t{1} << i)) != 0) activeCurveBanks[i] = nullptr;
    const auto fxChannel = std::clamp(fxModChannel, 1, 16);
    const auto fxIndex = static_cast<size_t>(fxChannel - 1);
    const bool mpeMember = isMpeMemberChannel(fxChannel);
    const bool frozen = mpeMember && fxModControlsFrozen;
    const auto& notePerformance = frozen ? fxModReleasedPerformance : midiState().performanceStates[fxIndex];
    const auto managerChannel = std::max(1, mpeManagerChannelFor(fxChannel));
    const auto managerIndex = static_cast<size_t>(managerChannel - 1);
    const auto& managerPerformance = midiState().performanceStates[managerIndex];
    const auto notePitch = (frozen ? fxModReleasedPitch : .5f+.5f*midiState().pitchWheelBipolar[fxIndex])
        + (fxMidi2Active ? fxMidi2Pitch-.5f : 0.0f);
    const auto noteSlide = fxMidi2SlideActive ? fxMidi2Slide
        : fxMidi2Active && !notePerformance.slideReceived ? .5f : notePerformance.slide;
    const auto managerPitchOffset = mpeMember
        ? .5f*midiState().pitchWheelBipolar[managerIndex]
            * effectivePitchBendRange(managerChannel) / std::max(1.0f, frozen ? fxModReleasedBendRange : effectivePitchBendRange(fxChannel))
        : 0.0f;
    for(int sample=0;sample<fxModFrameCount;++sample) {
        auto& values=fxModSourceFrames[static_cast<size_t>(sample)];values={};
        values.nativeVoiceActive = fxModEnvelopes[0].isActive();
        values.nativeNoiseOsc = renderFxNoiseSource(values.nativeVoiceActive);
        values.ampEnvelope=fxModEnvelopes[0].getNextSample();
        values.filterEnvelope=fxModEnvelopes[1].getNextSample();values.modEnvelope=fxModEnvelopes[2].getNextSample();
        values.macros=parameterSnapshot.macroValues; values.learnedMidiControllers=fxLearnedMidiControllers;values.velocity=fxModVelocity;
        values.modWheel=mpeMember ? managerPerformance.modWheel : notePerformance.modWheel;
        values.aftertouch=mpeMember
            ? std::min(1.0f, notePerformance.aftertouch + managerPerformance.aftertouch)
            : notePerformance.aftertouch;
        values.nativeChannelPressure=values.aftertouch;values.nativePolyPressure=fxModPolyPressure;
        values.nativeNoteOn=fxNoteOnValues;
        values.nativeSlide=mpeMember
            ? managerPerformance.combinedSlide(noteSlide) : noteSlide;
        values.nativeReleaseVelocity=fxModReleaseVelocity;
        values.nativeNoteNumber=static_cast<float>(std::clamp(fxModNote, 0, 127))/127.0f;
        values.nativePitchBend=std::clamp(notePitch + managerPitchOffset, .0001f, .9999f);
        const std::array<float*,4> legacy{&values.lfo1,&values.lfo2,&values.lfo3,&values.lfo4};
        for(size_t i=0;i<globalLfos.size();++i) {
            const auto& settings=parameterSnapshot.modulation.lfos[i];
            const auto* bank=blockLfoCurveBanks[i];
            const bool nodeCurve = (fxNodeLfoMask & (uint32_t{1} << i)) != 0;
            const bool sampledCurve = bank != nullptr && bank->frames.size() > i && bank->frames[i].size() >= 2;
            const bool curved = nodeCurve || sampledCurve;
            auto& lfo=settings.triggerMode==0?modulatedGlobalLfos[i]:fxNoteLfos[i];
            const auto raw = nodeCurve
                ? fxNodeLfos[i].sample(lfo.nodePhaseAtOffset(0, settings.triggerMode == 2,
                    bank != nullptr ? bank->curveLoopStart(i) : -1.f))
                : sampledCurve ? lfo.renderCurveSampleAtOffset(bank->frames[i].data(),bank->frames[i].size(),0,settings.triggerMode==2,bank->curveEndpoint(i), bank->curveLoopStart(i))
                               : lfo.renderSampleAtOffset(settings.shape,0,settings.triggerMode==2);
            if(settings.triggerMode==0)globalLfoBlockStorage[i][static_cast<size_t>(sample)]=raw;
            if(i<legacy.size())*legacy[i]=raw*settings.depth;
            values.nativeLfos[i]=curved?raw:.5f*(raw+1.0f);values.lfoDepth[i]=settings.depth;
            values.fadLfos[i]=raw;
            if (curved) values.unipolarLfoMask |= uint32_t{1} << i;
        }
        modulation::applyNativeLfoBuses(parameterSnapshot.modulation, values, activeCurveBanks,
            nativeCurveRuntimes, blockWavetableGeneration,
            [&](size_t i, const modulation::NativeLfoCurveRuntime& curve) noexcept {
                const auto mode = parameterSnapshot.modulation.lfos[i].triggerMode;
                return (mode == 0 ? modulatedGlobalLfos[i] : fxNoteLfos[i]).renderPointCurveAtOffset(curve, 0, mode == 2);
            });
        if (fxNodeLfoMask != 0) updateFxNodeLfos(values.nativeLfos);
        if(hasNativeSustainRoutes) {
            float offset=0;
            for(const auto& route:parameterSnapshot.modulation.slots)
                if(route.nativeTarget==38)offset+=modulation::ModMatrix::resolveRouteSourceValue(route,values)*route.amount;
            const auto raw=std::clamp(sustainBase+offset, 0.0f, 1.0f);fxModEnvelopes[0].setModulatedSustain(raw*raw);
        }
        if(hasNativeDecayRoutes) {
            float offset=0.0f;
            for(const auto& route:parameterSnapshot.modulation.slots)
                if(route.nativeTarget==37)
                    offset+=modulation::ModMatrix::resolveRouteSourceValue(route,values)*route.amount;
            fxModEnvelopes[0].setModulatedSerumDecay(decayBase,offset);
        }
        if(hasNativeReleaseRoutes) {
            float offset=0.0f;
            for(const auto& route:parameterSnapshot.modulation.slots)
                if(route.nativeTarget==39)
                    offset+=modulation::ModMatrix::resolveRouteSourceValue(route,values)*route.amount;
            fxModEnvelopes[0].setModulatedSerumRelease(releaseBase,offset);
        }
        for(size_t i=0;i<globalLfos.size();++i) {
            if((nativeRateRouteMask&(uint32_t{1}<<i))!=0) {
                const auto rate=modulation::modulatedNativeLfoRate(parameterSnapshot.modulation,i,values,tempoBpm);
                modulatedGlobalLfos[i].setRate(rate);fxNoteLfos[i].setRate(rate);
            }
            modulatedGlobalLfos[i].advance(1);fxNoteLfos[i].advance(1);
        }
    }
}

void RenderEngine::deferResetMidiMessage(const midi::ShortMessage& message,
                                        int absoluteSampleOffset, midi::Source source, midi::UmpChannelVoice ump) noexcept
{
    if(static_cast<size_t>(source)>=midi::sourceCount)return;
    if (resetDeferredMidiOverflowed)
        return;

    const auto clampedOrderKey = std::max(0, absoluteSampleOffset);
    if (resetDeferredMidiCount >= resetDeferredMidi.size())
    {
        assert(false);
        lastResetDeferredMidiOverflowCount.store(static_cast<int>(resetDeferredMidiCount),
                                                 std::memory_order_release);
        lastResetDeferredMidiOverflowCapacity.store(static_cast<int>(resetDeferredMidi.size()),
                                                    std::memory_order_release);
        lastResetDeferredMidiOverflowOrderKey.store(clampedOrderKey, std::memory_order_release);
        pendingResetDeferredMidiOverflowLogCount.fetch_add(1, std::memory_order_acq_rel);
        std::array<bool, midi::sourceCount> owners{};
        owners[static_cast<size_t>(source)] = true;
        for (size_t i = 0; i < resetDeferredMidiCount; ++i)
            owners[static_cast<size_t>(resetDeferredMidi[i].source)] = true;
        resetDeferredMidiOverflowed = true;
        resetDeferredMidiCount = 0;
        resetDeferredNoteCount = 0;
        for (size_t owner = 0; owner < owners.size(); ++owner)
            if (owners[owner])
                for (int channel = 1; channel <= 16 && resetDeferredMidiCount < resetDeferredMidi.size(); ++channel)
                    resetDeferredMidi[resetDeferredMidiCount++] =
                        {midi::ShortMessage::allNotesOff(channel), clampedOrderKey, clampedOrderKey,
                         static_cast<midi::Source>(owner), {}};
        return;
    }

    auto insertIndex = resetDeferredMidiCount;
    while (insertIndex > 0 && resetDeferredMidi[insertIndex - 1].orderKey > clampedOrderKey)
    {
        resetDeferredMidi[insertIndex] = resetDeferredMidi[insertIndex - 1];
        --insertIndex;
    }

    resetDeferredMidi[insertIndex] = {message, clampedOrderKey, clampedOrderKey, source, ump};
    ++resetDeferredMidiCount;
    if (message.isNoteOn())
    {
        for (size_t index = 0; index < resetDeferredNoteCount; ++index)
        {
            auto& note = resetDeferredNotes[index];
            if (note.source == source && note.channel == message.getChannel() && note.note == message.getNoteNumber())
            {
                ++note.count;
                return;
            }
        }
        if (resetDeferredNoteCount < resetDeferredNotes.size())
            resetDeferredNotes[resetDeferredNoteCount++] = {source, message.getChannel(), message.getNoteNumber(), 1};
    }
}

bool RenderEngine::hasDeferredResetNote(const midi::ShortMessage& message, midi::Source source) const noexcept
{
    if (resetDeferredNoteCount == 0 || !message.isNoteOnOrOff()) return false;
    return std::any_of(resetDeferredNotes.begin(), resetDeferredNotes.begin() + static_cast<ptrdiff_t>(resetDeferredNoteCount), [&](const auto& note) {
        return note.count > 0 && note.source == source && note.channel == message.getChannel()
            && note.note == message.getNoteNumber();
    });
}

void RenderEngine::releaseDeferredResetNote(const midi::ShortMessage& message, midi::Source source) noexcept
{
    if (!message.isNoteOff()) return;
    for (size_t index = 0; index < resetDeferredNoteCount; ++index)
    {
        auto& note = resetDeferredNotes[index];
        if (note.count > 0 && note.source == source && note.channel == message.getChannel()
            && note.note == message.getNoteNumber())
        {
            if (--note.count == 0) note = resetDeferredNotes[--resetDeferredNoteCount];
            return;
        }
    }
}

void RenderEngine::removeDeferredResetNoteOns(int midiChannel, midi::Source source, size_t replayedCount) noexcept
{
    const auto targetChannel = std::clamp(midiChannel, 0, 16);
    for (size_t index = 0; index < resetDeferredNoteCount;)
        if (resetDeferredNotes[index].source == source
            && (targetChannel == 0 || resetDeferredNotes[index].channel == targetChannel))
        {
            resetDeferredNotes[index] = resetDeferredNotes[--resetDeferredNoteCount];
        }
        else ++index;
    size_t writeIndex = std::min(replayedCount, resetDeferredMidiCount);
    for (size_t readIndex = writeIndex; readIndex < resetDeferredMidiCount; ++readIndex)
    {
        const auto& deferred = resetDeferredMidi[readIndex];
        if (deferred.source != source || ! deferred.message.isNoteOn()
            || (targetChannel > 0 && deferred.message.getChannel() != targetChannel))
            resetDeferredMidi[writeIndex++] = deferred;
    }
    resetDeferredMidiCount = writeIndex;
}

uint64_t RenderEngine::beginOscillatorUiCapture() noexcept
{
    const auto owner = oscillatorUiNextOwner.fetch_add(1, std::memory_order_relaxed) + 1;
    oscillatorUiOwner.store(owner, std::memory_order_release);
    oscillatorUiRequest.fetch_add(1, std::memory_order_release);
    return owner;
}

void RenderEngine::endOscillatorUiCapture(uint64_t owner) noexcept
{
    if (owner != 0) oscillatorUiOwner.compare_exchange_strong(owner, 0, std::memory_order_acq_rel);
}

void RenderEngine::requestOscillatorUiFrame(uint64_t owner) noexcept
{
    if (owner != 0 && oscillatorUiOwner.load(std::memory_order_acquire) == owner)
        oscillatorUiRequest.fetch_add(1, std::memory_order_release);
}

bool RenderEngine::tryReadOscillatorUiFrame(uint64_t owner, OscillatorUiFrame& result) const noexcept
{
    if (owner == 0 || !realtimeProcessing.load(std::memory_order_relaxed)
        || oscillatorUiOwner.load(std::memory_order_acquire) != owner) return false;
    const auto context = oscillatorUiContext.load(std::memory_order_acquire);
    const auto index = oscillatorUiReadIndex.load(std::memory_order_acquire);
    auto& state = oscillatorUiSlotStates[static_cast<size_t>(index)];
    auto readers = state.load(std::memory_order_acquire);
    if (readers < 0 || !state.compare_exchange_strong(readers, readers + 1, std::memory_order_acq_rel)) return false;
    const bool current = oscillatorUiReadIndex.load(std::memory_order_acquire) == index
        && oscillatorUiOwner.load(std::memory_order_acquire) == owner
        && oscillatorUiFrames[static_cast<size_t>(index)].owner == owner
        && oscillatorUiFrames[static_cast<size_t>(index)].context == context;
    if (current) result = oscillatorUiFrames[static_cast<size_t>(index)];
    state.fetch_sub(1, std::memory_order_release);
    return current && oscillatorUiOwner.load(std::memory_order_acquire) == owner
        && oscillatorUiContext.load(std::memory_order_acquire) == context;
}

VoiceCore* RenderEngine::prepareOscillatorUiCapture() noexcept
{
    const auto owner = oscillatorUiOwner.load(std::memory_order_acquire);
    if (owner == 0 || !realtimeProcessing.load(std::memory_order_relaxed))
    {
        oscillatorUiCollecting = false; oscillatorUiCursor = 0; oscillatorUiPublishedActive = false;
        return nullptr;
    }
    const auto request = oscillatorUiRequest.load(std::memory_order_acquire);
    const bool ownerChanged = oscillatorUiWorking.owner != owner;
    if (!ownerChanged && !oscillatorUiCollecting && request == oscillatorUiServicedRequest) return nullptr;
    VoiceCore* selected = nullptr;
    for (auto* voice : voiceCache)
        if (voice != nullptr && voice->isVoiceActive()
            && (selected == nullptr || voice->getRealtimeStartOrder() > selected->getRealtimeStartOrder())) selected = voice;
    if (selected == nullptr)
    {
        oscillatorUiCollecting = false; oscillatorUiCursor = 0;
        oscillatorUiWorking.owner = owner; oscillatorUiWorking.active = false;
        oscillatorUiWorking.context = oscillatorUiContext.load(std::memory_order_relaxed);
        if (ownerChanged || oscillatorUiPublishedActive) publishOscillatorUiFrame();
        oscillatorUiServicedRequest = request;
        return nullptr;
    }
    const auto generation = blockPublishedOwnership ? blockPublishedOwnership->generation : 0;
    bool resourceChanged = oscillatorUiWorking.tableGeneration != generation;
    for (size_t i=0;i<2;++i)
        resourceChanged = resourceChanged || oscillatorUiWorking.slots[i] != parameterSnapshot.oscillators[i].wavetableSlot
            || oscillatorUiWorking.modes[i] != static_cast<int>(parameterSnapshot.oscillators[i].mode);
    if (!oscillatorUiCollecting || ownerChanged || resourceChanged
        || oscillatorUiWorking.voiceStartOrder != selected->getRealtimeStartOrder()
        || oscillatorUiWorking.note != selected->getRealtimeMidiNote()
        || oscillatorUiWorking.channel != selected->getRealtimeMidiChannel()
        || oscillatorUiWorking.source != selected->getMidiSource())
    {
        oscillatorUiWorking = {};
        oscillatorUiWorking.owner = owner;
        oscillatorUiWorking.context = oscillatorUiContext.load(std::memory_order_relaxed);
        oscillatorUiWorking.voiceStartOrder = selected->getRealtimeStartOrder();
        oscillatorUiWorking.note = selected->getRealtimeMidiNote();
        oscillatorUiWorking.channel = selected->getRealtimeMidiChannel();
        oscillatorUiWorking.source = selected->getMidiSource();
        oscillatorUiWorking.sampleRate = lastSampleRate;
        oscillatorUiWorking.tableGeneration = generation;
        for (size_t i=0;i<2;++i) {
            oscillatorUiWorking.slots[i] = parameterSnapshot.oscillators[i].wavetableSlot;
            oscillatorUiWorking.modes[i] = static_cast<int>(parameterSnapshot.oscillators[i].mode);
        }
        oscillatorUiWorking.active = true;
        oscillatorUiCursor = 0; oscillatorUiCollecting = true;
    }
    return selected;
}

bool RenderEngine::publishOscillatorUiFrame() noexcept
{
    if (oscillatorUiWorking.owner == 0
        || oscillatorUiOwner.load(std::memory_order_acquire) != oscillatorUiWorking.owner) return false;
    const auto index = tryClaimSnapshotWriteSlot(oscillatorUiSlotStates, oscillatorUiReadIndex);
    if (index < 0) return false;
    oscillatorUiWorking.sequence = ++oscillatorUiSequence;
    oscillatorUiFrames[static_cast<size_t>(index)] = oscillatorUiWorking;
    oscillatorUiReadIndex.store(index, std::memory_order_release);
    oscillatorUiSlotStates[static_cast<size_t>(index)].store(0, std::memory_order_release);
    oscillatorUiPublishedActive = oscillatorUiWorking.active;
    return true;
}

void RenderEngine::finishOscillatorUiCapture(VoiceCore* voice, int samples) noexcept
{
    if (voice == nullptr) return;
    oscillatorUiCursor += std::min(samples, OscillatorUiFrame::sampleCount - oscillatorUiCursor);
    if (oscillatorUiCursor < OscillatorUiFrame::sampleCount) return;
    oscillatorUiWorking.active = voice->isVoiceActive() && oscillatorUiWorking.finite;
    publishOscillatorUiFrame();
    oscillatorUiServicedRequest = oscillatorUiRequest.load(std::memory_order_acquire);
    oscillatorUiCollecting = false; oscillatorUiCursor = 0;
}

void RenderEngine::invalidateOscillatorUiCapture() noexcept
{
    oscillatorUiCollecting = false; oscillatorUiCursor = 0; oscillatorUiServicedRequest = 0;
    const auto owner = oscillatorUiOwner.load(std::memory_order_acquire);
    if (owner == 0) return;
    const auto context = oscillatorUiContext.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (oscillatorUiPublishedActive)
    {
        oscillatorUiWorking.owner = owner; oscillatorUiWorking.active = false;
        oscillatorUiWorking.context = context;
        publishOscillatorUiFrame();
    }
}

void RenderEngine::renderChunk(fad::engine::AudioBufferView audioBuffer, int startSample, int numSamples)
{
    if (numSamples <= 0)
        return;

    // Pure snapshot preparation stays valid until the next accepted snapshot.
    // This avoids repeating route classification and tuning/pan math for tiny
    // host/MIDI segments; voices retain their independent sample progression.
    const auto renderVoices = [&](int offset, int count)
    {
        auto* captured = prepareOscillatorUiCapture();
        for (auto* voice : voiceCache)
        {
            if (voice == nullptr || !voice->isVoiceActive()) continue;
            if (!voiceRenderContext) voiceRenderContext.emplace(VoiceCore::prepareRenderContext(parameterSnapshot));
            if (voice == captured) voice->setOscillatorUiCapture(&oscillatorUiWorking, oscillatorUiCursor);
            voice->renderNextBlockWithContext(audioBuffer, offset, count, &*voiceRenderContext);
            if (voice == captured) voice->setOscillatorUiCapture(nullptr);
        }
        finishOscillatorUiCapture(captured, count);
    };

    const auto lfoBlockCapacity = globalLfoBlockStorage.empty()
        ? 0
        : static_cast<int>(globalLfoBlockStorage.front().size());
    if (lfoBlockCapacity <= 0)
    {
        renderVoices(startSample, numSamples);
        for (auto& lfo : globalLfos)
            lfo.advance(numSamples);
        return;
    }

    for (int renderedSamples = 0; renderedSamples < numSamples;)
    {
        const auto chunkSamples = std::min(lfoBlockCapacity, numSamples - renderedSamples);
        if(nativeRateRouteMask!=0)renderRateModulatedLfos(startSample+renderedSamples,chunkSamples);
        else {
            renderGlobalLfoBlock(chunkSamples);
            renderNativeFxModulation(startSample+renderedSamples,chunkSamples);
        }

        renderVoices(startSample + renderedSamples, chunkSamples);

        if(nativeRateRouteMask!=0)globalLfos=modulatedGlobalLfos;
        else {
            for(auto& lfo:globalLfos)lfo.advance(chunkSamples);
            for(auto& lfo:fxNoteLfos)lfo.advance(chunkSamples);
        }
        globalLfoBlockPointers.fill(nullptr);
        renderedSamples += chunkSamples;
    }
}

void RenderEngine::advanceBypassRuntimeState(int numSamples)
{
    if (oscillatorUiCollecting || oscillatorUiPublishedActive) invalidateOscillatorUiCapture();
    if (numSamples <= 0)
        return;

    if(nativeRateRouteMask!=0 && !fxModSourceFrames.empty()) {
        for(int offset=0;offset<numSamples;) {
            const auto count=std::min(static_cast<int>(fxModSourceFrames.size()), numSamples-offset);
            renderRateModulatedLfos(0,count);globalLfos=modulatedGlobalLfos;offset+=count;
        }
        globalLfoBlockPointers.fill(nullptr);fxModFrameStart=-1;fxModFrameCount=0;
    } else {
        for(auto& lfo:globalLfos)lfo.advance(numSamples);
        for(auto& lfo:fxNoteLfos)lfo.advance(numSamples);
        for(int sample=0;sample<numSamples;++sample)
            for(auto& envelope:fxModEnvelopes)(void) (envelope.getNextSample());
    }

    if (! hasHeldOrSustainedNotes())
        clearBypassInactiveAudioTails();

    allSoundOffSamplesRemaining = std::max(0, allSoundOffSamplesRemaining - numSamples);
    allSoundOffOutputGain.skip(numSamples);

    outputGainSmoother.skip(numSamples);
}

void RenderEngine::beginAllSoundOffFade() noexcept
{
    allSoundOffSamplesRemaining = std::max(8, static_cast<int>(lastSampleRate * 0.002));
    allSoundOffOutputGain.setTargetValue(0.0f);
}

void RenderEngine::captureMidiCiChannelStatesFromAudioThread(midi::Source source, std::array<midici::ChannelState, 16>& result) const noexcept
{
    if (static_cast<size_t>(source) < midiSourceStates.size()) result = midiSourceStates[static_cast<size_t>(source)].ciReportValues;
}

void RenderEngine::setMidiCiMpeProfilesFromAudioThread(midi::Source source, const std::array<uint8_t, 16>& counts) noexcept
{
    if (static_cast<size_t>(source) >= midi::sourceCount || source == midi::Source::touch) return;
    auto& state = midiSourceStates[static_cast<size_t>(source)];
    if (state.ciProfileCounts == counts) return;
    const auto previousSource = activeMidiSource; activeMidiSource = source;
    std::array<int,16> previousManagers{}; std::array<bool,16> affected{};
    for (int channel=1;channel<=16;++channel)
        previousManagers[static_cast<size_t>(channel-1)] = isMpeManagerChannel(channel) ? channel : mpeManagerChannelFor(channel);
    const auto previousCounts = state.ciProfileCounts;
    const auto previousActive = state.ciProfileActive;
    state.ciProfileCounts = counts;
    state.ciProfileActive = std::any_of(counts.begin(), counts.end(), [](auto count) { return count >= 2; });
    for (unsigned channel=0;channel<16;++channel)
        affected[channel] = previousManagers[channel] != (isMpeManagerChannel(static_cast<int>(channel+1))
            ? static_cast<int>(channel+1) : mpeManagerChannelFor(static_cast<int>(channel+1)));
    for (unsigned manager=0;manager<16;++manager)
        if (previousCounts[manager] != counts[manager])
            for (unsigned channel=manager;channel<std::min(16u, manager+std::max(previousCounts[manager], counts[manager]));++channel) affected[channel]=true;
    // Stop/reset only changed clusters. An independent active cluster keeps
    // its sounding notes, negotiated bend range and performance controls.
    state.ciProfileCounts = previousCounts; state.ciProfileActive = previousActive;
    for (unsigned channel=0;channel<16;++channel)
        if (affected[channel])
        {
            removeDeferredResetNoteOns(static_cast<int>(channel+1),source);
            if (midi2SourcesUsed[static_cast<size_t>(source)])
                std::fill_n(midi2NoteStates.begin()+static_cast<ptrdiff_t>((static_cast<size_t>(source)*16+channel)*128),128,Midi2NoteState{});
            dispatchMidiMessage(midi::ShortMessage::allSoundOff(static_cast<int>(channel+1)),source);
            dispatchMidiMessage(midi::ShortMessage::controllerEvent(static_cast<int>(channel+1),121,0),source);
            state.performanceStates[channel]={}; state.pitchBendRangeOverrides[channel]=-1.0f; state.ciReportValues[channel]={};
        }
    state.ciProfileCounts = counts;
    state.ciProfileActive = std::any_of(counts.begin(), counts.end(), [](auto count) { return count >= 2; });
    for (unsigned manager=0;manager<16;++manager) if (previousCounts[manager] != counts[manager])
    { state.ciProfileBendRanges[manager]=48.0f; state.ciProfileBendCoarse[manager]=48; }
    refreshMpePedals(); refreshMpeManagerControls();
    activeMidiSource = previousSource;
}

void RenderEngine::silenceMidiSourceFromAudioThread(midi::Source source)
{
    const auto sourceIndex=static_cast<size_t>(source);
    if(sourceIndex>=midi::sourceCount)return;
    if(midi2SourcesUsed[sourceIndex])
    {
        std::fill_n(midi2NoteStates.begin()+static_cast<ptrdiff_t>(sourceIndex*16*128),16*128,Midi2NoteState{});
        midi2SourcesUsed[sourceIndex]=false;
    }
    if(midiSourceResetter)midiSourceResetter(midiObserverOwner,source);
    for (int channel = 1; channel <= 16; ++channel)
    {
        removeDeferredResetNoteOns(channel, source);
        dispatchMidiMessage(midi::ShortMessage::allSoundOff(channel), source);
        if (source == midi::Source::touch || source >= midi::Source::standalone0)
            dispatchMidiMessage(midi::ShortMessage::controllerEvent(channel, 121, 0), source);
    }
}

void RenderEngine::reportMidiInputOverflow() noexcept
{
    pendingInputMidiOverflowLogCount.fetch_add(1, std::memory_order_relaxed);
    // Dispatch the accepted prefix first. A final panic must follow every
    // retained note-on, otherwise a discarded note-off could hold it forever.
    inputMidiOverflowPanicPending = true;
}

void RenderEngine::silenceAllChannelsFromAudioThread()
{
    for (size_t sourceIndex=0;sourceIndex<midi::sourceCount;++sourceIndex)
        silenceMidiSourceFromAudioThread(static_cast<midi::Source>(sourceIndex));
    beginAllSoundOffFade();
}

void RenderEngine::setLearnedPerNoteController(midi::Source source,int channel,int note,int slot,float value) noexcept
{
    if(slot<0 || slot>=32)return;
    for(auto* voice:voiceCache)
        if(voice && voice->getMidiSource()==source && voice->matchesRealtimeNote(channel,note))
            voice->setLearnedMidiController(slot,value);
    if(fxModSource==source && fxModChannel==channel && fxModNote==note && !fxModControlsFrozen && !fxMidi2Detached)
        fxLearnedMidiControllers[static_cast<size_t>(slot)]=value;
}

void RenderEngine::dispatchUmpMessage(const midi::UmpChannelVoice& packet, midi::Source source)
{
    if (!packet.valid() || static_cast<size_t>(source)>=midi::sourceCount) return;
    midi2SourcesUsed[static_cast<size_t>(source)]=true;
    const fad::engine::detail::ScopedValue<midi::Source> sourceScope(activeMidiSource,source);
    const fad::engine::detail::ScopedValue<bool> umpScope(dispatchingUmp,true);
    const auto channel=packet.channel(), index=packet.byte2();
    const auto channelIndex=static_cast<size_t>(channel-1);
    const auto proxy=midi::umpProxyShortMessage(packet);
    if (midiObserver != nullptr)
        midiObserver(midiObserverOwner,proxy,packet,source,isMpeMemberChannel(channel));
    auto& note=midi2NoteStates[(static_cast<size_t>(source)*16+channelIndex)*128+static_cast<size_t>(index)];
    const auto forNote=[&](auto&& fn) {
        for(auto* voice:voiceCache) if(ownsVoice(voice) && voice->matchesRealtimeNote(channel,index)) fn(*voice);
    };
    const bool ownsFx=fxModSource==source && fxModChannel==channel && fxModNote==index;
    switch(packet.status())
    {
        case 8: case 9:
        {
            const fad::engine::detail::ScopedValue<float> velocityScope(umpVelocityOverride,packet.velocity());
            dispatchMidiMessage(proxy,source);
            if(packet.noteOn())
            {
                if(parameterSnapshot.global.voiceMode==parameters::VoiceMode::mono && monoHeldNoteCount>0)
                    monoHeldNotes[monoHeldNoteCount-1].ump=packet;
                VoiceCore* latest=nullptr;
                forNote([&](VoiceCore& v) { if(v.isKeyDown() && (!latest || v.getRealtimeStartOrder()>latest->getRealtimeStartOrder())) latest=&v; });
                if(latest)
                {
                    const float pitch=packet.byte3()==3 ? static_cast<float>(packet.data()&65535)/512.0f : note.tuning;
                    latest->beginMidi2Note(midiState().perNoteBendRanges[channelIndex],pitch);
                    latest->syncMidi2ChannelControls();
                    latest->setMidi2ChannelPitch(midiState().pitchWheelBipolar[channelIndex]);
                    latest->setMidi2PolyPressure(note.pressure);
                    if(note.slideReceived) latest->setMidi2Slide(note.slide);
                    latest->setMidi2NotePitch((note.pitch-.5f)*2.0f);
                    fxMidi2Active=true;fxMidi2Detached=false;fxMidi2SlideActive=note.slideReceived;
                    fxModPolyPressure=note.pressure;fxMidi2Slide=note.slide;fxMidi2Pitch=note.pitch;
                    if(midiNoteInitialiser)midiNoteInitialiser(midiObserverOwner,source,channel,index);
                }
            }
            return;
        }
        case 0:
            // Registered per-note 3 is Q7.25 pitch; 74 is brightness.
            if(packet.byte3()==3) { note.tuning=static_cast<float>(static_cast<double>(packet.data())/33554432.0); forNote([&](VoiceCore& v){v.setMidi2Tuning(note.tuning);}); }
            else if(packet.byte3()==74) { note.slide=packet.unit(); note.slideReceived=true; forNote([&](VoiceCore& v){v.setMidi2Slide(note.slide);});
                if(ownsFx && !fxMidi2Detached && !fxModControlsFrozen) { fxMidi2Slide=note.slide;fxMidi2SlideActive=true; } }
            return;
        case 6:
            note.pitch=.5f+.5f*packet.bipolar();
            forNote([&](VoiceCore& v){v.setMidi2NotePitch(packet.bipolar());});
            if(ownsFx && !fxMidi2Detached && !fxModControlsFrozen)fxMidi2Pitch=note.pitch;
            return;
        case 10:
            note.pressure=packet.unit();
            forNote([&](VoiceCore& v){v.setMidi2PolyPressure(note.pressure);});
            if(ownsFx && !fxModControlsFrozen && !fxMidi2Detached) fxModPolyPressure=note.pressure;
            return;
        case 15:
            if(packet.byte3()&2) { forNote([](VoiceCore& v){v.detachMidi2Note();});if(ownsFx)fxMidi2Detached=true; }
            if(packet.byte3()&1) { note={}; forNote([](VoiceCore& v){v.resetMidi2NoteControllers();});
                if(ownsFx && !fxMidi2Detached) { fxModPolyPressure=0;fxMidi2Slide=fxMidi2Pitch=.5f;fxMidi2SlideActive=false;fxLearnedMidiControllers.fill(0); } }
            return;
        case 13:
            midiState().ciReportValues[channelIndex].pressure=packet.data();
            midiState().performanceStates[channelIndex].aftertouch=packet.unit();
            for(auto* voice:voiceCache) if(ownsVoice(voice) && voice->isPlayingChannel(channel)) voice->setMidi2ChannelPressure(packet.unit());
            return;
        case 14:
            midiState().ciReportValues[channelIndex].pitch=packet.data();
            midiState().lastPitchWheelValues[channelIndex]=static_cast<int>(packet.data()>>18);
            midiState().pitchWheelBipolar[channelIndex]=packet.bipolar();
            for(auto* voice:voiceCache) if(ownsVoice(voice) && voice->isPlayingChannel(channel)) voice->setMidi2ChannelPitch(packet.bipolar());
            if(isMpeManagerChannel(channel)) refreshMpeManagerControls();
            return;
        case 11:
        {
            // These CCs are replaced by bank/program and RC/AC messages in MIDI 2.0.
            if(index==0 || index==32 || index==6 || index==38 || (index>=98 && index<=101)) return;
            dispatchMidiMessage(proxy,source);
            if(isMpeMemberChannel(channel) && index!=74) return;
            auto& performance=midiState().performanceStates[channelIndex];
            if(index==1) performance.modWheel=packet.unit();
            if(index==7) performance.channelVolume=packet.unit();
            if(index==11) performance.expression=packet.unit();
            if(index==74) { performance.slide=packet.unit(); performance.slideReceived=true; }
            if(index==1 || index==7 || index==11 || index==74 || index==121)
                for(auto* voice:voiceCache) if(ownsVoice(voice) && voice->isPlayingChannel(channel)) voice->syncMidi2ChannelControls();
            if(isMpeManagerChannel(channel)) refreshMpeManagerControls();
            return;
        }
        case 2:
            if(index==0 && (packet.byte3()&127)==7)
            {
                const auto range=static_cast<float>(static_cast<double>(packet.data())/33554432.0);
                midiState().perNoteBendRanges[channelIndex]=range;
                for(auto* voice:voiceCache)if(ownsVoice(voice) && voice->isPlayingChannel(channel))voice->setMidi2NoteBendRange(range);
            }
            else if(index==0 && (packet.byte3()&127)==6 && (channel==1 || channel==16) && mpeEnabledForCurrentSource())
            { const int members=static_cast<int>(packet.data()>>25); if(members<=15)configureMpeZone(channel,members); }
            else if(index==0 && (packet.byte3()&127)==0)
            {
                // RPN #00/00 uses 7-bit semitones followed by 7-bit cents.
                const auto msb=midiState().rpnMsb[channelIndex], lsb=midiState().rpnLsb[channelIndex];
                midiState().rpnMsb[channelIndex]=midiState().rpnLsb[channelIndex]=0;
                dispatchMidiMessage(midi::ShortMessage::controllerEvent(channel,6,static_cast<int>(packet.data()>>25)),source);
                dispatchMidiMessage(midi::ShortMessage::controllerEvent(channel,38,static_cast<int>((packet.data()>>18)&127)),source);
                midiState().rpnMsb[channelIndex]=msb; midiState().rpnLsb[channelIndex]=lsb;
            }
            return;
        case 12:
            // MIDI bank/program selection is not implemented by this voice
            // engine; preserve the instrument's existing host-program contract.
            dispatchMidiMessage(proxy,source); return;
        default: return; // Undefined registered functions do not acquire invented meanings.
    }
}

void RenderEngine::dispatchInstrumentPitch(int value) noexcept
{
    const auto safe = std::clamp(value, 0, 16383);
    const auto bipolar = static_cast<float>(safe - 8192) / 8192.0f;
    instrumentPitchActive = safe != 8192;
    // A single ordered UI event updates bounded, preallocated controller state.
    // MPE member bends remain independent: the screen wheel controls managers.
    for (size_t source = 0; source < midi::sourceCount; ++source)
    {
        const fad::engine::detail::ScopedValue<midi::Source> owner(activeMidiSource, static_cast<midi::Source>(source));
        for (int channel = 1; channel <= 16; ++channel)
        {
            if (isMpeMemberChannel(channel)) continue;
            const auto index = static_cast<size_t>(channel - 1);
            midiState().lastPitchWheelValues[index] = safe;
            midiState().pitchWheelBipolar[index] = bipolar;
            midiState().ciReportValues[index].pitch = midi::expandMidi1<14>(static_cast<uint32_t>(safe));
        }
    }
    for (auto* voice : voiceCache)
    {
        if (voice == nullptr || !voice->isVoiceActive()) continue;
        const fad::engine::detail::ScopedValue<midi::Source> owner(activeMidiSource, voice->getMidiSource());
        if (isMpeMemberChannel(voice->getRealtimeMidiChannel())) applyMpeManagerControls(voice, false);
        else voice->pitchWheelMoved(safe);
    }
    sessionMetrics.pitchWheelCount.fetch_add(1, std::memory_order_relaxed);
}

void RenderEngine::resetInstrumentPitchFromAudioThread() noexcept
{
    if (instrumentPitchActive) dispatchInstrumentPitch(8192);
}

void RenderEngine::dispatchMidiMessage(const midi::ShortMessage& message, midi::Source source, midi::UmpChannelVoice ump)
{
    if (source == midi::Source::instrumentPitch)
    {
        if (!ump.present() && message.isPitchWheel()) dispatchInstrumentPitch(message.getPitchWheelValue());
        return;
    }
    if(static_cast<size_t>(source)>=midi::sourceCount)return;
    if (ump.present()) { dispatchUmpMessage(ump,source); return; }
    const fad::engine::detail::ScopedValue<midi::Source> sourceScope(activeMidiSource, source);
    if (midiObserver != nullptr && !dispatchingUmp && source != midi::Source::touch && source != midi::Source::audition)
        midiObserver(midiObserverOwner,message,{},source,isMpeMemberChannel(message.getChannel()));
    const auto rawMidiChannel = message.getChannel();
    const auto midiChannel = std::clamp(rawMidiChannel, 1, 16);

    if (message.isNoteOn())
    {
        allSoundOffSamplesRemaining = 0;
        // A note arriving during panic supersedes the FX reset, but must not
        // jump the complete-output gain from its partial fade straight to one.
        allSoundOffOutputGain.setTargetValue(1.0f);
        const auto noteNumber = std::clamp(message.getNoteNumber(), 0, 127);
        const auto velocity = std::clamp((umpVelocityOverride >= 0.0f ? umpVelocityOverride : message.getFloatVelocity()), 0.0f, 1.0f);
        const bool hadHeldOrSustainedNotes = hasHeldOrSustainedNotes();
        recordHeldNoteOn(midiChannel, noteNumber);
        fxModSource = activeMidiSource;
        fxModChannel = midiChannel;
        fxModNote = noteNumber;
        fxMidi2Active=false;fxMidi2Detached=false;fxMidi2SlideActive=false;fxMidi2Slide=fxMidi2Pitch=.5f;
        fxModControlsFrozen = false;
        fxModRawVelocity = velocity;
        fxModVelocity = shapeVelocity(fxModRawVelocity, parameterSnapshot.global.velocityCurve);
        fxModReleaseVelocity = 0.0f;
        fxNoisePhase = 0; fxNoiseValue = 0;
        fxNoiseState = 0x6d2b79f5u ^ static_cast<uint32_t>(noteNumber + midiChannel * 128);
        const auto& noise = parameterSnapshot.noise;
        fxNoiseState ^= fxNoiseState << 13; fxNoiseState ^= fxNoiseState >> 17; fxNoiseState ^= fxNoiseState << 5;
        const double noiseRandom = static_cast<double>(fxNoiseState) / 4294967296.0;
        fxNoisePlayer.reset(noise.phase + noise.randomPhase * noiseRandom);
        fxModPolyPressure=0;
        fxLearnedMidiControllers.fill(0);
        fxModReleasePending = false;
        const bool nativeLegato = parameterSnapshot.global.serumAmplitude
            && parameterSnapshot.global.voiceMode == parameters::VoiceMode::mono
            && !parameterSnapshot.global.nativeMonoRetrigger && monoHeldNoteCount > 0
            && activeMonoVoice != nullptr && activeMonoVoice->isVoiceActive();
        if (!nativeLegato)
            fxNoteOnValues = noteOnModulation.next();
        // Poly+Legato preserves the shared FX LFO on overlap, independently
        // of each polyphonic voice's own triggered phase (native probes).
        const bool nativeFxLfoLegato = parameterSnapshot.global.serumAmplitude
            && !parameterSnapshot.global.nativeMonoRetrigger && hadHeldOrSustainedNotes
            && fxModEnvelopes[0].isActive();
        if (!nativeLegato)
            for (auto& envelope : fxModEnvelopes) { envelope.reset(); envelope.noteOn(); }
        if (!nativeFxLfoLegato) {
            fxNodeLfoMask = 0;
            fxNodeCurveCounter = 0;
            for (size_t i = 0; i < fxNoteLfos.size(); ++i)
                if (parameterSnapshot.modulation.lfos[i].triggerMode > 0) fxNoteLfos[i].setPhase(0.0f);
        }
        fxModEnvelopes[0].setSustainModulationEnabled(hasNativeSustainRoutes);
        fxModEnvelopes[0].setDecayModulationEnabled(hasNativeDecayRoutes);
        fxModEnvelopes[0].setReleaseModulationEnabled(hasNativeReleaseRoutes);

        if (parameterSnapshot.global.voiceMode == parameters::VoiceMode::mono)
            handleMonoNoteOn(midiChannel, noteNumber, velocity);
        else
        {
            VoiceCore* releasedNativeVoice = nullptr;
            if (parameterSnapshot.global.serumAmplitude && ! isMpeMemberChannel(midiChannel))
                for (auto* voice : voiceCache)
                    if (ownsVoice(voice) && voice->matchesRealtimeNote(midiChannel, noteNumber)
                        && voice->canResumeReleasedNote()
                        && (releasedNativeVoice == nullptr
                            || voice->getRealtimeStartOrder() > releasedNativeVoice->getRealtimeStartOrder()))
                        releasedNativeVoice = voice;
            if (releasedNativeVoice != nullptr)
            {
                releasedNativeVoice->setNoteOnModulationSources(fxNoteOnValues);
                releasedNativeVoice->resumeNativeReleasedNote(velocity, ++voiceStartCounter);
                if (mpeEnabledForCurrentSource())
                {
                    // A reused release tail kept its former per-note expression.
                    // Reattach the current channel state only after the new key is down.
                    releasedNativeVoice->setMpeMember(isMpeMemberChannel(midiChannel));
                    releasedNativeVoice->setPitchBendRange(effectivePitchBendRange(midiChannel));
                    releasedNativeVoice->pitchWheelMoved(midiState().lastPitchWheelValues[static_cast<size_t>(midiChannel - 1)]);
                    syncVoicePedalsForChannel(*releasedNativeVoice, midiChannel);
                    releasedNativeVoice->glideToRealtimeNote(midiChannel, noteNumber, velocity);
                    applyMpeManagerControls(releasedNativeVoice, true);
                }
                midiState().lastNativeNoteFrequency[static_cast<size_t>(midiChannel - 1)] = static_cast<float>(
                    midi::ShortMessage::getMidiNoteInHertz(noteNumber));
            }
            else
            {
                stopVoicesForNote(midiChannel, noteNumber, 0.0f, true, false);
                startVoice(findFreeVoiceOrVoiceToSteal(noteNumber),
                           midiChannel,
                           noteNumber,
                           velocity);
            }
        }

        // Record only after the real mono/allocation/steal decision. A
        // speculative pre-allocation +1 overstates mono legato and full-poly
        // voice stealing.
        sessionMetrics.recordNoteOn(noteNumber,
                                    static_cast<int>(std::lround(velocity * 127.0f)),
                                    countActiveVoices(voiceCache));
        return;
    }

    if (message.isNoteOff())
    {
        const auto noteNumber = std::clamp(message.getNoteNumber(), 0, 127);
        const auto velocity = std::clamp((umpVelocityOverride >= 0.0f ? umpVelocityOverride : message.getFloatVelocity()), 0.0f, 1.0f);
        // An unmatched NoteOff must not restart release or replace its velocity.
        if (midiState().heldNoteCounts[static_cast<size_t>(midiChannel - 1)][static_cast<size_t>(noteNumber)] == 0)
            return;
        recordHeldNoteOff(midiChannel, noteNumber);
        const bool releasedFxNote = fxModSource == activeMidiSource && fxModChannel == midiChannel && fxModNote == noteNumber;
        if (releasedFxNote)
            fxModReleaseVelocity = velocity;
        if (parameterSnapshot.global.voiceMode == parameters::VoiceMode::mono)
            handleMonoNoteOff(midiChannel, noteNumber, velocity);
        else
            stopVoicesForNote(midiChannel, noteNumber, velocity, true);

        if (releasedFxNote) {
            releaseFxModNote();
        }

        sessionMetrics.recordActiveNoteCount(countActiveVoices(voiceCache));
        return;
    }

    if (message.isAllSoundOff())
    {
        const bool wholeZone = isMpeManagerChannel(rawMidiChannel);
        if (fxModSource == activeMidiSource && (rawMidiChannel == 0 || fxModChannel == midiChannel
            || (wholeZone && mpeManagerChannelFor(fxModChannel) == rawMidiChannel))) {
            for (auto& envelope : fxModEnvelopes) envelope.reset();
    fxNoiseState = 0x6d2b79f5u; fxNoisePhase = 0; fxNoiseValue = 0; fxNoisePlayer.reset();
            fxModNote = -1;
            fxModControlsFrozen = false; fxModReleasePending = false;
        }
        const auto hasActiveVoiceOnAnotherChannel = [&] { return std::any_of(
            voiceCache.begin(), voiceCache.end(),
            [this, rawMidiChannel, wholeZone](const auto* voice)
            {
                return voice != nullptr && voice->isVoiceActive()
                    && (voice->getMidiSource() != activeMidiSource
                        || (rawMidiChannel != 0 && !voice->isPlayingChannel(rawMidiChannel)
                            && !(wholeZone && mpeManagerChannelFor(voice->getRealtimeMidiChannel()) == rawMidiChannel)));
            }); };
        const auto stopChannel = [this](int channel) noexcept
        {
            resetHeldNoteStateForChannel(channel);
            if (parameterSnapshot.global.voiceMode == parameters::VoiceMode::mono)
                resetMonoStateForChannel(channel);
            applySustainController(channel, false);
            applySostenutoController(channel, false);
            stopVoicesForChannel(channel, false);
        };
        if (rawMidiChannel == 0 || wholeZone)
        {
            for (int channel = 1; channel <= 16; ++channel)
                if (rawMidiChannel == 0 || channel == rawMidiChannel || mpeManagerChannelFor(channel) == rawMidiChannel)
                    stopChannel(channel);
        }
        else
            stopChannel(midiChannel);
        if (! hasActiveVoiceOnAnotherChannel())
        {
            // Preserve the running FX/safety history during the short complete
            // output fade; processWetAudioThrough retires it at the deadline.
            beginAllSoundOffFade();
        }
        sessionMetrics.recordActiveNoteCount(countActiveVoices(voiceCache));
        return;
    }

    if (message.isAllNotesOff())
    {
        const bool wholeZone = isMpeManagerChannel(rawMidiChannel);
        const bool releasedFxNote = fxModSource == activeMidiSource && (rawMidiChannel == 0 || fxModChannel == midiChannel
            || (wholeZone && mpeManagerChannelFor(fxModChannel) == rawMidiChannel));
        const auto stopChannel = [this](int channel) noexcept
        {
            // CC123 releases keys but leaves both pedals under MIDI control.
            resetHeldNoteStateForChannel(channel);
            if (parameterSnapshot.global.voiceMode == parameters::VoiceMode::mono)
                resetMonoStateForChannel(channel, false);
            for (auto* voice : voiceCache)
            {
                if (! ownsVoice(voice) || !voice->isPlayingChannel(channel) || !voice->isKeyDown())
                    continue;
                voice->recordNoteOffVelocity(0.0f);
                voice->setKeyDown(false);
                if (!voice->isSustainPedalDown() && !voice->isSostenutoPedalDown())
                    voice->stopRealtimeNote(0.0f, true);
            }
        };
        if (rawMidiChannel == 0 || wholeZone)
        {
            for (int channel = 1; channel <= 16; ++channel)
                if (rawMidiChannel == 0 || channel == rawMidiChannel || mpeManagerChannelFor(channel) == rawMidiChannel)
                    stopChannel(channel);
        }
        else
            stopChannel(midiChannel);
        if (releasedFxNote)
            releaseFxModNote();
        sessionMetrics.recordActiveNoteCount(countActiveVoices(voiceCache));
        return;
    }

    if (message.isPitchWheel())
    {
        const auto safeWheelValue = std::clamp(message.getPitchWheelValue(), 0, 16383);
        sessionMetrics.pitchWheelCount.fetch_add(1, std::memory_order_relaxed);
        midiState().lastPitchWheelValues[static_cast<size_t>(midiChannel - 1)] = safeWheelValue;
        midiState().ciReportValues[static_cast<size_t>(midiChannel - 1)].pitch = midi::expandMidi1<14>(static_cast<uint32_t>(safeWheelValue));
        midiState().pitchWheelBipolar[static_cast<size_t>(midiChannel - 1)] = static_cast<float>(safeWheelValue - 8192) / 8192.0f;
        for (auto* voice : voiceCache)
            if (ownsVoice(voice) && voice->isPlayingChannel(midiChannel))
                voice->pitchWheelMoved(safeWheelValue);
        if (isMpeManagerChannel(midiChannel))
            refreshMpeManagerControls();
        return;
    }

    if (message.isAftertouch())
    {
        sessionMetrics.aftertouchCount.fetch_add(1, std::memory_order_relaxed);
        const auto noteNumber = std::clamp(message.getNoteNumber(), 0, 127);
        const auto aftertouch = std::clamp(message.getAfterTouchValue(), 0, 127);
        if(fxModSource==activeMidiSource && fxModChannel==midiChannel && fxModNote==noteNumber && !fxModControlsFrozen)fxModPolyPressure=static_cast<float>(aftertouch)/127.0f;
        for (auto* voice : voiceCache)
            if (ownsVoice(voice) && voice->matchesRealtimeNote(midiChannel, noteNumber))
                voice->aftertouchChanged(aftertouch);
        return;
    }

    if (message.isChannelPressure())
    {
        sessionMetrics.aftertouchCount.fetch_add(1, std::memory_order_relaxed);
        const auto pressure = std::clamp(message.getChannelPressureValue(), 0, 127);
        midiState().ciReportValues[static_cast<size_t>(midiChannel - 1)].pressure = midi::expandMidi1<7>(static_cast<uint32_t>(pressure));
        midiState().performanceStates[static_cast<size_t>(std::clamp(midiChannel, 1, 16) - 1)].aftertouch =
            std::clamp(static_cast<float>(pressure) / 127.0f, 0.0f, 1.0f);
        for (auto* voice : voiceCache)
            if (ownsVoice(voice) && voice->isPlayingChannel(midiChannel))
                voice->channelPressureChanged(pressure);
        return;
    }

    if (message.isController())
    {
        const auto cc = std::clamp(message.getControllerNumber(), 0, 127);
        const auto value = std::clamp(message.getControllerValue(), 0, 127);
        // In an MPE zone only pitch-bend sensitivity and the three per-note
        // dimensions are accepted on member channels. Ordinary CCs belong to
        // the manager channel and must not affect a single member note.
        if (isMpeMemberChannel(midiChannel)
            && cc != 74 && cc != 101 && cc != 100
            && cc != 6 && cc != 38 && cc != 99 && cc != 98)
            return;
        const auto channelIndex = static_cast<size_t>(std::clamp(midiChannel, 1, 16) - 1);
        if (cc == 1) // Mod Wheel
        {
            midiState().performanceStates[channelIndex].modWheel =
                std::clamp(static_cast<float>(value) / 127.0f, 0.0f, 1.0f);
            sessionMetrics.modWheelCount.fetch_add(1, std::memory_order_relaxed);
        }
        else if (cc == 7)
        {
            midiState().performanceStates[channelIndex].channelVolume = static_cast<float>(value) / 127.0f;
            if (isMpeManagerChannel(midiChannel))
                refreshMpeManagerControls();
        }
        else if (cc == 11)
        {
            midiState().performanceStates[channelIndex].expression = static_cast<float>(value) / 127.0f;
            if (isMpeManagerChannel(midiChannel))
                refreshMpeManagerControls();
        }
        else if (cc == 74)
        {
            midiState().performanceStates[channelIndex].slide = static_cast<float>(value) / 127.0f;
            midiState().performanceStates[channelIndex].slideReceived = true;
        }
        else if (cc == 101)
            midiState().rpnMsb[channelIndex] = static_cast<uint8_t>(value);
        else if (cc == 100)
            midiState().rpnLsb[channelIndex] = static_cast<uint8_t>(value);
        else if (cc == 99 || cc == 98) // NRPN selection cancels the current RPN.
            midiState().rpnMsb[channelIndex] = midiState().rpnLsb[channelIndex] = 127;
        else if (cc == 6 && midiState().rpnMsb[channelIndex] == 0 && midiState().rpnLsb[channelIndex] == 6
                 && (midiChannel == 1 || midiChannel == 16) && mpeEnabledForCurrentSource() && !midiState().ciProfileActive)
        {
            // Values 16..127 are reserved, not a request for a maximal zone.
            if (value <= 15) configureMpeZone(midiChannel, value);
        }
        else if ((cc == 6 || cc == 38) && midiState().rpnMsb[channelIndex] == 0
                 && midiState().rpnLsb[channelIndex] == 0)
        {
            const auto manager = mpeManagerChannelFor(midiChannel);
            if (midiState().ciProfileActive && (manager != 0 || isMpeManagerChannel(midiChannel)))
            {
                // MPE Profile RPN 0/0 belongs only to the Manager, and applies
                // the identical range to every channel in its cluster.
                if (manager == 0)
                {
                    auto& coarse = midiState().ciProfileBendCoarse[channelIndex];
                    auto& range = midiState().ciProfileBendRanges[channelIndex];
                    if (cc == 6) coarse = static_cast<uint8_t>(std::min(96, value));
                    range = static_cast<float>(coarse) + (cc == 38 ? std::min(99, value) * .01f : 0.0f);
                    range = std::min(96.0f, range);
                    for (auto* voice : voiceCache)
                        if (ownsVoice(voice) && mpeManagerChannelFor(voice->getRealtimeMidiChannel()) == midiChannel)
                            voice->setPitchBendRange(range);
                    refreshMpeManagerControls();
                }
            }
            else
            {
                auto& zone = midiState().mpeZones[(manager != 0 ? manager : midiChannel) == 16 ? 1 : 0];
                auto& coarse = manager != 0 ? zone.memberBendCoarse : midiState().bendRangeCoarse[channelIndex];
                auto& range = manager != 0 ? zone.memberBend
                    : isMpeManagerChannel(midiChannel) ? zone.managerBend : midiState().pitchBendRangeOverrides[channelIndex];
                if (cc == 6) { coarse = static_cast<uint8_t>(value); range = coarse; }
                else if (range >= 0.0f) range = static_cast<float>(coarse) + static_cast<float>(std::min(99, value)) * .01f;
                for (auto* voice : voiceCache)
                    if (ownsVoice(voice) && (voice->isPlayingChannel(midiChannel)
                        || (manager != 0 && mpeManagerChannelFor(voice->getRealtimeMidiChannel()) == manager)))
                        voice->setPitchBendRange(effectivePitchBendRange(voice->getRealtimeMidiChannel()));
                if (isMpeManagerChannel(midiChannel)) refreshMpeManagerControls();
            }
        }
        else if (cc == 64 && value >= 64) // Sustain Pedal on
        {
            applySustainController(midiChannel, true);
            sessionMetrics.sustainPedalCount.fetch_add(1, std::memory_order_relaxed);
        }
        else if (cc == 64)
        {
            applySustainController(midiChannel, false);
        }
        else if (cc == 66)
        {
            applySostenutoController(midiChannel, value >= 64);
        }
        else if (cc == 121) // Reset All Controllers
        {
            // RP-015 leaves Channel Volume (CC7) untouched; Expression and the
            // other performance controllers return to their defaults.
            const auto channelVolume = midiState().performanceStates[channelIndex].channelVolume;
            midiState().performanceStates[channelIndex] = {};
            midiState().performanceStates[channelIndex].channelVolume = channelVolume;
            if (fxModSource == activeMidiSource && fxModChannel == midiChannel)
                fxModPolyPressure = 0.0f;
            midiState().lastPitchWheelValues[channelIndex] = 8192;
            midiState().ciReportValues[channelIndex] = {};
            midiState().pitchWheelBipolar[channelIndex] = 0;
            for (auto* voice : voiceCache)
                if (ownsVoice(voice) && voice->isPlayingChannel(midiChannel))
                    voice->pitchWheelMoved(8192);
            // Reset selection, not the parameter data already written by RPN.
            midiState().rpnMsb[channelIndex] = midiState().rpnLsb[channelIndex] = 127;
            applySustainController(midiChannel, false);
            applySostenutoController(midiChannel, false);
            if (isMpeManagerChannel(midiChannel))
                refreshMpeManagerControls();
        }

        if (fxModSource == activeMidiSource && fxModReleasePending && (cc == 64 || cc == 66 || cc == 121)
            && (fxModChannel == midiChannel
                || (mpeManagerChannelFor(fxModChannel) == midiChannel))
            && !isFxModNotePedalHeld())
        {
            for (auto& envelope : fxModEnvelopes) envelope.noteOff();
            fxModReleasePending = false;
        }

        for (auto* voice : voiceCache)
            if (ownsVoice(voice) && voice->isPlayingChannel(midiChannel))
                voice->controllerMoved(cc, value);
        return;
    }

    if (message.isProgramChange())
        return;
}

VoiceCore* RenderEngine::findFreeVoice() noexcept
{
    for (auto* voice : voiceCache)
        if (voice != nullptr && ! voice->isVoiceActive())
            return voice;

    return nullptr;
}

VoiceCore* RenderEngine::findVoiceToSteal(int midiNoteNumber) noexcept
{
    const auto targetNote = std::clamp(midiNoteNumber, 0, 127);
    VoiceCore* oldestReleased = nullptr;
    VoiceCore* oldestAny = nullptr;

    for (auto* voice : voiceCache)
    {
        if (voice == nullptr || ! voice->isVoiceActive())
            continue;

        if (ownsVoice(voice) && ! mpeEnabledForCurrentSource()
            && voice->getRealtimeMidiNote() == targetNote)
            return voice;

        if (oldestAny == nullptr || voice->getRealtimeStartOrder() < oldestAny->getRealtimeStartOrder())
            oldestAny = voice;

        if (! voice->isKeyDown()
            && (oldestReleased == nullptr
                || voice->getRealtimeStartOrder() < oldestReleased->getRealtimeStartOrder()))
        {
            oldestReleased = voice;
        }
    }

    return oldestReleased != nullptr ? oldestReleased : oldestAny;
}

VoiceCore* RenderEngine::findFreeVoiceOrVoiceToSteal(int midiNoteNumber) noexcept
{
    if (auto* voice = findFreeVoice(); voice != nullptr)
        return voice;

    auto* voice = findVoiceToSteal(midiNoteNumber);
    if (voice != nullptr)
        sessionMetrics.recordVoiceSteal();
    return voice;
}

void RenderEngine::startVoice(VoiceCore* voice, int midiChannel, int midiNoteNumber, float velocity)
{
    if (voice == nullptr)
        return;

    attachVoiceSource(voice);
    const auto safeChannel = std::clamp(midiChannel, 1, 16);
    voice->setMpeMember(isMpeMemberChannel(safeChannel));
    voice->setPitchBendRange(effectivePitchBendRange(safeChannel));
    voice->setNativeGlideStartFrequency(0.0f);
    voice->setNoteOnModulationSources(fxNoteOnValues);
    if (parameterSnapshot.global.serumAmplitude)
        voice->setNativePhaseRandomValues(nativePhaseRandom.nextNote());
    const auto channelIndex = static_cast<size_t>(safeChannel - 1);
    bool overlapping = false;
    for (size_t noteIndex = 0; noteIndex < midiState().heldNoteCounts[channelIndex].size(); ++noteIndex)
        if (midiState().heldNoteCounts[channelIndex][noteIndex] > (static_cast<int>(noteIndex) == midiNoteNumber ? 1 : 0))
            overlapping = true;
    const bool glide = parameterSnapshot.global.glideMode == parameters::GlideMode::always
        || (parameterSnapshot.global.serumAmplitude
            && parameterSnapshot.global.glideMode == parameters::GlideMode::legato && overlapping);
    if (glide)
        voice->setNativeGlideStartFrequency(midiState().lastNativeNoteFrequency[channelIndex]);
    // This is performance history, independent of the chosen voice or glide
    // profile. Off/Legato notes also become the next Always note's origin.
    midiState().lastNativeNoteFrequency[channelIndex] = static_cast<float>(
        midi::ShortMessage::getMidiNoteInHertz(std::clamp(midiNoteNumber, 0, 127)));
    voice->startRealtimeNote(safeChannel,
                             std::clamp(midiNoteNumber, 0, 127),
                             std::clamp(velocity, 0.0f, 1.0f),
                             midiState().lastPitchWheelValues[static_cast<size_t>(safeChannel - 1)],
                             ++voiceStartCounter,
                             midiState().sustainPedalDown[static_cast<size_t>(safeChannel - 1)]);
    applyMpeManagerControls(voice, true);
}

void RenderEngine::stopVoicesForNote(int midiChannel,
                                    int midiNoteNumber,
                                    float velocity,
                                    bool allowTailOff, bool realNoteOff) noexcept
{
    const auto safeChannel = std::clamp(midiChannel, 1, 16);
    const auto safeNote = std::clamp(midiNoteNumber, 0, 127);
    for (auto* voice : voiceCache)
    {
        if (! ownsVoice(voice) || ! voice->matchesRealtimeNote(safeChannel, safeNote)
            || (isMpeMemberChannel(safeChannel) && ! voice->isKeyDown()))
            continue;

        if (realNoteOff) voice->recordNoteOffVelocity(velocity);
        voice->setKeyDown(false);
        if (! voice->isSustainPedalDown() && ! voice->isSostenutoPedalDown())
        {
            if (!realNoteOff && allowTailOff) voice->releasePedalHeldNote();
            else voice->stopRealtimeNote(velocity, allowTailOff);
        }
    }
}

void RenderEngine::stopVoicesForChannel(int midiChannel, bool allowTailOff) noexcept
{
    for (auto* voice : voiceCache)
    {
        // A mode change must not start a steal fade on an idle voice: that
        // would make a cold reset defer the next MIDI events as audible tails.
        if (voice == nullptr || (midiChannel > 0 && !ownsVoice(voice)) || !voice->isVoiceActive())
            continue;

        if (midiChannel <= 0 || voice->isPlayingChannel(midiChannel))
        {
            if (allowTailOff) voice->releasePedalHeldNote();
            else voice->stopRealtimeNote(0.0f, false);
        }
    }

    if (midiChannel <= 0)
        midiState().sustainPedalDown.fill(false);
    else
        midiState().sustainPedalDown[static_cast<size_t>(std::clamp(midiChannel, 1, 16) - 1)] = false;
}

void RenderEngine::syncVoicePedalsForChannel(VoiceCore& voice, int midiChannel) noexcept
{
    const auto channel = std::clamp(midiChannel, 1, 16);
    // Sustain belongs to the destination channel. Sostenuto captures existing
    // notes on its rising edge; a handoff must not carry another channel's latch.
    if (voice.getRealtimeMidiChannel() != channel)
        voice.setSostenutoPedalDown(false);
    voice.setSustainPedalDown(midiState().sustainPedalDown[static_cast<size_t>(channel - 1)]);
}

void RenderEngine::retireFxModNote() noexcept
{
    for (auto& envelope : fxModEnvelopes) envelope.reset();
    fxNoiseState = 0x6d2b79f5u; fxNoisePhase = 0; fxNoiseValue = 0; fxNoisePlayer.reset();
    fxModNote = -1;
    fxModRawVelocity = fxModVelocity = fxModReleaseVelocity = fxModPolyPressure = 0.0f;
    fxModControlsFrozen = false;
    fxModReleasePending = false;
    fxMidi2Active=fxMidi2Detached=fxMidi2SlideActive=false;
    fxLearnedMidiControllers.fill(0);
}

void RenderEngine::releaseFxModNote() noexcept
{
    const fad::engine::detail::ScopedValue<midi::Source> sourceScope(activeMidiSource, fxModSource);
    if (parameterSnapshot.global.voiceMode == parameters::VoiceMode::mono && monoHeldNoteCount > 0)
    {
        const auto& held = monoHeldNotes[monoHeldNoteCount - 1];
        fxModSource = held.source;
        fxModChannel = held.midiChannel;
        fxModNote = held.midiNoteNumber;
        fxModControlsFrozen = false;
        fxModRawVelocity = held.velocity;
        fxModVelocity = shapeVelocity(fxModRawVelocity, parameterSnapshot.global.velocityCurve);
        fxModPolyPressure = 0.0f;
        fxModReleaseVelocity = 0.0f;
        fxModReleasePending = false;
        fxMidi2Active=held.ump.present();fxMidi2Detached=fxMidi2SlideActive=false;
        fxMidi2Pitch=fxMidi2Slide=.5f;fxLearnedMidiControllers.fill(0);
        if (parameterSnapshot.global.serumAmplitude && parameterSnapshot.global.nativeMonoRetrigger)
        {
            for (auto& envelope : fxModEnvelopes) envelope.noteOn();
            for (size_t i = 0; i < fxNoteLfos.size(); ++i)
                if (parameterSnapshot.modulation.lfos[i].triggerMode > 0) fxNoteLfos[i].setPhase(0.0f);
        }
        if(held.ump.present())
        {
            const auto channel=static_cast<size_t>(held.midiChannel-1);
            const auto& controls=midi2NoteStates[(static_cast<size_t>(held.source)*16+channel)*128+static_cast<size_t>(held.midiNoteNumber)];
            fxModPolyPressure=controls.pressure;fxMidi2Pitch=controls.pitch;fxMidi2Slide=controls.slide;fxMidi2SlideActive=controls.slideReceived;
            if(activeMonoVoice && activeMonoVoice->getMidiSource()==held.source && activeMonoVoice->matchesRealtimeNote(held.midiChannel,held.midiNoteNumber))
            {
                const auto pitch=held.ump.byte3()==3 ? static_cast<float>(held.ump.data()&65535)/512.0f : controls.tuning;
                activeMonoVoice->beginMidi2Note(midiSourceStates[static_cast<size_t>(held.source)].perNoteBendRanges[channel],pitch);
                activeMonoVoice->syncMidi2ChannelControls();
                activeMonoVoice->setMidi2ChannelPitch(midiSourceStates[static_cast<size_t>(held.source)].pitchWheelBipolar[channel]);
                activeMonoVoice->setMidi2PolyPressure(controls.pressure);
                if(controls.slideReceived)activeMonoVoice->setMidi2Slide(controls.slide);
                activeMonoVoice->setMidi2NotePitch((controls.pitch-.5f)*2.0f);
                if(midiNoteInitialiser)midiNoteInitialiser(midiObserverOwner,held.source,held.midiChannel,held.midiNoteNumber);
            }
        }
    }
    else
    {
        if (isMpeMemberChannel(fxModChannel) && !fxModControlsFrozen)
        {
            const auto index = static_cast<size_t>(fxModChannel - 1);
            fxModReleasedPerformance = midiState().performanceStates[index];
            fxModReleasedPitch = .5f+.5f*midiState().pitchWheelBipolar[index];
            fxModReleasedBendRange = effectivePitchBendRange(fxModChannel);
            fxModControlsFrozen = true;
        }
        fxModReleasePending = isFxModNotePedalHeld();
        if (!fxModReleasePending)
            for (auto& envelope : fxModEnvelopes) envelope.noteOff();
    }
}

bool RenderEngine::isFxModNotePedalHeld() const noexcept
{
    const auto& fxState = midiSourceStates[static_cast<size_t>(fxModSource)];
    if (fxState.sustainPedalDown[static_cast<size_t>(std::clamp(fxModChannel, 1, 16) - 1)])
        return true;

    // The newest matching voice owns this note's capture. An older same-key
    // release tail must not lend its sostenuto latch to a later articulation.
    const VoiceCore* latest = nullptr;
    for (const auto* voice : voiceCache)
        if (voice != nullptr && voice->getMidiSource() == fxModSource && voice->matchesRealtimeNote(fxModChannel, fxModNote)
            && (latest == nullptr || voice->getRealtimeStartOrder() > latest->getRealtimeStartOrder()))
            latest = voice;
    return latest != nullptr && latest->isSostenutoPedalDown();
}

void RenderEngine::handleSustainPedal(int midiChannel, bool isDown) noexcept
{
    const auto safeChannel = std::clamp(midiChannel, 1, 16);
    const auto channelIndex = static_cast<size_t>(safeChannel - 1);
    midiState().sustainPedalDown[channelIndex] = isDown;

    for (auto* voice : voiceCache)
    {
        if (! ownsVoice(voice) || ! voice->isPlayingChannel(safeChannel))
            continue;

        const bool wasDown = voice->isSustainPedalDown();
        voice->setSustainPedalDown(isDown);
        if (wasDown && ! isDown && ! voice->isKeyDown() && ! voice->isSostenutoPedalDown())
            voice->releasePedalHeldNote();
    }
}

void RenderEngine::handleSostenutoPedal(int midiChannel, bool isDown) noexcept
{
    const auto safeChannel = std::clamp(midiChannel, 1, 16);

    for (auto* voice : voiceCache)
    {
        if (! ownsVoice(voice) || ! voice->isPlayingChannel(safeChannel))
            continue;

        if (isDown)
        {
            if (voice->isKeyDown())
                voice->setSostenutoPedalDown(true);
        }
        else if (voice->isSostenutoPedalDown())
        {
            voice->setSostenutoPedalDown(false);
            if (! voice->isKeyDown() && ! voice->isSustainPedalDown())
                voice->releasePedalHeldNote();
        }
    }
}

void RenderEngine::handleMonoNoteOn(int midiChannel, int midiNoteNumber, float velocity)
{
    if (activeMonoVoice != nullptr)
        activeMonoVoice->setNoteOnModulationSources(fxNoteOnValues);
    const auto safeChannel = std::clamp(midiChannel, 1, 16);
    const auto safeNote = std::clamp(midiNoteNumber, 0, 127);
    const auto safeVelocity = std::clamp(velocity, 0.0f, 1.0f);
    const bool wasAlreadyPlaying = (monoHeldNoteCount > 0);
    removeHeldMonoNote(safeChannel, safeNote);

    if (monoHeldNoteCount < monoHeldNotes.size())
        monoHeldNotes[monoHeldNoteCount++] = {safeChannel, safeNote, safeVelocity, activeMidiSource, {}};

    currentMonoSource = activeMidiSource;
    currentMonoChannel = safeChannel;
    currentMonoNote = safeNote;

    if (wasAlreadyPlaying)
    {
        // True legato: change pitch on active voice without retriggering envelope
        if (activeMonoVoice != nullptr && activeMonoVoice->isVoiceActive())
        {
            attachVoiceSource(activeMonoVoice);
            activeMonoVoice->setMpeMember(isMpeMemberChannel(safeChannel));
            activeMonoVoice->setPitchBendRange(effectivePitchBendRange(safeChannel));
            activeMonoVoice->pitchWheelMoved(midiState().lastPitchWheelValues[static_cast<size_t>(safeChannel - 1)]);
            syncVoicePedalsForChannel(*activeMonoVoice, safeChannel);
            activeMonoVoice->glideToRealtimeNote(safeChannel, safeNote, safeVelocity);
            applyMpeManagerControls(activeMonoVoice, true);
            midiState().lastNativeNoteFrequency[static_cast<size_t>(safeChannel - 1)] =
                    static_cast<float>(midi::ShortMessage::getMidiNoteInHertz(safeNote));
            if (parameterSnapshot.global.serumAmplitude && parameterSnapshot.global.nativeMonoRetrigger)
                activeMonoVoice->resumeNativeReleasedNote(safeVelocity, ++voiceStartCounter);
            monoVoiceStartedChannel = safeChannel;
            monoVoiceStartedNote = safeNote;
            return;
        }

        activeMonoVoice = findFreeVoiceOrVoiceToSteal(safeNote);
        startVoice(activeMonoVoice,
                   safeChannel,
                   safeNote,
                   safeVelocity);
        monoVoiceStartedChannel = safeChannel;
        monoVoiceStartedNote = safeNote;
    }
    else
    {
        // First note: normal trigger
        VoiceCore* nativeRelease = nullptr;
        for (auto* candidate : voiceCache)
            if (candidate != nullptr && candidate->isVoiceActive()
                && (candidate->canResumeReleasedNote()
                    || (!candidate->isKeyDown()
                        && (candidate->isSustainPedalDown() || candidate->isSostenutoPedalDown())))
                && (nativeRelease == nullptr
                    || candidate->getRealtimeStartOrder() > nativeRelease->getRealtimeStartOrder()))
                nativeRelease = candidate;
        // Mono owns one voice across releases, pedal holds and MIDI channels.
        // Rearticulate its envelopes while preserving oscillator/filter history;
        // allocating another voice here leaves the old pitch ringing in parallel.
        if (nativeRelease != nullptr)
        {
            activeMonoVoice = nativeRelease;
            // A new key must not inherit a previous note's sostenuto capture.
            activeMonoVoice->setSostenutoPedalDown(false);
            // A release tail is not a held-key overlap. Only Always portamento
            // glides here; preserve Legato/Off's immediate pitch-change contract.
            activeMonoVoice->setGlideMode(parameterSnapshot.global.glideMode == parameters::GlideMode::always
                ? parameters::GlideMode::always : parameters::GlideMode::off);
            if (mpeEnabledForCurrentSource())
                activeMonoVoice->resumeNativeReleasedNote(safeVelocity, ++voiceStartCounter);
            attachVoiceSource(activeMonoVoice);
            activeMonoVoice->setMpeMember(isMpeMemberChannel(safeChannel));
            activeMonoVoice->setPitchBendRange(effectivePitchBendRange(safeChannel));
            activeMonoVoice->pitchWheelMoved(midiState().lastPitchWheelValues[static_cast<size_t>(safeChannel - 1)]);
            syncVoicePedalsForChannel(*activeMonoVoice, safeChannel);
            activeMonoVoice->glideToRealtimeNote(safeChannel, safeNote, safeVelocity);
            activeMonoVoice->setGlideMode(parameterSnapshot.global.glideMode);
            if (! mpeEnabledForCurrentSource())
                activeMonoVoice->resumeNativeReleasedNote(safeVelocity, ++voiceStartCounter);
            applyMpeManagerControls(activeMonoVoice, true);
            midiState().lastNativeNoteFrequency[static_cast<size_t>(safeChannel-1)] =
                static_cast<float>(midi::ShortMessage::getMidiNoteInHertz(safeNote));
            monoVoiceStartedChannel = safeChannel;
            monoVoiceStartedNote = safeNote;
            return;
        }
        activeMonoVoice = findFreeVoiceOrVoiceToSteal(safeNote);
        startVoice(activeMonoVoice,
                   safeChannel,
                   safeNote,
                   safeVelocity);
        monoVoiceStartedChannel = safeChannel;
        monoVoiceStartedNote = safeNote;
    }
}

void RenderEngine::handleMonoNoteOff(int midiChannel, int midiNoteNumber, float velocity)
{
    const auto safeChannel = std::clamp(midiChannel, 1, 16);
    const auto safeNote = std::clamp(midiNoteNumber, 0, 127);
    const auto safeVelocity = std::clamp(velocity, 0.0f, 1.0f);
    removeHeldMonoNote(safeChannel, safeNote);

    if (currentMonoSource != activeMidiSource || currentMonoNote != safeNote || currentMonoChannel != safeChannel)
        return;

    if (monoHeldNoteCount > 0)
    {
        const auto& heldNote = monoHeldNotes[monoHeldNoteCount - 1];
        const fad::engine::detail::ScopedValue<midi::Source> heldSource(activeMidiSource, heldNote.source);
        currentMonoSource = heldNote.source;
        currentMonoChannel = heldNote.midiChannel;
        currentMonoNote = heldNote.midiNoteNumber;
        // Legato glide back: change pitch on active voice
        if (activeMonoVoice != nullptr && activeMonoVoice->isVoiceActive())
        {
            attachVoiceSource(activeMonoVoice);
            activeMonoVoice->setMpeMember(isMpeMemberChannel(heldNote.midiChannel));
            activeMonoVoice->setPitchBendRange(effectivePitchBendRange(heldNote.midiChannel));
            activeMonoVoice->pitchWheelMoved(midiState().lastPitchWheelValues[static_cast<size_t>(heldNote.midiChannel - 1)]);
            syncVoicePedalsForChannel(*activeMonoVoice, heldNote.midiChannel);
            activeMonoVoice->glideToRealtimeNote(heldNote.midiChannel,
                                                 heldNote.midiNoteNumber,
                                                 heldNote.velocity);
            applyMpeManagerControls(activeMonoVoice, true);
            midiState().lastNativeNoteFrequency[static_cast<size_t>(heldNote.midiChannel - 1)] =
                    static_cast<float>(midi::ShortMessage::getMidiNoteInHertz(heldNote.midiNoteNumber));
            if (parameterSnapshot.global.serumAmplitude && parameterSnapshot.global.nativeMonoRetrigger)
                activeMonoVoice->resumeNativeReleasedNote(heldNote.velocity, ++voiceStartCounter);
            monoVoiceStartedChannel = heldNote.midiChannel;
            monoVoiceStartedNote = heldNote.midiNoteNumber;
            return;
        }

        activeMonoVoice = findFreeVoiceOrVoiceToSteal(heldNote.midiNoteNumber);
        startVoice(activeMonoVoice,
                   heldNote.midiChannel,
                   heldNote.midiNoteNumber,
                   heldNote.velocity);
        monoVoiceStartedChannel = heldNote.midiChannel;
        monoVoiceStartedNote = heldNote.midiNoteNumber;
        return;
    }

    const auto releaseNote = (monoVoiceStartedNote >= 0) ? monoVoiceStartedNote : safeNote;
    const auto releaseChannel = (monoVoiceStartedNote >= 0) ? monoVoiceStartedChannel : safeChannel;
    if (activeMonoVoice != nullptr && activeMonoVoice->isVoiceActive())
    {
        activeMonoVoice->recordNoteOffVelocity(safeVelocity);
        activeMonoVoice->setKeyDown(false);
        if (! activeMonoVoice->isSustainPedalDown() && ! activeMonoVoice->isSostenutoPedalDown())
            activeMonoVoice->stopRealtimeNote(safeVelocity, true);
    }
    else
    {
        stopVoicesForNote(releaseChannel, releaseNote, safeVelocity, true);
    }

    activeMonoVoice = nullptr;
    currentMonoChannel = 1;
    currentMonoNote = -1;
    monoVoiceStartedChannel = 1;
    monoVoiceStartedNote = -1;
}

void RenderEngine::removeHeldMonoNote(int midiChannel, int midiNoteNumber) noexcept
{
    for (size_t noteIndex = 0; noteIndex < monoHeldNoteCount; ++noteIndex)
    {
        if (monoHeldNotes[noteIndex].source == activeMidiSource
            && monoHeldNotes[noteIndex].midiChannel == midiChannel
            && monoHeldNotes[noteIndex].midiNoteNumber == midiNoteNumber)
        {
            for (size_t shiftIndex = noteIndex + 1; shiftIndex < monoHeldNoteCount; ++shiftIndex)
                monoHeldNotes[shiftIndex - 1] = monoHeldNotes[shiftIndex];

            --monoHeldNoteCount;
            monoHeldNotes[monoHeldNoteCount] = {};
            return;
        }
    }
}

void RenderEngine::recordHeldNoteOn(int midiChannel, int midiNoteNumber) noexcept
{
    const auto channelIndex = static_cast<size_t>(std::clamp(midiChannel, 1, 16) - 1);
    const auto noteIndex = static_cast<size_t>(std::clamp(midiNoteNumber, 0, 127));
    auto& count = midiState().heldNoteCounts[channelIndex][noteIndex];
    if (count == 0)
        totalHeldNoteCount.fetch_add(1, std::memory_order_relaxed);
    if (count < std::numeric_limits<uint16_t>::max())
        ++count;
}

void RenderEngine::recordHeldNoteOff(int midiChannel, int midiNoteNumber) noexcept
{
    const auto channelIndex = static_cast<size_t>(std::clamp(midiChannel, 1, 16) - 1);
    const auto noteIndex = static_cast<size_t>(std::clamp(midiNoteNumber, 0, 127));
    auto& count = midiState().heldNoteCounts[channelIndex][noteIndex];
    if (count > 0)
    {
        if (count == 1)
            totalHeldNoteCount.fetch_sub(1, std::memory_order_relaxed);
        --count;
    }
}

void RenderEngine::resetHeldNoteState() noexcept
{
    for (auto& source : midiSourceStates)
    {
        source.lastNativeNoteFrequency.fill(0.0f);
        for (auto& notes : source.heldNoteCounts) notes.fill(0);
        source.sustainPedalDown.fill(false);
    }
    totalHeldNoteCount.store(0, std::memory_order_relaxed);
}

void RenderEngine::resetHeldNoteStateForChannel(int midiChannel) noexcept
{
    if (midiChannel <= 0)
    {
        resetHeldNoteState();
        return;
    }

    const auto channelIndex = static_cast<size_t>(std::clamp(midiChannel, 1, 16) - 1);
    int channelHeldNotes = 0;
    for (const auto count : midiState().heldNoteCounts[channelIndex])
        if (count > 0)
            ++channelHeldNotes;
    midiState().heldNoteCounts[channelIndex].fill(0);
    midiState().lastNativeNoteFrequency[channelIndex] = 0.0f;
    if (channelHeldNotes > 0)
        totalHeldNoteCount.fetch_sub(channelHeldNotes, std::memory_order_relaxed);
}

bool RenderEngine::hasActiveVoices() const noexcept
{
    return std::any_of(voiceCache.begin(),
                       voiceCache.end(),
                       [](const VoiceCore* voice)
                       {
                           return voice != nullptr && voice->isVoiceActive();
                       });
}

bool RenderEngine::hasHeldOrSustainedNotes() const noexcept
{
    if (totalHeldNoteCount.load(std::memory_order_relaxed) > 0)
        return true;

    for (const auto& source : midiSourceStates)
        for (const bool pedal : source.sustainPedalDown)
            if (pedal) return true;

    // Sostenuto holds the voices captured by CC66, not every note on its MIDI
    // channel. Keep those existing per-voice leases through hard bypass too.
    for (const auto* voice : voiceCache)
        if (voice != nullptr && voice->isVoiceActive() && voice->isSostenutoPedalDown())
            return true;

    return false;
}

void RenderEngine::resetMonoState() noexcept
{
    monoHeldNotes.fill({});
    monoHeldNoteCount = 0;
    currentMonoChannel = 1;
    currentMonoNote = -1;
    monoVoiceStartedChannel = 1;
    monoVoiceStartedNote = -1;
    activeMonoVoice = nullptr;
}

void RenderEngine::resetMonoStateForChannel(int midiChannel, bool stopVoices)
{
    auto* monoVoice = std::exchange(activeMonoVoice, nullptr);

    if (midiChannel <= 0)
    {
        if (stopVoices) stopVoicesForChannel(0, true);
        resetMonoState();
        return;
    }

    const auto safeChannel = std::clamp(midiChannel, 1, 16);
    const bool removedCurrentNote = currentMonoSource == activeMidiSource && currentMonoChannel == safeChannel;
    size_t writeIndex = 0;
    for (size_t readIndex = 0; readIndex < monoHeldNoteCount; ++readIndex)
    {
        if (monoHeldNotes[readIndex].source != activeMidiSource || monoHeldNotes[readIndex].midiChannel != safeChannel)
            monoHeldNotes[writeIndex++] = monoHeldNotes[readIndex];
    }

    for (size_t clearIndex = writeIndex; clearIndex < monoHeldNoteCount; ++clearIndex)
        monoHeldNotes[clearIndex] = {};
    monoHeldNoteCount = writeIndex;

    if (monoHeldNoteCount == 0)
    {
        if (stopVoices) stopVoicesForChannel(safeChannel, true);
        resetMonoState();
        return;
    }

    const MonoHeldNote* targetNote = nullptr;
    if (! removedCurrentNote)
    {
        for (size_t noteIndex = 0; noteIndex < monoHeldNoteCount; ++noteIndex)
        {
            if (monoHeldNotes[noteIndex].source == currentMonoSource
                && monoHeldNotes[noteIndex].midiChannel == currentMonoChannel
                && monoHeldNotes[noteIndex].midiNoteNumber == currentMonoNote)
            {
                targetNote = &monoHeldNotes[noteIndex];
                break;
            }
        }
    }

    if (targetNote == nullptr)
        targetNote = &monoHeldNotes[monoHeldNoteCount - 1];

    const fad::engine::detail::ScopedValue<midi::Source> targetSource(activeMidiSource, targetNote->source);
    currentMonoSource = targetNote->source;
    currentMonoChannel = targetNote->midiChannel;
    currentMonoNote = targetNote->midiNoteNumber;

    if (monoVoice != nullptr && monoVoice->isVoiceActive())
    {
        activeMonoVoice = monoVoice;
        attachVoiceSource(activeMonoVoice);
        activeMonoVoice->setMpeMember(isMpeMemberChannel(targetNote->midiChannel));
        activeMonoVoice->setPitchBendRange(effectivePitchBendRange(targetNote->midiChannel));
        activeMonoVoice->pitchWheelMoved(midiState().lastPitchWheelValues[static_cast<size_t>(targetNote->midiChannel - 1)]);
        syncVoicePedalsForChannel(*activeMonoVoice, targetNote->midiChannel);
        activeMonoVoice->glideToRealtimeNote(targetNote->midiChannel,
                                             targetNote->midiNoteNumber,
                                             targetNote->velocity);
        applyMpeManagerControls(activeMonoVoice, true);
        midiState().lastNativeNoteFrequency[static_cast<size_t>(targetNote->midiChannel - 1)] =
                static_cast<float>(midi::ShortMessage::getMidiNoteInHertz(targetNote->midiNoteNumber));
        monoVoiceStartedChannel = targetNote->midiChannel;
        monoVoiceStartedNote = targetNote->midiNoteNumber;
        return;
    }

    activeMonoVoice = findFreeVoiceOrVoiceToSteal(targetNote->midiNoteNumber);
    startVoice(activeMonoVoice,
               targetNote->midiChannel,
               targetNote->midiNoteNumber,
               targetNote->velocity);
    monoVoiceStartedChannel = targetNote->midiChannel;
    monoVoiceStartedNote = targetNote->midiNoteNumber;
}

float RenderEngine::resolveLfoRateHz(const parameters::LfoSnapshot& snapshot) const noexcept
{
    return modulation::baseLfoRate(snapshot, tempoBpm);
}

void RenderEngine::applyOutputGain(fad::engine::AudioBufferView audioBuffer)
{
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    const auto numChannels = audioBuffer.getNumChannels();
    const auto numSamples = audioBuffer.getNumSamples();
    const auto clampedChannels = std::min(numChannels, 2);
    const auto maxOutputGain = std::pow(10.0f, 6.0f * 0.05f);
    uint64_t clippedSamples = 0;
    uint64_t nonFiniteSamples = 0;

    float* channelPtrs[2] = {nullptr, nullptr};
    for (int ch = 0; ch < clampedChannels; ++ch)
        channelPtrs[ch] = audioBuffer.getWritePointer(ch);

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const auto gainValue =
            fadmodel1::dsp::utils::sanitizeFiniteClamped(outputGainSmoother.getNextValue(),
                                                         1.0f,
                                                         0.0f,
                                                         maxOutputGain);

        for (int channel = 0; channel < clampedChannels; ++channel)
        {
            if (channelPtrs[channel] == nullptr)
                continue;

            const auto rawOutput = channelPtrs[channel][sampleIndex] * gainValue;
            if (! std::isfinite(rawOutput))
                ++nonFiniteSamples;
            channelPtrs[channel][sampleIndex] = fadmodel1::dsp::utils::sanitizeFinite(rawOutput);
            const auto magnitude = std::abs(channelPtrs[channel][sampleIndex]);
            if (! std::isfinite(rawOutput) || magnitude > 1.0f)
                ++clippedSamples;

            if (channel == 0)
                peakLeft = std::max(peakLeft, magnitude);
            else if (channel == 1)
                peakRight = std::max(peakRight, magnitude);
        }
    }

    if (numChannels == 1)
        peakRight = peakLeft;

    renderPeakLeft.store(peakLeft, std::memory_order_relaxed);
    renderPeakRight.store(peakRight, std::memory_order_relaxed);
    if (std::max(peakLeft, peakRight) > 1.0e-7f)
        renderedAudibleState.store(true, std::memory_order_relaxed);
    sessionMetrics.recordAudioSummary(numSamples,
                                      clippedSamples,
                                      nonFiniteSamples);
}

void RenderEngine::processFx(fad::engine::AudioBufferView audioBuffer, bool diagEnabled)
{
    if (diagEnabled)
    {
        fxChain.processWithDiagnostics(audioBuffer, fxCpuMeters, clipDetector);
    }
    else
    {
        fxChain.process(audioBuffer);
    }
}

void RenderEngine::setDiagnosticsEnabled(bool enabled) noexcept
{
    collectGarbage();
    diagnosticsEnabled.store(enabled, std::memory_order_relaxed);
    diagnosticsRefreshCountdown.store(0, std::memory_order_relaxed);
    // Audio thread will handle clearing diagnosticsSnapshot and clipDetector
    // when it reads diagnosticsEnabled as false in render().
}

diagnostics::DiagnosticsSnapshot RenderEngine::getDiagnosticsSnapshot() const noexcept
{
    collectGarbage();

    for (int attempt = 0; attempt < 128; ++attempt)
    {
        const auto readIndex = diagnosticsReadIndex.load(std::memory_order_acquire);
        auto& slotState = diagnosticsSlotStates[static_cast<size_t>(readIndex)];
        auto readerCount = slotState.load(std::memory_order_acquire);
        while (readerCount >= 0)
        {
            if (slotState.compare_exchange_weak(readerCount,
                                                readerCount + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire))
            {
                if (diagnosticsReadIndex.load(std::memory_order_acquire) == readIndex)
                {
                    const auto snapshot = diagnosticsSnapshots[static_cast<size_t>(readIndex)];
                    slotState.fetch_sub(1, std::memory_order_release);
                    return snapshot;
                }

                slotState.fetch_sub(1, std::memory_order_release);
                break;
            }
        }

        std::this_thread::yield();
    }

    return {};
}

int RenderEngine::tryClaimDiagnosticsWriteSlot() noexcept
{
    return tryClaimSnapshotWriteSlot(diagnosticsSlotStates, diagnosticsReadIndex);
}

int RenderEngine::tryClaimSnapshotWriteSlot(std::array<std::atomic<int>, 3>& states,
                                          const std::atomic<int>& publishedIndex) noexcept
{
    const auto readIndex = publishedIndex.load(std::memory_order_acquire);
    for (size_t offset = 1; offset < diagnosticsSnapshotSlotCount; ++offset)
    {
        const auto candidate = static_cast<int>(
            (static_cast<size_t>(readIndex) + offset) % diagnosticsSnapshotSlotCount);
        auto expected = 0;
        if (states[static_cast<size_t>(candidate)].compare_exchange_strong(
                expected,
                diagnosticsWriterClaim,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return candidate;
        }
    }

    return -1;
}

void RenderEngine::publishDiagnosticsWriteSlot(int slotIndex) noexcept
{
    assert(slotIndex >= 0
            && static_cast<size_t>(slotIndex) < diagnosticsSnapshotSlotCount);
    diagnosticsReadIndex.store(slotIndex, std::memory_order_release);
    diagnosticsSlotStates[static_cast<size_t>(slotIndex)].store(0,
                                                                std::memory_order_release);
}

void RenderEngine::buildDiagnosticsSnapshot(fad::engine::AudioBufferView audioBuffer)
{
    (void) (audioBuffer);

    const auto writeIndex = tryClaimDiagnosticsWriteSlot();
    if (writeIndex < 0)
        return;

    auto& snap = diagnosticsSnapshots[static_cast<size_t>(writeIndex)];

    snap.enabled = true;

    if (signalFlowDirty)
    {
        cachedSignalFlowSnapshot =
            diagnostics::buildSignalFlowGraph(parameterSnapshot, lastSampleRate, lastBlockSize);
        signalFlowDirty = false;
    }

    snap.signalFlow = cachedSignalFlowSnapshot;

    // Fill diagnostics activity into signal flow nodes. Timing is only
    // published when the meter has a non-RT timing source; otherwise the CPU
    // fields stay zero so the UI does not display synthetic microseconds.
    snap.signalFlow.cpuTimingAvailable = totalCpuMeter.hasTimingData();
    snap.signalFlow.totalBlockMicroseconds = snap.signalFlow.cpuTimingAvailable
        ? totalCpuMeter.getSmoothedMicroseconds()
        : 0.0f;

    auto fillNodeFromMeter = [&](diagnostics::ModuleId id, int fxIndex)
    {
        if (fxIndex < 0 || static_cast<size_t>(fxIndex) >= fxCpuMeters.size())
            return;
        auto& node = snap.signalFlow.nodes[static_cast<size_t>(static_cast<int>(id))];
        const auto& meter = fxCpuMeters[static_cast<size_t>(fxIndex)];
        node.processingObserved = meter.hasObservedProcessing();
        node.cpuMicroseconds = snap.signalFlow.cpuTimingAvailable
            ? meter.getSmoothedMicroseconds()
            : 0.0f;
        node.cpuPercent = 0.0f;
        if (snap.signalFlow.cpuTimingAvailable
            && snap.signalFlow.totalBlockMicroseconds > 0.0f)
        {
            node.cpuPercent = node.cpuMicroseconds
                / snap.signalFlow.totalBlockMicroseconds * 100.0f;
        }
        node.clipping = clipDetector.isClipping(id);
        node.peakLevel = clipDetector.getPeakLevel(id);
    };

    const auto accumulateIntoSemanticNode = [&](diagnostics::ModuleId semanticId,
                                                const diagnostics::ModuleNode& slotNode)
    {
        auto& semanticNode = snap.signalFlow.nodes[static_cast<size_t>(static_cast<int>(semanticId))];
        semanticNode.processingObserved =
            semanticNode.processingObserved || slotNode.processingObserved;
        semanticNode.cpuMicroseconds += slotNode.cpuMicroseconds;
        semanticNode.clipping = semanticNode.clipping || slotNode.clipping;
        semanticNode.peakLevel = std::max(semanticNode.peakLevel, slotNode.peakLevel);
        semanticNode.cpuPercent = 0.0f;
        if (snap.signalFlow.cpuTimingAvailable
            && snap.signalFlow.totalBlockMicroseconds > 0.0f)
        {
            semanticNode.cpuPercent = semanticNode.cpuMicroseconds
                / snap.signalFlow.totalBlockMicroseconds * 100.0f;
        }
    };

    std::array<std::pair<int, int>, parameters::ids::maxFxSlots> dynamicSlotOrder{};
    size_t dynamicSlotOrderCount = 0;
    const auto diagnosticFxSnapshot = hasAppliedDynamicFxSnapshot
        ? appliedDynamicFxSnapshot
        : buildCanonicalLegacyFxSnapshot(parameterSnapshot.fx);
    const auto clampedSlotCount = std::clamp(diagnosticFxSnapshot.slotCount, 0, static_cast<int>(diagnosticFxSnapshot.slots.size()));
    const auto dynamicFxProcessingRanks =
        parameters::buildDynamicFxProcessingRanks(diagnosticFxSnapshot, clampedSlotCount);
    for (int slotIndex = 0; slotIndex < clampedSlotCount; ++slotIndex)
    {
        dynamicSlotOrder[dynamicSlotOrderCount++] =
            {dynamicFxProcessingRanks[static_cast<size_t>(slotIndex)], slotIndex};
    }

    const bool useDynamicFxDiagnostics = diagnosticFxSnapshot.hasExplicitTopology;

    if (useDynamicFxDiagnostics)
    {
        for (size_t i = 1; i < dynamicSlotOrderCount; ++i)
        {
            const auto candidate = dynamicSlotOrder[i];
            size_t j = i;
            while (j > 0
                   && (dynamicSlotOrder[j - 1].first > candidate.first
                       || (dynamicSlotOrder[j - 1].first == candidate.first
                           && dynamicSlotOrder[j - 1].second > candidate.second)))
            {
                dynamicSlotOrder[j] = dynamicSlotOrder[j - 1];
                --j;
            }
            dynamicSlotOrder[j] = candidate;
        }

        for (int processingIndex = 0; processingIndex < static_cast<int>(dynamicSlotOrderCount); ++processingIndex)
        {
            const auto slotIndex = dynamicSlotOrder[static_cast<size_t>(processingIndex)].second;
            const auto& slot = diagnosticFxSnapshot.slots[static_cast<size_t>(slotIndex)];
            const auto type = fx::FxCatalog::sanitizeTypeValue(slot.type);
            if (! slot.enabled || type == fx::FxType::none)
                continue;

            const auto slotNodeId = diagnostics::fxSlotModuleIdForIndex(slotIndex);
            if (! slotNodeId)
                continue;

            fillNodeFromMeter(*slotNodeId, processingIndex);

            auto& slotNode = snap.signalFlow.nodes[static_cast<size_t>(static_cast<int>(*slotNodeId))];
            if (slotNode.active && slotNode.semanticId != diagnostics::ModuleId::masterOutput)
                accumulateIntoSemanticNode(slotNode.semanticId, slotNode);
        }
    }
    else
    {
        fillNodeFromMeter(diagnostics::ModuleId::driveFx, 0);
        fillNodeFromMeter(diagnostics::ModuleId::chorusFx, 1);
        fillNodeFromMeter(diagnostics::ModuleId::delayFx, 2);
        fillNodeFromMeter(diagnostics::ModuleId::reverbFx, 3);
    }

    auto& masterNode = snap.signalFlow.nodes[
        static_cast<int>(diagnostics::ModuleId::masterOutput)];
    masterNode.processingObserved = totalCpuMeter.hasObservedProcessing();
    masterNode.clipping = clipDetector.isClipping(diagnostics::ModuleId::masterOutput);
    masterNode.peakLevel = clipDetector.getPeakLevel(diagnostics::ModuleId::masterOutput);

    // Grab modulation trace from the first active voice.
    // If no voice remains active by the end of this block, fall back to the
    // most recent non-empty trace so diagnostics still reflect the block that
    // just rendered.
    bool capturedTrace = false;
    for (auto* synthVoice : voiceCache)
    {
        if (synthVoice->isVoiceActive())
        {
            snap.modulationTrace = synthVoice->getLastModulationTrace();
            capturedTrace = true;
            break;
        }
    }

    if (! capturedTrace)
    {
        for (auto* synthVoice : voiceCache)
        {
            const auto& trace = synthVoice->getLastModulationTrace();
            if (std::any_of(trace.slots.begin(), trace.slots.end(),
                            [](const auto& slot) { return slot.active; }))
            {
                snap.modulationTrace = trace;
                break;
            }
        }
    }

    publishDiagnosticsWriteSlot(writeIndex);
}





wavetable::RuntimeWavetableBank& RenderEngine::getWavetableBank() noexcept
{
    collectGarbage();
    return wavetableBank;
}

const wavetable::RuntimeWavetableBank& RenderEngine::getWavetableBank() const noexcept
{
    collectGarbage();
    return wavetableBank;
}

const wavetable::RuntimeWavetableBank& RenderEngine::peekWavetableBankForStateCapture() const noexcept
{
    return wavetableBank;
}

bool RenderEngine::hasNoisePublicationRetireCapacity() const noexcept
{
    const std::scoped_lock lock(deferredReleaseDrainMutex);
    return deferredReleaseFifo.getFreeSpace() > 0;
}

size_t RenderEngine::getNoiseRetiredAssetBytes() const noexcept
{
    const std::scoped_lock lock(deferredReleaseDrainMutex);
    size_t bytes = 0;
    // Local Noise publications only enter this bounded primary queue. Shared
    // assets in multiple ownerships are conservatively charged more than once.
    for (const auto& entry : deferredReleaseBin)
        if (entry.object != nullptr) bytes += entry.noiseAssetBytes;
    return bytes;
}

bool RenderEngine::publishNoiseSampleSlot(
    int slotIndex, std::shared_ptr<const wavetable::WavetableData> asset)
{
    const std::scoped_lock lock(deferredReleaseDrainMutex);
    if (deferredReleaseFifo.getFreeSpace() == 0)
        return false;
    size_t previousBytes = 0;
    const auto previous = wavetableBank.getPublishedSnapshot();
    for (const auto& asset : previous.userTableOwners)
    {
        if (asset == nullptr) continue;
        for (const auto& frame : asset->frames) previousBytes += frame.size() * sizeof(float);
        for (const auto& level : asset->mipLevels) previousBytes += level.size() * sizeof(float);
        previousBytes += (asset->phaseRemap.size() + asset->curveEndpoints.size()
            + asset->curveLoopStarts.size()) * sizeof(float);
        for (const auto& curve : asset->nativeCurves.curves)
            previousBytes += curve.points.size() * sizeof(modulation::NativeLfoCurveData::Point);
    }
    wavetable::RuntimeWavetableBank::CommitRetireList retired;
    if (!wavetableBank.publishNoiseSampleSlot(slotIndex, std::move(asset), retired))
        return false;
    // Capacity and publication share this existing retirement lock. No other
    // producer or collector can consume the reserved entry in between.
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    deferredReleaseFifo.prepareToWrite(1, start1, size1, start2, size2);
    auto& destination = deferredReleaseBin[static_cast<size_t>(size1 > 0 ? start1 : start2)];
    auto& source = retired.retiredObjects[0];
    destination.object = std::move(source.object);
    destination.activeAudioReaders = source.activeAudioReaders;
    destination.pendingPublishedOwnershipReaders = source.pendingPublishedOwnershipReaders;
    destination.noiseAssetBytes = previousBytes;
    deferredReleaseFifo.finishedWrite(1);
    ensureDeferredReleaseWorkerStarted();
    deferredReleaseWorkAvailable.signal();
    return true;
}



bool RenderEngine::commitQueuedWavetableTransaction() noexcept
{
    wavetable::RuntimeWavetableBank::CommitRetireList retireList;
    const bool committed = wavetableBank.commitPendingTransaction(&retireList);
    adoptWavetableRetireList(std::move(retireList));
    if (committed)
        requestReset();
    return committed;
}

void RenderEngine::adoptWavetableRetireList(
    wavetable::RuntimeWavetableBank::CommitRetireList retireList) noexcept
{
    for (int i = 0; i < retireList.count; ++i)
    {
        auto& retired = retireList.retiredObjects[static_cast<size_t>(i)];
        deferRelease(std::move(retired.object),
                     retired.activeAudioReaders,
                     retired.pendingPublishedOwnershipReaders);
    }
}







void RenderEngine::releaseBlockWavetableOwnership(bool forceRelease) noexcept
{
    const auto* restoreGate = stateRestoreGate.load(std::memory_order_acquire);
    if (! forceRelease
        && restoreGate != nullptr
        && restoreGate->load(std::memory_order_acquire))
    {
        return;
    }

    // With a configured bulk gate, sample-backed Noise keeps its accepted
    // cohort across callbacks: the gate can start between release and resolve.
    // Ungated Engine callers retain their existing block ownership lifecycle.
    // The next accepted snapshot (or reset) releases this existing reader pin.
    if (! forceRelease && restoreGate != nullptr && parameterSnapshot.noise.sampleSlot > 0)
        return;

    if (forceRelease)
    {
        fxNoisePlayer.releaseAssetViews();
        for (auto* voice : voiceCache) voice->releaseNoiseAssetViews();
    }
    if (blockPublishedOwnership == nullptr)
    {
        blockWavetablePointers = { nullptr, nullptr };
        blockLfoCurveBanks.fill(nullptr);
        blockNoiseSample = nullptr;
        return;
    }

    auto* previousOwnership = std::exchange(blockPublishedOwnership, nullptr);
    previousOwnership->audioReaderCount.fetch_sub(1, std::memory_order_acq_rel);
    blockWavetablePointers = { nullptr, nullptr };
    blockLfoCurveBanks.fill(nullptr);
}

bool RenderEngine::hasDeferredReleaseWorkPending() const noexcept
{
    const std::scoped_lock lock(deferredReleaseDrainMutex);
    return deferredReleaseFifo.getNumReady() > 0
           || deferredReleaseOverflowFifo.getNumReady() > 0
           || deferredReleaseQuarantineFifo.getNumReady() > 0
           || deferredReleaseEmergencyFifo.getNumReady() > 0
           || ! deferredReleaseSpillover.empty();
}

void RenderEngine::deferRelease(std::shared_ptr<const void> ptr,
                               const std::atomic<uint32_t>* activeAudioReaders,
                               const std::atomic<uint32_t>* pendingPublishedOwnershipReaders) noexcept
{
    if (! ptr)
        return;

    const std::scoped_lock lock(deferredReleaseDrainMutex);
    ensureDeferredReleaseWorkerStarted();

    auto enqueueIntoFifo = [](auto& fifo,
                              auto& storage,
                              const std::shared_ptr<const void>& object,
                              const std::atomic<uint32_t>* readerGuard,
                              const std::atomic<uint32_t>* pendingReaderGuard) noexcept -> bool
    {
        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            auto& entry = storage[static_cast<size_t>(start1)];
            entry.object = std::move(object);
            entry.activeAudioReaders = readerGuard;
            entry.pendingPublishedOwnershipReaders = pendingReaderGuard;
            fifo.finishedWrite(1);
            return true;
        }

        if (size2 > 0)
        {
            auto& entry = storage[static_cast<size_t>(start2)];
            entry.object = std::move(object);
            entry.activeAudioReaders = readerGuard;
            entry.pendingPublishedOwnershipReaders = pendingReaderGuard;
            fifo.finishedWrite(1);
            return true;
        }

        return false;
    };

    const bool enqueued =
        enqueueIntoFifo(deferredReleaseFifo,
                        deferredReleaseBin,
                        ptr,
                        activeAudioReaders,
                        pendingPublishedOwnershipReaders)
        || enqueueIntoFifo(deferredReleaseOverflowFifo,
                           deferredReleaseOverflowBin,
                           ptr,
                           activeAudioReaders,
                           pendingPublishedOwnershipReaders)
        || enqueueIntoFifo(deferredReleaseQuarantineFifo,
                           deferredReleaseQuarantineBin,
                           ptr,
                           activeAudioReaders,
                           pendingPublishedOwnershipReaders);

    if (enqueued)
    {
        deferredReleaseWorkAvailable.signal();
        return;
    }

    if (enqueueIntoFifo(deferredReleaseEmergencyFifo,
                        deferredReleaseEmergencyBin,
                        ptr,
                        activeAudioReaders,
                        pendingPublishedOwnershipReaders))
    {
        deferredReleaseWorkAvailable.signal();
        return;
    }

    DeferredReleaseEntry spilloverEntry;
    spilloverEntry.object = std::move(ptr);
    spilloverEntry.activeAudioReaders = activeAudioReaders;
    spilloverEntry.pendingPublishedOwnershipReaders = pendingPublishedOwnershipReaders;
    deferredReleaseSpillover.push_back(std::move(spilloverEntry));
    deferredReleaseSpilloverCount.fetch_add(1, std::memory_order_release);
    deferredReleaseWorkAvailable.signal();
}

bool RenderEngine::collectGarbage() const noexcept
{
    const std::scoped_lock lock(deferredReleaseDrainMutex);

    auto entryBlocked = [this](const DeferredReleaseEntry& entry) noexcept
    {
        uint32_t pendingOwnershipReaders = 0;
        if (entry.pendingPublishedOwnershipReaders != nullptr)
        {
            // Sample the bank-wide resolver guard first. Together with the
            // seq_cst raw-pointer publication/resolution operations, a zero
            // here means a resolver that starts later must observe the new
            // ownership. A resolver already transferring its pin is either
            // visible here or in activeAudioReaders below.
            pendingOwnershipReaders =
                entry.pendingPublishedOwnershipReaders->load(std::memory_order_seq_cst);

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
            if (deferredReleaseFirstGuardSampleHook)
                deferredReleaseFirstGuardSampleHook();
#endif
        }

        if (pendingOwnershipReaders > 0)
            return true;

        if (entry.activeAudioReaders != nullptr
            && entry.activeAudioReaders->load(std::memory_order_acquire) > 0)
            return true;

        return false;
    };

    auto rotateToBack = [](auto& fifo,
                           auto& storage,
                           DeferredReleaseEntry&& entry) noexcept
    {
        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            storage[static_cast<size_t>(start1)] = std::move(entry);
            fifo.finishedWrite(1);
            return true;
        }

        if (size2 > 0)
        {
            storage[static_cast<size_t>(start2)] = std::move(entry);
            fifo.finishedWrite(1);
            return true;
        }

        return false;
    };

    auto drainFifo = [&](auto& fifo, auto& storage) noexcept
    {
        bool blocked = false;
        const int entriesToInspect = fifo.getNumReady();

        for (int inspected = 0; inspected < entriesToInspect; ++inspected)
        {
            int start1 = 0;
            int size1 = 0;
            int start2 = 0;
            int size2 = 0;
            fifo.prepareToRead(1, start1, size1, start2, size2);

            const int index = size1 > 0 ? start1 : (size2 > 0 ? start2 : -1);
            if (index < 0)
            {
                fifo.finishedRead(0);
                break;
            }

            auto& entry = storage[static_cast<size_t>(index)];
            if (entryBlocked(entry))
            {
                blocked = true;
                auto blockedEntry = std::move(entry);
                entry.reset();
                fifo.finishedRead(1);

                if (deferredReleaseRotateHook)
                    deferredReleaseRotateHook();

                const bool rotated = rotateToBack(fifo, storage, std::move(blockedEntry));
                assert(rotated);
                if (! rotated)
                    return true;
                continue;
            }

            entry.reset();
            fifo.finishedRead(1);
        }

        return blocked;
    };

    const bool blockedPrimary = drainFifo(deferredReleaseFifo, deferredReleaseBin);
    const bool blockedOverflow = drainFifo(deferredReleaseOverflowFifo, deferredReleaseOverflowBin);
    const bool blockedQuarantine = drainFifo(deferredReleaseQuarantineFifo, deferredReleaseQuarantineBin);
    const bool blockedEmergency = drainFifo(deferredReleaseEmergencyFifo, deferredReleaseEmergencyBin);
    bool blockedSpillover = false;
    size_t spilloverWriteIndex = 0;
    for (size_t spilloverReadIndex = 0; spilloverReadIndex < deferredReleaseSpillover.size(); ++spilloverReadIndex)
    {
        auto& entry = deferredReleaseSpillover[spilloverReadIndex];
        if (entryBlocked(entry))
        {
            blockedSpillover = true;
            if (spilloverWriteIndex != spilloverReadIndex)
                deferredReleaseSpillover[spilloverWriteIndex] = std::move(entry);
            ++spilloverWriteIndex;
            continue;
        }

        entry.reset();
    }

    for (size_t staleIndex = spilloverWriteIndex; staleIndex < deferredReleaseSpillover.size(); ++staleIndex)
        deferredReleaseSpillover[staleIndex].reset();
    deferredReleaseSpillover.resize(spilloverWriteIndex);

    const bool blocked = blockedPrimary
        || blockedOverflow
        || blockedQuarantine
        || blockedEmergency
        || blockedSpillover;

    return blocked;
}




}
