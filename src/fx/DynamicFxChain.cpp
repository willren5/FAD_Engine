#include <fad/engine/fx/DynamicFxChain.h>
#include <fad/engine/fx/WideFxControls.h>
#include <fad/engine/fx/Eq3BandFx.h>
#include <fad/engine/math/DspSafetyUtils.h>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/Numeric.h>
#include <fad/engine/detail/Scope.h>
#include <fad/engine/rt/ScopedNoDenormals.h>
#include <fad/engine/fx/FxTail.h>
#include <algorithm>
#include <cassert>

#include <bit>
#include <limits>

namespace fadmodel1::dsp::fx
{
namespace
{
constexpr int maxDesiredSnapshotReadAttempts = 16;
constexpr int maxReadableConfigAcquireRetries = 4;
constexpr uint64_t configEpochWritingBit = 1u;
constexpr double slotTypeSwitchFadeSeconds = 0.003;
// Bounds publication and parallel-lane histories. Higher-latency FX must also
// increase this bound; the current latency-bearing effect is the 5 ms Limiter.
constexpr double maximumLatencyPerSlotSeconds = 0.005;
constexpr int maximumLatencyBearingSlotCount = parameters::ids::maxFxSlots;
constexpr double retiredTailSilenceThreshold = 1.0e-5;
constexpr double maximumRetiredTailSeconds = 1800.0;
constexpr double maximumReverbNetworkDelaySeconds = 0.18;
constexpr double delayTimeSmoothingSeconds = 0.04;
constexpr unsigned wetTailModulation = 1u;
constexpr unsigned reverbTimeModulation = 2u;

bool isReadableConfigEpoch(uint64_t epoch) noexcept
{
    return (epoch & configEpochWritingBit) == 0u;
}

bool nearlyEqual(float lhs, float rhs) noexcept
{
    return std::abs(lhs - rhs) <= 1.0e-6f;
}

int slotTypeSwitchFadeSamples(const fad::engine::ProcessSpec& spec) noexcept
{
    const auto sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    return std::clamp(static_cast<int>(std::ceil(sampleRate * slotTypeSwitchFadeSeconds)), 1, 4096);
}

double estimateFeedbackTailSeconds(double cycleSeconds, double feedback) noexcept
{
    const auto safeCycle = std::clamp(cycleSeconds, 0.0, 60.0);
    const auto safeFeedback = std::clamp(feedback, 0.0, 0.95);
    if (safeCycle <= 0.0 || safeFeedback <= retiredTailSilenceThreshold)
        return safeCycle;

    return safeCycle * std::ceil(std::log(retiredTailSilenceThreshold)
                                 / std::log(safeFeedback));
}

double delayIndexToBeats(int index) noexcept
{
    static constexpr std::array<double, 9> beatValues{
        0.125,
        1.0 / 6.0,
        0.25,
        1.0 / 3.0,
        0.5,
        2.0 / 3.0,
        1.0,
        2.0,
        4.0};
    return beatValues[static_cast<size_t>(std::clamp(index, 0, 8))];
}

std::array<int, parameters::ids::maxFxSlots> sanitizeSlotOrder(const std::array<int, parameters::ids::maxFxSlots>& order, int activeSlotCount) noexcept
{
    std::array<int, parameters::ids::maxFxSlots> sanitized{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::array<bool, parameters::ids::maxFxSlots> used{};
    const auto clampedCount = std::clamp(activeSlotCount, 0, parameters::ids::maxFxSlots);
    int writeIndex = 0;

    for (int i = 0; i < clampedCount; ++i)
    {
        const auto slotIndex = order[static_cast<size_t>(i)];
        if (slotIndex < 0 || slotIndex >= clampedCount || used[static_cast<size_t>(slotIndex)])
            continue;

        sanitized[static_cast<size_t>(writeIndex++)] = slotIndex;
        used[static_cast<size_t>(slotIndex)] = true;
    }

    for (int slotIndex = 0; slotIndex < clampedCount; ++slotIndex)
    {
        if (used[static_cast<size_t>(slotIndex)])
            continue;
        sanitized[static_cast<size_t>(writeIndex++)] = slotIndex;
    }

    for (int slotIndex = clampedCount; slotIndex < parameters::ids::maxFxSlots; ++slotIndex)
        sanitized[static_cast<size_t>(slotIndex)] = slotIndex;

    return sanitized;
}


float paramAt(const float* params, size_t count, size_t index) noexcept
{
    return params != nullptr && index < count ? params[index] : 0.0f;
}

bool shouldBypassSlot(FxType type, bool enabled, const float* params, size_t paramCount) noexcept
{
    if (! enabled || type == FxType::none)
        return true;

    switch (type)
    {
        case FxType::none:
            return true;

        case FxType::drive:
            return paramCount < 3
                || (paramAt(params, paramCount, 2) <= 1.0e-4f
                    && paramAt(params, paramCount, 0) <= 1.0e-4f);

        case FxType::chorus:
        case FxType::delay:
        case FxType::reverb:
            (void) (params, paramCount);
            return false;

        case FxType::eq3Band:
        case FxType::compressor:
        case FxType::phaser:
        case FxType::flanger:
        case FxType::distortion:
        case FxType::bitcrusher:
        case FxType::filterFx:
        case FxType::stereo:
        case FxType::limiter:
        default:
            return false;
    }
}

void sanitizeBlock(fad::engine::AudioBlock<float>& block) noexcept
{
    for (size_t channel = 0; channel < block.getNumChannels(); ++channel)
    {
        auto* samples = block.getChannelPointer(channel);
        for (size_t sample = 0; sample < block.getNumSamples(); ++sample)
        {
            auto value = samples[sample];
            fad::engine::math::snapSmallState(value);
            samples[sample] = utils::sanitizeFinite(value);
        }
    }

}

float measurePeakAbs(const fad::engine::AudioBlock<float>& block, int numSamples) noexcept
{
    float peak = 0.0f;
    const auto samplesToMeasure = std::clamp(numSamples, 0, static_cast<int>(block.getNumSamples()));
    for (size_t channel = 0; channel < block.getNumChannels(); ++channel)
    {
        const auto* samples = block.getChannelPointer(channel);
        for (int sample = 0; sample < samplesToMeasure; ++sample)
            peak = std::max(peak, std::abs(samples[sample]));
    }
    return peak;
}
}

BaseFxProcessorCore* DynamicFxChainCore::ChainConfig::Slot::getProcessor(FxType requestedType) noexcept
{
    return requestedType != FxType::none && requestedType == processorType
        ? processor.get()
        : nullptr;
}

BaseFxProcessorCore* DynamicFxChainCore::ChainConfig::Slot::getProcessor(FxType requestedType) const noexcept
{
    return requestedType != FxType::none && requestedType == processorType
        ? processor.get()
        : nullptr;
}

DynamicFxChainCore::DynamicFxChainCore()
{
    for (auto& readerCount : configReaders)
        readerCount.store(0, std::memory_order_relaxed);
    for (auto& epoch : configEpochs)
        epoch.store(0, std::memory_order_relaxed);
    for (size_t slotIndex = 0; slotIndex < desiredTypes.size(); ++slotIndex)
    {
        desiredTypes[slotIndex].store(static_cast<int>(FxType::none), std::memory_order_relaxed);
        desiredEnabled[slotIndex].store(true, std::memory_order_relaxed);
        desiredOrder[slotIndex].store(static_cast<int>(slotIndex), std::memory_order_relaxed);
        desiredRoutingStages[slotIndex].store(static_cast<int>(slotIndex), std::memory_order_relaxed);
        desiredRoutingLanes[slotIndex].store(0, std::memory_order_relaxed);
        cachedParamCount[slotIndex].store(0, std::memory_order_relaxed);
        for (auto& paramBits : cachedParamBits[slotIndex])
            paramBits.store(encodeParamValue(0.0f), std::memory_order_relaxed);
    }
    for (auto& config : configs)
    {
        for (size_t slotIndex = 0; slotIndex < config.paramBits.size(); ++slotIndex)
        {
            config.paramCount[slotIndex].store(0, std::memory_order_relaxed);
            for (auto& paramBits : config.paramBits[slotIndex])
                paramBits.store(encodeParamValue(0.0f), std::memory_order_relaxed);
        }
    }
    initialiseProcessorBanks();
    configs[0].ready.store(true, std::memory_order_release);
}

DynamicFxChainCore::ScopedBatchUpdate::ScopedBatchUpdate(
    DynamicFxChainCore& owner,
    bool requireExplicitPublicationAcceptance) noexcept
    : chain(owner)
{
    chain.beginBatchUpdate(requireExplicitPublicationAcceptance);
}

DynamicFxChainCore::ScopedBatchUpdate::~ScopedBatchUpdate()
{
    chain.endBatchUpdate();
}

int DynamicFxChainCore::acquireReadableConfigIndex(bool requireActiveMatch) const noexcept
{
    for (int attempt = 0; attempt < maxReadableConfigAcquireRetries; ++attempt)
    {
        const auto acceptedSequence = acceptedStateSequence.load(std::memory_order_acquire);
        const auto tryAcquireConfig = [this, acceptedSequence](int candidate,
                                                               bool candidateMustMatchActive,
                                                               bool candidateMustMatchAccepted) noexcept -> int
        {
            const auto acquired = acquireSpecificReadableConfigIndex(candidate);
            if (acquired < 0)
                return -1;

            const auto& config = configs[static_cast<size_t>(acquired)];
            if ((! candidateMustMatchActive
                 || acquired == activeIndex.load(std::memory_order_acquire))
                && (! candidateMustMatchAccepted
                    || config.desiredStateSequence == acceptedSequence))
            {
                return acquired;
            }

            releaseReadableConfigIndex(acquired);
            return -1;
        };

        const auto activeCandidate = activeIndex.load(std::memory_order_acquire);
        if (requireActiveMatch)
        {
            if (const auto acquired = tryAcquireConfig(activeCandidate,
                                                       true,
                                                       false);
                acquired >= 0)
                return acquired;
            continue;
        }

        if (const auto acquired = tryAcquireConfig(activeCandidate,
                                                   false,
                                                   true);
            acquired >= 0)
            return acquired;

        const auto fadeCandidate = publicationFadeConfigIndex.load(std::memory_order_acquire);
        if (fadeCandidate != activeCandidate)
            if (const auto acquired = tryAcquireConfig(fadeCandidate,
                                                       false,
                                                       true);
                acquired >= 0)
                return acquired;

        for (int candidate = 0; candidate < static_cast<int>(configs.size()); ++candidate)
        {
            if (candidate == activeCandidate || candidate == fadeCandidate)
                continue;
            if (const auto acquired = tryAcquireConfig(candidate,
                                                       false,
                                                       true);
                acquired >= 0)
                return acquired;
        }
    }

    assert(false);
    return -1;
}

int DynamicFxChainCore::acquireSpecificReadableConfigIndex(int candidate) const noexcept
{
    if (candidate < 0 || candidate >= static_cast<int>(configs.size()))
        return -1;

    const auto candidateIndex = static_cast<size_t>(candidate);
    for (int attempt = 0; attempt < maxReadableConfigAcquireRetries; ++attempt)
    {
        const auto epochBefore = configEpochs[candidateIndex].load(std::memory_order_acquire);
        if (! isReadableConfigEpoch(epochBefore))
            continue;

        configReaders[candidateIndex].fetch_add(1, std::memory_order_acq_rel);
        const auto epochAfter = configEpochs[candidateIndex].load(std::memory_order_acquire);
        if (epochBefore == epochAfter
            && isReadableConfigEpoch(epochAfter)
            && configs[candidateIndex].ready.load(std::memory_order_acquire))
        {
            return candidate;
        }

        configReaders[candidateIndex].fetch_sub(1, std::memory_order_acq_rel);
    }

    return -1;
}

void DynamicFxChainCore::releaseReadableConfigIndex(int index) const noexcept
{
    if (index < 0 || index >= static_cast<int>(configReaders.size()))
        return;

    configReaders[static_cast<size_t>(index)].fetch_sub(1, std::memory_order_acq_rel);
}

DynamicFxChainCore::ReadableConfigGuard::ReadableConfigGuard(DynamicFxChainCore& owner,
                                                        bool requireActiveMatch) noexcept
    : chain(owner),
      index(chain.acquireReadableConfigIndex(requireActiveMatch))
{
}

DynamicFxChainCore::ReadableConfigGuard::~ReadableConfigGuard()
{
    chain.releaseReadableConfigIndex(index);
}

const DynamicFxChainCore::ChainConfig& DynamicFxChainCore::ReadableConfigGuard::get() const noexcept
{
    assert(isValid());
    return chain.configs[static_cast<size_t>(index)];
}

uint32_t DynamicFxChainCore::encodeParamValue(float value) noexcept
{
    return std::bit_cast<uint32_t>(value);
}

float DynamicFxChainCore::decodeParamValue(uint32_t bits) noexcept
{
    return std::bit_cast<float>(bits);
}

void DynamicFxChainCore::beginDesiredStateWrite() noexcept
{
    const auto previousWriters = desiredStateActiveWriters.fetch_add(1, std::memory_order_acq_rel);
    if (previousWriters == 0)
        desiredStateSequence.fetch_add(1, std::memory_order_acq_rel);
}

void DynamicFxChainCore::endDesiredStateWrite() noexcept
{
    const auto previousWriters = desiredStateActiveWriters.fetch_sub(1, std::memory_order_acq_rel);
    if (previousWriters == 1)
        desiredStateSequence.fetch_add(1, std::memory_order_release);
}

bool DynamicFxChainCore::copyDesiredStateSnapshot(DesiredStateSnapshot& snapshot) const noexcept
{
    for (int attempt = 0; attempt < maxDesiredSnapshotReadAttempts; ++attempt)
    {
        const auto startingSequence = desiredStateSequence.load(std::memory_order_acquire);
        if ((startingSequence & 1u) != 0u)
            return false;
        if (desiredStateActiveWriters.load(std::memory_order_acquire) != 0)
            return false;

        snapshot.slotCount = desiredSlotCount.load(std::memory_order_acquire);
        snapshot.orderCount = desiredOrderCount.load(std::memory_order_acquire);
        snapshot.requiresExplicitPublicationAcceptance =
            desiredPublicationRequiresAcceptance.load(std::memory_order_acquire);
        for (size_t slotIndex = 0; slotIndex < snapshot.types.size(); ++slotIndex)
        {
            snapshot.types[slotIndex] =
                static_cast<FxType>(desiredTypes[slotIndex].load(std::memory_order_acquire));
            snapshot.enabled[slotIndex] = desiredEnabled[slotIndex].load(std::memory_order_acquire);
            snapshot.order[slotIndex] = desiredOrder[slotIndex].load(std::memory_order_acquire);
            snapshot.routingStages[slotIndex] = desiredRoutingStages[slotIndex].load(std::memory_order_acquire);
            snapshot.routingLanes[slotIndex] = desiredRoutingLanes[slotIndex].load(std::memory_order_acquire);
            snapshot.paramCount[slotIndex] = cachedParamCount[slotIndex].load(std::memory_order_acquire);
            for (size_t paramIndex = 0; paramIndex < snapshot.params[slotIndex].size(); ++paramIndex)
            {
                snapshot.params[slotIndex][paramIndex] =
                    decodeParamValue(cachedParamBits[slotIndex][paramIndex].load(std::memory_order_acquire));
            }
        }

        const auto endingSequence = desiredStateSequence.load(std::memory_order_acquire);
        if (startingSequence == endingSequence
            && (endingSequence & 1u) == 0u
            && desiredStateActiveWriters.load(std::memory_order_acquire) == 0)
        {
            snapshot.sequence = endingSequence;
            return true;
        }
    }

    return false;
}

void DynamicFxChainCore::initialiseProcessorBanks()
{
    for (auto& config : configs)
    {
        for (auto& slot : config.slots)
        {
            slot.processor.reset();
            slot.processorType = FxType::none;
        }
    }
}

// ── prepare ────────────────────────────────────────────────────────
void DynamicFxChainCore::prepare(const fad::engine::ProcessSpec& spec)
{
    prepared = false;
    currentSpec = spec;
    const auto historyChannels = std::clamp(static_cast<int>(spec.numChannels), 1, 2);
    const auto historySamples = maximumPublicationAlignmentSamples(spec) + 1;
    for (auto& config : configs)
    {
        config.transitionHistoryBuffer.setSize(historyChannels,
                                               historySamples,
                                               false,
                                               false,
                                               true);
        resetTransitionHistory(config);
    }
    slotFadeScratchBuffer.setSize(
        historyChannels,
        slotTypeSwitchFadeSamples(spec),
        false,
        false,
        true);
    publicationFadeScratchBuffer.setSize(
        std::clamp(static_cast<int>(spec.numChannels), 1, 2),
        std::max(slotTypeSwitchFadeSamples(spec), static_cast<int>(spec.maximumBlockSize)),
        false,
        false,
        true);
    zeroChannelAdvanceBuffer.setSize(
        historyChannels,
        publicationFadeScratchBuffer.getNumSamples(),
        false,
        false,
        true);
    zeroChannelAdvanceBuffer.clear();
    for (auto* buffer : {&routingInputBuffer, &routingSumBuffer, &routingLaneBuffer})
        buffer->setSize(std::max(1, static_cast<int>(spec.numChannels)),
                        publicationFadeScratchBuffer.getNumSamples(), false, false, true);
    publicationFadeConfigIndex.store(-1, std::memory_order_release);
    publicationWarmupRemaining.store(0, std::memory_order_release);
    publicationFadeRemaining.store(0, std::memory_order_release);
    publicationFadeTotal.store(0, std::memory_order_release);
    publicationOldLatencySamples.store(0, std::memory_order_release);
    publicationNewLatencySamples.store(0, std::memory_order_release);
    publicationTailRemainingSamples.store(0, std::memory_order_release);
    publicationTailSilentSamples.store(0, std::memory_order_release);
    publicationTailSilenceWindowSamples.store(0, std::memory_order_release);
    publicationTailTransportBpm.store(120.0, std::memory_order_release);
    secondaryTailConfigIndex.store(-1, std::memory_order_release);
    secondaryTailRemainingSamples.store(0, std::memory_order_release);
    secondaryTailSilentSamples.store(0, std::memory_order_release);
    secondaryTailSilenceWindowSamples.store(0, std::memory_order_release);
    secondaryTailTransportBpm.store(120.0, std::memory_order_release);
    tailPromotionInProgress.store(false, std::memory_order_release);

    if (rebuildPending.load(std::memory_order_acquire))
        (void) (tryRebuildChain());

    prepared = true;

    // Prepare only processors required by the topology already published in each bank.
    for (auto& config : configs)
        for (auto& slot : config.slots)
            if (slot.processor != nullptr)
                slot.processor->prepare(spec);

    // Re-apply cached params after prepare so type switches stay deterministic.
    for (int i = 0; i < parameters::ids::maxFxSlots; ++i)
    {
        const auto slotIndex = static_cast<size_t>(i);
        const int numCached = cachedParamCount[slotIndex].load(std::memory_order_acquire);
        if (numCached <= 0)
            continue;

        for (auto& config : configs)
        {
            auto& slot = config.slots[slotIndex];
            auto* processor = slot.getProcessor(slot.type);
            if (processor == nullptr)
                continue;

            const auto paramCount = maxParamsPerSlot;
            for (int p = 0; p < std::min(numCached, paramCount); ++p)
            {
                processor->setGenericParam(
                    p,
                    decodeParamValue(cachedParamBits[slotIndex][static_cast<size_t>(p)].load(
                        std::memory_order_acquire)));
            }
        }
    }

    for (auto& config : configs)
        prepareRoutingHistory(config, true);
    {
        ReadableConfigGuard acceptedConfig(*this);
        if (acceptedConfig.isValid())
            publishedLatencySamples.store(configLatencySamples(acceptedConfig.get()),
                                          std::memory_order_release);
    }

    const auto candidate = preparedIndex.load(std::memory_order_acquire);
    if (candidate >= 0)
    {
        auto& config = configs[static_cast<size_t>(candidate)];
        config.preparedFadeSamples = config.preparedNeedsGraphFade
            ? slotTypeSwitchFadeSamples(spec) : 0;
        config.preparedOldLatency = configLatencySamples(
            configs[static_cast<size_t>(activeIndex.load(std::memory_order_acquire))]);
        config.preparedNewLatency = configLatencySamples(config);
        config.preparedTransitionLatency = std::max(config.preparedOldLatency, config.preparedNewLatency);
        int silenceWindow = 0;
        const auto tailSamples = estimateRetiredTailSamples(
            configs[static_cast<size_t>(activeIndex.load(std::memory_order_acquire))],
            spec, config.preparedTailBpm, silenceWindow);
        config.preparedTailSamples = tailSamples > 0
            ? tailSamples + config.preparedFadeSamples + config.preparedTransitionLatency : 0;
        config.preparedTailSilenceWindow = silenceWindow;
    }
    inactiveProcessorCleanupPending.store(true, std::memory_order_release);
    reclaimInactiveProcessorBanksOnControlThread();
    hasProcessedSincePrepare.store(false, std::memory_order_release);
}

// ── process ────────────────────────────────────────────────────────
void DynamicFxChainCore::process(fad::engine::ProcessContextReplacing<float>& context)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;

    if (bypassed.load(std::memory_order_relaxed))
        return;

    if (context.getOutputBlock().getNumSamples() == 0)
        return;

    if (! tryEnterProcessCallback())
    {
        rebuildPending.store(true, std::memory_order_release);
        return;
    }
    const auto leaveCallback = fad::engine::detail::ScopeExit{
        [this] { leaveProcessCallback(); }};

    hasProcessedSincePrepare.store(true, std::memory_order_release);

    if (context.getOutputBlock().getNumChannels() == 0)
    {
        advanceZeroChannelBlock(context.getOutputBlock().getNumSamples());
        return;
    }

    const auto scratchCapacity = publicationFadeScratchBuffer.getNumSamples();
    if (scratchCapacity > 0
        && context.getOutputBlock().getNumSamples()
            > static_cast<size_t>(scratchCapacity))
    {
        const auto* savedSources = nativeModulationSources;
        const auto savedCount = nativeModulationSourceCount;
        const auto restoreSources = fad::engine::detail::ScopeExit{[this, savedSources, savedCount] {
            nativeModulationSources = savedSources; nativeModulationSourceCount = savedCount;
        }};
        auto outputBlock = context.getOutputBlock();
        for (size_t offset = 0; offset < outputBlock.getNumSamples();
             offset += static_cast<size_t>(scratchCapacity))
        {
            auto subBlock = outputBlock.getSubBlock(
                offset,
                std::min(static_cast<size_t>(scratchCapacity), outputBlock.getNumSamples() - offset));
            nativeModulationSourceCount = std::max(0, savedCount - static_cast<int>(offset));
            nativeModulationSources = savedSources != nullptr && nativeModulationSourceCount > 0 ? savedSources + offset : nullptr;
            auto subContext = fad::engine::ProcessContextReplacing<float>(subBlock);
            process(subContext);
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
            if (offset + subBlock.getNumSamples() < outputBlock.getNumSamples()
                && oversizedChunkBoundaryHook)
            {
                oversizedChunkBoundaryHook();
            }
#endif
        }
        return;
    }

    ReadableConfigGuard readableConfig(*this);
    if (! readableConfig.isValid())
    {
        rebuildPending.store(true, std::memory_order_release);
        return;
    }

    const auto& config = readableConfig.get();
    auto outputBlock = context.getOutputBlock();
    sanitizeBlock(outputBlock);
    const auto currentTransportBpm = transportBpm.load(std::memory_order_acquire);
    int publicationTransitionSamples = 0;
    int publicationWarmupRemainingBefore = 0;
    int publicationFadeRemainingBefore = 0;
    int publicationFadeTotalBefore = 0;
    const auto fadeSourceConfigIndex =
        preparePublicationFadeScratch(readableConfig.getIndex(),
                                      outputBlock,
                                      publicationTransitionSamples,
                                      publicationWarmupRemainingBefore,
                                      publicationFadeRemainingBefore,
                                      publicationFadeTotalBefore);

    processConfigSlots(config, context, outputBlock, currentTransportBpm);
    if (fadeSourceConfigIndex >= 0)
    {
        renderAndBlendPublicationFade(readableConfig.getIndex(),
                                      fadeSourceConfigIndex,
                                      publicationTransitionSamples,
                                      publicationWarmupRemainingBefore,
                                      publicationFadeRemainingBefore,
                                      publicationFadeTotalBefore,
                                      outputBlock,
                                      currentTransportBpm);
    }
    else
    {
        applyTransitionDelay(config, outputBlock, 0, true);
        renderRetiredTail(readableConfig.getIndex(), outputBlock);
    }
    renderSecondaryRetiredTail(readableConfig.getIndex(), outputBlock);
    sanitizeBlock(outputBlock);
}

std::array<float, parameters::ids::maxFxSlots> DynamicFxChainCore::getGainReductionDbForUi() noexcept
{
    std::array<float, parameters::ids::maxFxSlots> values;
    values.fill(-1.0f);
    ReadableConfigGuard guard(*this, true);
    if (!guard.isValid()) return values;
    const auto& config = guard.get();
    for (int index = 0; index < config.activeSlotCount; ++index)
    {
        const auto& slot = config.slots[static_cast<size_t>(index)];
        if (slot.type != FxType::compressor || slot.processor == nullptr) continue;
        values[static_cast<size_t>(index)] = !slot.enabled || bypassed.load(std::memory_order_relaxed)
            ? 0.0f : slot.processor->getGainReductionDbForUi();
    }
    return values;
}

bool DynamicFxChainCore::tryEnterProcessCallback() noexcept
{
    // Writers mutate only banks excluded by per-bank reader/epoch checks and
    // publish the ready candidate atomically. Keep the currently audible bank
    // rendering across that work instead of returning an untreated dry block.
    processCallbackReaders.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void DynamicFxChainCore::leaveProcessCallback() noexcept
{
    processCallbackReaders.fetch_sub(1, std::memory_order_acq_rel);
}

void DynamicFxChainCore::advanceZeroChannelBlock(size_t numSamples)
{
    const auto capacity = zeroChannelAdvanceBuffer.getNumSamples();
    if (capacity <= 0)
        return;

    for (size_t offset = 0; offset < numSamples; offset += static_cast<size_t>(capacity))
    {
        const auto chunkSamples = std::min(static_cast<size_t>(capacity), numSamples - offset);
        zeroChannelAdvanceBuffer.clear();
        auto discardBlock = fad::engine::AudioBlock<float>(zeroChannelAdvanceBuffer)
            .getSubBlock(0, chunkSamples);
        auto discardContext = fad::engine::ProcessContextReplacing<float>(discardBlock);
        process(discardContext);
    }
}

// ── reset ──────────────────────────────────────────────────────────
void DynamicFxChainCore::reset()
{
    // A reset is a graph-wide audio boundary.  Pin each bank before touching
    // processor state; resetting only the active bank leaves a retired fade/tail
    // bank capable of resurrecting stale audio after the next publication.
    finishPublicationRetirement();
    finishSecondaryRetirement();
    for (int candidate = 0; candidate < static_cast<int>(configs.size()); ++candidate)
    {
        const auto acquired = acquireSpecificReadableConfigIndex(candidate);
        if (acquired < 0)
            continue;

        const auto releaseConfig = fad::engine::detail::ScopeExit{[this, acquired]
        {
            releaseReadableConfigIndex(acquired);
        }};
        auto& config = configs[static_cast<size_t>(acquired)];
        for (auto& slot : config.slots)
        {
            slot.tailModulationSeen.store(0, std::memory_order_relaxed);
            if (slot.processor != nullptr)
                slot.processor->reset();
        }
        resetTransitionHistory(config);
        config.routingHistoryPosition.fill(0);
        for (auto& history : config.routingHistory) history.clear();
    }
}

// ── setBypassed ────────────────────────────────────────────────────
void DynamicFxChainCore::setBypassed(bool shouldBeBypassed)
{
    bypassed.store(shouldBeBypassed, std::memory_order_relaxed);
}

// ── setGenericParam (no-op at chain level) ─────────────────────────
void DynamicFxChainCore::setGenericParam(int /*index*/, float /*normalizedValue*/)
{
    // DynamicFxChainCore is a composite — generic params are set on individual
    // slot processors via setSlotParam(), not on the chain itself.
}

// ── setTransportBpm ────────────────────────────────────────────────
void DynamicFxChainCore::setTransportBpm(double bpm)
{
    transportBpm.store(std::clamp(std::isfinite(bpm) ? bpm : 120.0, 20.0, 400.0),
                       std::memory_order_release);
}

// ── setSlotCount ───────────────────────────────────────────────────
void DynamicFxChainCore::setSlotCount(int count)
{
    const auto clampedCount = std::clamp(count, 0, parameters::ids::maxFxSlots);
    if (desiredSlotCount.load(std::memory_order_acquire) == clampedCount)
        return;

    beginDesiredStateWrite();
    desiredSlotCount.store(clampedCount, std::memory_order_release);
    endDesiredStateWrite();

    requestRebuild();
}

// ── setSlotType ────────────────────────────────────────────────────
void DynamicFxChainCore::setSlotType(int slotIndex, FxType type)
{
    if (slotIndex < 0 || slotIndex >= parameters::ids::maxFxSlots)
        return;

    // Preset/AI/host state can cross this public boundary as an arbitrary enum
    // value. Normalize it before publishing a graph; otherwise tryRebuildChain
    // can mark an unknown type ready with no processor and silently drop audio.
    const auto sanitizedType = FxCatalog::sanitizeTypeValue(static_cast<int>(type));

    // Store the desired type; rebuildChain() will create/destroy processors
    // on the inactive config accordingly.
    const auto si = static_cast<size_t>(slotIndex);
    const auto currentType = static_cast<FxType>(desiredTypes[si].load(std::memory_order_acquire));
    if (currentType == sanitizedType)
        return;

    beginDesiredStateWrite();
    desiredTypes[si].store(static_cast<int>(sanitizedType), std::memory_order_release);

    // Clear cached params so the new FX type starts with clean defaults
    // instead of inheriting stale values from the previous type.
    for (auto& paramBits : cachedParamBits[si])
        paramBits.store(encodeParamValue(0.0f), std::memory_order_release);
    cachedParamCount[si].store(0, std::memory_order_release);
    endDesiredStateWrite();

    requestRebuild();
}

// ── setSlotOrder ───────────────────────────────────────────────────
void DynamicFxChainCore::setSlotOrder(const std::vector<int>& order)
{
    std::array<int, parameters::ids::maxFxSlots> fixedOrder{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto limit = std::min(static_cast<int>(order.size()), static_cast<int>(fixedOrder.size()));
    for (int index = 0; index < limit; ++index)
        fixedOrder[static_cast<size_t>(index)] = order[static_cast<size_t>(index)];

    setSlotOrder(fixedOrder, limit);
}

void DynamicFxChainCore::setSlotOrder(const std::array<int, parameters::ids::maxFxSlots>& order, int activeSlotCount)
{
    const auto clampedCount = std::clamp(activeSlotCount, 0, parameters::ids::maxFxSlots);
    const auto sanitizedOrder = sanitizeSlotOrder(order, clampedCount);
    bool orderMatches = desiredOrderCount.load(std::memory_order_acquire) == clampedCount;
    for (size_t index = 0; index < desiredOrder.size() && orderMatches; ++index)
        orderMatches = desiredOrder[index].load(std::memory_order_acquire) == sanitizedOrder[index];

    if (orderMatches)
        return;

    beginDesiredStateWrite();
    for (size_t index = 0; index < desiredOrder.size(); ++index)
        desiredOrder[index].store(sanitizedOrder[index], std::memory_order_release);
    desiredOrderCount.store(clampedCount, std::memory_order_release);
    endDesiredStateWrite();

    requestRebuild();
}

void DynamicFxChainCore::setSlotRouting(int slotIndex, int stage, int lane)
{
    if (!(slotIndex >= 0 && slotIndex < parameters::ids::maxFxSlots)) return;
    const auto index = static_cast<size_t>(slotIndex);
    stage = parameters::resolvedFxRoutingStage(slotIndex, stage);
    lane = std::clamp(lane, 0, parameters::ids::maxFxSlots - 1);
    if (desiredRoutingStages[index].load(std::memory_order_acquire) == stage
        && desiredRoutingLanes[index].load(std::memory_order_acquire) == lane) return;
    beginDesiredStateWrite();
    desiredRoutingStages[index].store(stage, std::memory_order_release);
    desiredRoutingLanes[index].store(lane, std::memory_order_release);
    endDesiredStateWrite();
    requestRebuild();
}

// ── setSlotEnabled ─────────────────────────────────────────────────
void DynamicFxChainCore::setSlotEnabled(int slotIndex, bool enabled)
{
    if (slotIndex < 0 || slotIndex >= parameters::ids::maxFxSlots)
        return;

    const auto si = static_cast<size_t>(slotIndex);
    if (desiredEnabled[si].load(std::memory_order_acquire) == enabled)
        return;

    beginDesiredStateWrite();
    desiredEnabled[si].store(enabled, std::memory_order_release);
    endDesiredStateWrite();

    requestRebuild();
}

// ── setSlotParam ───────────────────────────────────────────────────
void DynamicFxChainCore::setSlotParam(int slotIndex,
                                  int paramIndex,
                                  float value01,
                                  bool updateReadableActiveProcessor)
{
    if (slotIndex < 0 || slotIndex >= parameters::ids::maxFxSlots)
        return;
    if (paramIndex < 0 || paramIndex >= maxParamsPerSlot)
        return;

    const auto si = static_cast<size_t>(slotIndex);
    const auto pi = static_cast<size_t>(paramIndex);
    const auto clampedValue = std::clamp(utils::sanitizeFinite(value01), 0.0f, 1.0f);
    const auto currentParamCount = cachedParamCount[si].load(std::memory_order_acquire);
    const auto cachedValue = decodeParamValue(cachedParamBits[si][pi].load(std::memory_order_acquire));
    if (paramIndex < currentParamCount && nearlyEqual(cachedValue, clampedValue))
        return;

    beginDesiredStateWrite();
    cachedParamBits[si][pi].store(encodeParamValue(clampedValue), std::memory_order_release);
    if (paramIndex >= currentParamCount)
    {
        const int desiredParamCount = paramIndex + 1;
        int observedParamCount = currentParamCount;
        for (int attempt = 0;
             attempt < maxParamsPerSlot && observedParamCount < desiredParamCount;
             ++attempt)
        {
            if (cachedParamCount[si].compare_exchange_strong(observedParamCount,
                                                             desiredParamCount,
                                                             std::memory_order_acq_rel,
                                                             std::memory_order_acquire))
            {
                break;
            }
        }
    }
    endDesiredStateWrite();

    if (! updateReadableActiveProcessor)
    {
        requestRebuild();
        return;
    }

    const bool rebuildWasActive = rebuildWriterInProgress.load(std::memory_order_acquire);

    const auto desiredType = static_cast<FxType>(desiredTypes[si].load(std::memory_order_acquire));
    if (desiredType == FxType::none)
        return;

    const auto paramCount = FxCatalog::getParamCountForType(desiredType);
    if (paramIndex >= paramCount && paramIndex < parameters::ids::maxFxParamsPerSlot)
        return;

    std::array<float, maxParamsPerSlot> paramValues{};
    for (size_t index = 0; index < paramValues.size(); ++index)
        paramValues[index] =
            decodeParamValue(cachedParamBits[si][index].load(std::memory_order_acquire));
    const auto enabled = desiredEnabled[si].load(std::memory_order_acquire);

    ReadableConfigGuard readableConfig(*this, true);
    if (! readableConfig.isValid())
    {
        rebuildPending.store(true, std::memory_order_release);
        return;
    }

    const auto& activeConfig = readableConfig.get();
    activeConfig.paramBits[si][pi].store(encodeParamValue(clampedValue),
                                         std::memory_order_release);
    auto activeParamCount = activeConfig.paramCount[si].load(std::memory_order_acquire);
    for (int attempt = 0; attempt < maxParamsPerSlot && activeParamCount < paramIndex + 1; ++attempt)
    {
        if (activeConfig.paramCount[si].compare_exchange_weak(activeParamCount,
                                                               paramIndex + 1,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_acquire))
            break;
    }
    const auto effectiveParamCount =
        static_cast<size_t>(std::clamp(std::max(currentParamCount, paramIndex + 1), 0, paramCount));
    if (auto* processor = activeConfig.slots[si].getProcessor(desiredType); processor != nullptr)
    {
        processor->setGenericParam(paramIndex, clampedValue);
        processor->setBypassed(
            shouldBypassSlot(desiredType, enabled, paramValues.data(), effectiveParamCount));
    }

    if (rebuildWasActive || rebuildWriterInProgress.load(std::memory_order_acquire))
    {
        rebuildPending.store(true, std::memory_order_release);
    }
}

void DynamicFxChainCore::beginBatchUpdate(bool requireExplicitPublicationAcceptance) noexcept
{
    const int previousDepth = batchUpdateDepth.fetch_add(1, std::memory_order_acq_rel);
    if (previousDepth == 0)
    {
        beginDesiredStateWrite();
        desiredPublicationRequiresAcceptance.store(requireExplicitPublicationAcceptance,
                                                    std::memory_order_release);
    }
    else if (requireExplicitPublicationAcceptance)
    {
        desiredPublicationRequiresAcceptance.store(true, std::memory_order_release);
    }
}

void DynamicFxChainCore::endBatchUpdate() noexcept
{
    const int previousDepth = batchUpdateDepth.fetch_sub(1, std::memory_order_acq_rel);

    if (previousDepth <= 0)
    {
        assert(false);
        batchUpdateDepth.store(0, std::memory_order_release);
        return;
    }

    if (previousDepth == 1)
    {
        endDesiredStateWrite();
        // An explicit publication is a new acceptance token even if every
        // setter was a no-op. Publish that token on the control thread;
        // otherwise a restored synth snapshot waits forever for a sequence
        // that no readable bank contains. Do not rebuild on this caller.
        if (desiredPublicationRequiresAcceptance.load(std::memory_order_acquire))
            rebuildPending.store(true, std::memory_order_release);
    }
}

bool DynamicFxChainCore::tryAcceptLatestPreparedState() noexcept
{
    return tryAcceptPreparedState(getLatestDesiredStateSequence());
}

uint64_t DynamicFxChainCore::getLatestDesiredStateSequence() const noexcept
{
    return desiredStateSequence.load(std::memory_order_acquire);
}

bool DynamicFxChainCore::tryAcceptPreparedState(uint64_t expectedSequence) noexcept
{
    if (audioCallbackActive && ! audioAcceptanceOpen)
        return false;
    if (audioCallbackActive)
        audioAcceptanceOpen = false;
    if ((expectedSequence & 1u) != 0u
        || desiredStateActiveWriters.load(std::memory_order_acquire) != 0)
        return false;

    // A single bounded claim serializes acceptance with candidate preparation.
    // Failure keeps the complete accepted graph/snapshot for this callback.
    bool writerExpected = false;
    if (! rebuildWriterInProgress.compare_exchange_strong(
            writerExpected, true, std::memory_order_acq_rel, std::memory_order_acquire))
        return false;
    const auto releaseWriter = fad::engine::detail::ScopeExit{[this] {
        rebuildWriterInProgress.store(false, std::memory_order_release);
    }};
    if (expectedSequence != desiredStateSequence.load(std::memory_order_acquire)
        || desiredStateActiveWriters.load(std::memory_order_acquire) != 0)
        return false;

    const auto candidate = preparedIndex.load(std::memory_order_acquire);
    if (candidate < 0)
        return configs[static_cast<size_t>(activeIndex.load(std::memory_order_acquire))]
                   .desiredStateSequence == expectedSequence
            && acceptedStateSequence.load(std::memory_order_acquire) == expectedSequence;
    const auto& config = configs[static_cast<size_t>(candidate)];
    if (! config.ready.load(std::memory_order_acquire)
        || config.desiredStateSequence != expectedSequence
        || config.publicationPreviousIndex != activeIndex.load(std::memory_order_acquire)
        || ! publishPreparedConfig(candidate))
        return false;

    preparedIndex.store(-1, std::memory_order_release);
    if (expectedSequence == desiredStateSequence.load(std::memory_order_acquire))
        desiredPublicationRequiresAcceptance.store(false, std::memory_order_release);
    return true;
}

bool DynamicFxChainCore::tryRebuildChain() noexcept
{
    bool writerExpected = false;
    if (! rebuildWriterInProgress.compare_exchange_strong(writerExpected,
                                                          true,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
    {
        return false;
    }

    if (processCallbackReaders.load(std::memory_order_acquire) != 0)
    {
        rebuildPending.store(true, std::memory_order_release);
        rebuildWriterInProgress.store(false, std::memory_order_release);
        return false;
    }

    const auto hadPendingRequest = rebuildPending.exchange(false, std::memory_order_acq_rel);
    const auto releaseWriter = [this, hadPendingRequest](bool restorePending) noexcept
    {
        if (restorePending || hadPendingRequest)
            rebuildPending.store(true, std::memory_order_release);
        rebuildWriterInProgress.store(false, std::memory_order_release);
    };

    int active = activeIndex.load(std::memory_order_acquire);
    const auto fadeSource = publicationFadeConfigIndex.load(std::memory_order_acquire);
    const auto warmupRemaining = publicationWarmupRemaining.load(std::memory_order_acquire);
    const auto fadeRemaining = publicationFadeRemaining.load(std::memory_order_acquire);
    const auto tailRemaining = publicationTailRemainingSamples.load(std::memory_order_acquire);
    const auto secondaryTail = secondaryTailConfigIndex.load(std::memory_order_acquire);
    const auto secondaryTailRemaining =
        secondaryTailRemainingSamples.load(std::memory_order_acquire);

    int inactive = -1;
    if (warmupRemaining > 0 || fadeRemaining > 0)
    {
        releaseWriter(true);
        return false;
    }
    if (tailRemaining > 0)
    {
        const auto acceptedSequence = acceptedStateSequence.load(std::memory_order_acquire);
        const bool primaryTailIsValid = fadeSource >= 0
            && fadeSource < static_cast<int>(configs.size())
            && fadeSource != active
            && configs[static_cast<size_t>(fadeSource)].ready.load(std::memory_order_acquire);
        const bool activeIsAccepted = configs[static_cast<size_t>(active)].desiredStateSequence
            == acceptedSequence;
        if (! primaryTailIsValid
            || ! activeIsAccepted
            || (secondaryTail >= 0 && secondaryTailRemaining > 0))
        {
            releaseWriter(true);
            return false;
        }
    }

    const auto acceptedSequence = acceptedStateSequence.load(std::memory_order_acquire);
    const auto activeSequence = configs[static_cast<size_t>(active)].desiredStateSequence;
    for (int candidate = 0;
         inactive < 0 && candidate < static_cast<int>(configs.size());
         ++candidate)
    {
        if (candidate == active
            || candidate == fadeSource
            || (secondaryTailRemaining > 0 && candidate == secondaryTail))
            continue;

        // Keep an explicitly accepted baseline bank intact while a preview
        // graph is still awaiting publication acceptance.
        const auto& candidateConfig = configs[static_cast<size_t>(candidate)];
        if (activeSequence != acceptedSequence
            && candidateConfig.ready.load(std::memory_order_acquire)
            && candidateConfig.desiredStateSequence == acceptedSequence)
            continue;

        if (configReaders[static_cast<size_t>(candidate)].load(std::memory_order_acquire) == 0)
        {
            inactive = candidate;
            break;
        }
    }

    if (inactive < 0)
    {
        releaseWriter(true);
        return false;
    }

    const auto inactiveIndex = static_cast<size_t>(inactive);
    if (configReaders[inactiveIndex].load(std::memory_order_acquire) > 0)
    {
        releaseWriter(true);
        return false;
    }

    auto& config = configs[inactiveIndex];
    DesiredStateSnapshot desiredSnapshot;
    if (! copyDesiredStateSnapshot(desiredSnapshot))
    {
        releaseWriter(true);
        return false;
    }

    const auto writeEpoch = configEpochs[inactiveIndex].fetch_add(1, std::memory_order_acq_rel) + 1u;
    assert(! isReadableConfigEpoch(writeEpoch));
    (void) (writeEpoch);
    if (configReaders[inactiveIndex].load(std::memory_order_acquire) > 0)
    {
        const auto restoredEpoch = configEpochs[inactiveIndex].fetch_add(1, std::memory_order_release) + 1u;
        assert(isReadableConfigEpoch(restoredEpoch));
        (void) (restoredEpoch);
        releaseWriter(true);
        return false;
    }

    config.ready.store(false, std::memory_order_release);
    resetTransitionHistory(config);

    // Set the active slot count
    config.activeSlotCount = desiredSnapshot.slotCount;

    // Set up processing order
    if (desiredSnapshot.orderCount > 0)
    {
        const auto sanitizedOrder = sanitizeSlotOrder(desiredSnapshot.order, config.activeSlotCount);
        for (size_t i = 0; i < config.processingOrder.size(); ++i)
            config.processingOrder[i] = sanitizedOrder[i];
    }
    else
    {
        // Default order: 0, 1, 2, ...
        for (size_t i = 0; i < config.processingOrder.size(); ++i)
            config.processingOrder[i] = static_cast<int>(i);
    }

    config.routingStages = desiredSnapshot.routingStages;
    config.routingLanes = desiredSnapshot.routingLanes;
    uint32_t configuredMask = 0;
    for (int slot = 0; slot < config.activeSlotCount; ++slot)
        if (desiredSnapshot.types[static_cast<size_t>(slot)] != FxType::none)
            configuredMask |= uint32_t{1} << slot;
    config.routingPlan = parameters::buildFxRoutingPlan(config.processingOrder, config.activeSlotCount,
        config.routingStages, config.routingLanes, configuredMask);

    // Prepare the inactive bank without reading mutable active processor state.
    // Audio-thread topology snapshots are batched, so processor allocation is
    // deferred until the control thread drains the pending rebuild.
    try
    {
        for (int i = 0; i < parameters::ids::maxFxSlots; ++i)
        {
            const auto slotIndex = static_cast<size_t>(i);
            auto& slot = config.slots[static_cast<size_t>(i)];
            const FxType desiredType = i < config.activeSlotCount
                ? desiredSnapshot.types[slotIndex]
                : FxType::none;
            const bool targetEnabled = desiredSnapshot.enabled[slotIndex];
            const FxType desiredProcessorType = targetEnabled ? desiredType : FxType::none;

            config.paramCount[slotIndex].store(desiredSnapshot.paramCount[slotIndex],
                                               std::memory_order_relaxed);
            for (size_t paramIndex = 0; paramIndex < maxParamsPerSlot; ++paramIndex)
                config.paramBits[slotIndex][paramIndex].store(
                    encodeParamValue(desiredSnapshot.params[slotIndex][paramIndex]),
                    std::memory_order_relaxed);

            if (slot.processorType != desiredProcessorType)
            {
                auto replacement = createCoreFxProcessor(desiredProcessorType);
                if (replacement != nullptr && prepared)
                    replacement->prepare(currentSpec);

                slot.processor = std::move(replacement);
                slot.processorType = desiredProcessorType;
            }

            slot.type = desiredType;

            slot.enabled = targetEnabled;
            slot.tailModulationSeen.store(0, std::memory_order_relaxed);

            if (auto* processor = slot.getProcessor(slot.type); processor != nullptr)
            {
                const int numCached = desiredSnapshot.paramCount[slotIndex];
                const auto paramCount = maxParamsPerSlot;
                const auto effectiveParamCount =
                    static_cast<size_t>(std::clamp(numCached, 0, paramCount));
                for (int p = 0; p < static_cast<int>(effectiveParamCount); ++p)
                    processor->setGenericParam(
                        p,
                        desiredSnapshot.params[slotIndex][static_cast<size_t>(p)]);

                processor->setBypassed(shouldBypassSlot(slot.type,
                                                        slot.enabled,
                                                        desiredSnapshot.params[slotIndex].data(),
                                                        effectiveParamCount));
                processor->setTransportBpm(transportBpm.load(std::memory_order_acquire));
                processor->reset();
            }
        }
        prepareRoutingHistory(config, prepared);
    }
    catch (...)
    {
        config.ready.store(false, std::memory_order_release);
        const auto restoredEpoch = configEpochs[inactiveIndex].fetch_add(1, std::memory_order_release) + 1u;
        assert(isReadableConfigEpoch(restoredEpoch));
        (void) (restoredEpoch);
        releaseWriter(true);
        return false;
    }

    if (desiredStateSequence.load(std::memory_order_acquire) != desiredSnapshot.sequence)
    {
        config.ready.store(false, std::memory_order_release);
        const auto restoredEpoch = configEpochs[inactiveIndex].fetch_add(1, std::memory_order_release) + 1u;
        assert(isReadableConfigEpoch(restoredEpoch));
        (void) (restoredEpoch);
        releaseWriter(true);
        return false;
    }

    const auto& previousConfig = configs[static_cast<size_t>(active)];
    const auto sameRoutingPlan = [](const parameters::FxRoutingPlan& a, const parameters::FxRoutingPlan& b) noexcept
    {
        if (a.count != b.count || a.slotCount != b.slotCount) return false;
        for (int s = 0; s < a.count; ++s)
        {
            const auto& x = a.stages[static_cast<size_t>(s)];
            const auto& y = b.stages[static_cast<size_t>(s)];
            if (x.id != y.id || x.count != y.count) return false;
            for (int l = 0; l < x.count; ++l)
            {
                const auto& p = x.lanes[static_cast<size_t>(l)];
                const auto& q = y.lanes[static_cast<size_t>(l)];
                if (p.id != q.id || p.count != q.count) return false;
                for (int i = 0; i < p.count; ++i)
                    if (p.slots[static_cast<size_t>(i)] != q.slots[static_cast<size_t>(i)]) return false;
            }
        }
        return true;
    };
    bool needsGraphPublicationFade = previousConfig.activeSlotCount != config.activeSlotCount
        || !sameRoutingPlan(previousConfig.routingPlan, config.routingPlan);
    bool hasPreviousOrTargetAudibleSlot = false;
    const auto orderPosition = [](const ChainConfig& chainConfig, int slot) noexcept
    {
        for (int position = 0; position < chainConfig.activeSlotCount; ++position)
            if (chainConfig.processingOrder[static_cast<size_t>(position)] == slot)
                return position;
        return -1;
    };

    for (int i = 0; i < parameters::ids::maxFxSlots; ++i)
    {
        const auto slotIndex = static_cast<size_t>(i);
        auto& targetSlot = config.slots[slotIndex];
        const auto& previousSlot = previousConfig.slots[slotIndex];
        const auto targetOrderPosition = orderPosition(config, i);
        const auto previousOrderPosition = orderPosition(previousConfig, i);
        const bool targetAudible = targetSlot.type != FxType::none
            && targetSlot.enabled
            && targetOrderPosition >= 0;
        const bool previousAudible = previousSlot.type != FxType::none
            && previousSlot.enabled
            && previousOrderPosition >= 0;
        hasPreviousOrTargetAudibleSlot = hasPreviousOrTargetAudibleSlot
            || previousAudible || targetAudible;
        const bool configuredRoutingChanged =
            ((targetOrderPosition >= 0 && targetSlot.type != FxType::none)
                || (previousOrderPosition >= 0 && previousSlot.type != FxType::none))
            && (previousConfig.routingStages[slotIndex] != config.routingStages[slotIndex]
                || previousConfig.routingLanes[slotIndex] != config.routingLanes[slotIndex]);
        needsGraphPublicationFade = needsGraphPublicationFade
            || configuredRoutingChanged
            || previousAudible != targetAudible
            || (previousAudible
                && targetAudible
                && (previousSlot.type != targetSlot.type
                    || previousOrderPosition != targetOrderPosition));

        targetSlot.fadeInTotal.store(0, std::memory_order_release);
        targetSlot.fadeInRemaining.store(0, std::memory_order_release);
    }

    // Empty/disabled graphs have no sound to retire. A slot-count-only edit
    // there must not start a phantom fade that blocks the next real graph.
    // Count edits with any audible slot still preserve reset processor runtime
    // through the existing old/new graph fade and retired-tail path.
    needsGraphPublicationFade = needsGraphPublicationFade && hasPreviousOrTargetAudibleSlot;

    config.desiredStateSequence = desiredSnapshot.sequence;
    config.requiresExplicitPublicationAcceptance =
        desiredSnapshot.requiresExplicitPublicationAcceptance;
    config.ready.store(true, std::memory_order_release);
    const auto readableEpoch = configEpochs[inactiveIndex].fetch_add(1, std::memory_order_release) + 1u;
    assert(isReadableConfigEpoch(readableEpoch));
    (void) (readableEpoch);

    // Reserve the live transition here; cold publication is decided at actual
    // acceptance, since the old graph can sound while this candidate waits.
    const auto fadeSamples = prepared && needsGraphPublicationFade
        ? slotTypeSwitchFadeSamples(currentSpec) : 0;
    const auto retiredTransportBpm = transportBpm.load(std::memory_order_acquire);
    int retiredTailSilenceWindowSamples = 0;
    const auto naturalRetiredTailSamples = fadeSamples > 0
        ? estimateRetiredTailSamples(previousConfig,
                                     currentSpec,
                                     retiredTransportBpm,
                                     retiredTailSilenceWindowSamples)
        : 0;
    const auto previousLatencySamples = configLatencySamples(previousConfig);
    const auto targetLatencySamples = configLatencySamples(config);
    const auto transitionLatencySamples = needsGraphPublicationFade
        ? std::max(previousLatencySamples, targetLatencySamples)
        : 0;
    // The retired graph receives a tapered excitation during publication. Its
    // natural tail budget starts after that input and alignment interval.
    const auto retiredTailSamples = naturalRetiredTailSamples > 0
        ? naturalRetiredTailSamples + fadeSamples + transitionLatencySamples
        : 0;
    config.publicationPreviousIndex = active;
    config.preparedFadeSamples = fadeSamples;
    config.preparedNeedsGraphFade = needsGraphPublicationFade;
    config.preparedOldLatency = previousLatencySamples;
    config.preparedNewLatency = targetLatencySamples;
    config.preparedTransitionLatency = transitionLatencySamples;
    config.preparedTailSamples = retiredTailSamples;
    config.preparedTailSilenceWindow = retiredTailSilenceWindowSamples;
    config.preparedTailBpm = retiredTransportBpm;

    if (desiredSnapshot.requiresExplicitPublicationAcceptance)
    {
        // Preparing a bank never changes the audible graph or transition state.
        preparedIndex.store(inactive, std::memory_order_release);
        rebuildWriterInProgress.store(false, std::memory_order_release);
        return true;
    }
    preparedIndex.store(-1, std::memory_order_release);
    const auto published = publishPreparedConfig(inactive);
    if (! published)
        releaseWriter(true);
    else
        rebuildWriterInProgress.store(false, std::memory_order_release);
    return published;
}

bool DynamicFxChainCore::publishPreparedConfig(int inactive) noexcept
{
    const auto& config = configs[static_cast<size_t>(inactive)];
    const auto active = activeIndex.load(std::memory_order_acquire);
    const auto fadeSource = publicationFadeConfigIndex.load(std::memory_order_acquire);
    const bool promotePrimaryTailToSecondary =
        publicationTailRemainingSamples.load(std::memory_order_acquire) > 0;
    if (publicationWarmupRemaining.load(std::memory_order_acquire) > 0
        || publicationFadeRemaining.load(std::memory_order_acquire) > 0
        || (promotePrimaryTailToSecondary
            && secondaryTailRemainingSamples.load(std::memory_order_acquire) > 0))
        return false;
    const auto fadeSamples = config.requiresExplicitPublicationAcceptance
        && ! hasProcessedSincePrepare.load(std::memory_order_acquire)
        ? 0 : config.preparedFadeSamples;
    const auto previousLatencySamples = config.preparedOldLatency;
    const auto targetLatencySamples = config.preparedNewLatency;
    const auto transitionLatencySamples = config.preparedTransitionLatency;
    const auto retiredTailSamples = fadeSamples > 0 ? config.preparedTailSamples : 0;
    const auto retiredTailSilenceWindowSamples = config.preparedTailSilenceWindow;
    const auto retiredTransportBpm = config.preparedTailBpm;
    if (promotePrimaryTailToSecondary
        && publicationFadeConfigIndex.load(std::memory_order_acquire) == fadeSource)
    {
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
        if (tailPromotionBoundaryHook)
            tailPromotionBoundaryHook(true);
#endif
        bool expectedPromotion = false;
        if (! tailPromotionInProgress.compare_exchange_strong(
                expectedPromotion, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return false;
        }

        if (processCallbackReaders.load(std::memory_order_acquire) != 0
            || configReaders[static_cast<size_t>(fadeSource)].load(
                   std::memory_order_acquire) != 0)
        {
            tailPromotionInProgress.store(false, std::memory_order_release);
            return false;
        }

        const auto remaining =
            publicationTailRemainingSamples.load(std::memory_order_acquire);
        if (remaining > 0)
        {
            secondaryTailRemainingSamples.store(remaining, std::memory_order_release);
            secondaryTailSilentSamples.store(
                publicationTailSilentSamples.load(std::memory_order_acquire),
                std::memory_order_release);
            secondaryTailSilenceWindowSamples.store(
                publicationTailSilenceWindowSamples.load(std::memory_order_acquire),
                std::memory_order_release);
            secondaryTailTransportBpm.store(
                publicationTailTransportBpm.load(std::memory_order_acquire),
                std::memory_order_release);
            secondaryTailConfigIndex.store(fadeSource, std::memory_order_release);
        }

        publicationFadeConfigIndex.store(-1, std::memory_order_release);
        publicationWarmupRemaining.store(0, std::memory_order_release);
        publicationFadeRemaining.store(0, std::memory_order_release);
        publicationFadeTotal.store(0, std::memory_order_release);
        publicationOldLatencySamples.store(0, std::memory_order_release);
        publicationNewLatencySamples.store(0, std::memory_order_release);
        publicationTailRemainingSamples.store(0, std::memory_order_release);
        publicationTailSilentSamples.store(0, std::memory_order_release);
        publicationTailSilenceWindowSamples.store(0, std::memory_order_release);
        tailPromotionInProgress.store(false, std::memory_order_release);
    }

    if (fadeSamples > 0)
    {
        publicationFadeConfigIndex.store(active, std::memory_order_release);
        publicationWarmupRemaining.store(transitionLatencySamples,
                                          std::memory_order_release);
        publicationFadeTotal.store(fadeSamples, std::memory_order_release);
        publicationFadeRemaining.store(fadeSamples, std::memory_order_release);
        publicationOldLatencySamples.store(previousLatencySamples, std::memory_order_release);
        publicationNewLatencySamples.store(targetLatencySamples, std::memory_order_release);
        publicationTailRemainingSamples.store(retiredTailSamples, std::memory_order_release);
        publicationTailSilentSamples.store(0, std::memory_order_release);
        publicationTailSilenceWindowSamples.store(retiredTailSilenceWindowSamples,
                                                  std::memory_order_release);
        publicationTailTransportBpm.store(retiredTransportBpm, std::memory_order_release);
    }
    else
    {
        publicationWarmupRemaining.store(0, std::memory_order_release);
        publicationFadeRemaining.store(0, std::memory_order_release);
        publicationFadeTotal.store(0, std::memory_order_release);
        publicationFadeConfigIndex.store(-1, std::memory_order_release);
        publicationOldLatencySamples.store(0, std::memory_order_release);
        publicationNewLatencySamples.store(0, std::memory_order_release);
        publicationTailRemainingSamples.store(0, std::memory_order_release);
        publicationTailSilentSamples.store(0, std::memory_order_release);
        publicationTailSilenceWindowSamples.store(0, std::memory_order_release);
    }

    publishedLatencySamples.store(fadeSamples > 0
                                      ? transitionLatencySamples : targetLatencySamples,
                                  std::memory_order_release);
    activeIndex.store(inactive, std::memory_order_release);
    acceptedStateSequence.store(config.desiredStateSequence, std::memory_order_release);
    if (prepared && fadeSamples == 0)
        inactiveProcessorCleanupPending.store(true, std::memory_order_release);
    return true;
}

void DynamicFxChainCore::reclaimInactiveProcessorBanksOnControlThread() noexcept
{
    if (! inactiveProcessorCleanupPending.exchange(false, std::memory_order_acq_rel))
        return;

    bool writerExpected = false;
    if (! rebuildWriterInProgress.compare_exchange_strong(writerExpected,
                                                          true,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
    {
        inactiveProcessorCleanupPending.store(true, std::memory_order_release);
        return;
    }

    if (processCallbackReaders.load(std::memory_order_acquire) != 0)
    {
        inactiveProcessorCleanupPending.store(true, std::memory_order_release);
        rebuildWriterInProgress.store(false, std::memory_order_release);
        return;
    }

    const auto releaseWriter = fad::engine::detail::ScopeExit{[this]
    {
        rebuildWriterInProgress.store(false, std::memory_order_release);
    }};
    const auto active = activeIndex.load(std::memory_order_acquire);
    const auto fadeSource = publicationFadeConfigIndex.load(std::memory_order_acquire);
    const auto secondaryTail = secondaryTailConfigIndex.load(std::memory_order_acquire);
    const auto transitionActive =
        publicationWarmupRemaining.load(std::memory_order_acquire) > 0
        || publicationFadeRemaining.load(std::memory_order_acquire) > 0
        || publicationTailRemainingSamples.load(std::memory_order_acquire) > 0;
    const auto acceptedSequence = acceptedStateSequence.load(std::memory_order_acquire);

    for (int candidate = 0; candidate < static_cast<int>(configs.size()); ++candidate)
    {
        if (candidate == active
            || candidate == preparedIndex.load(std::memory_order_acquire)
            || (transitionActive && candidate == fadeSource)
            || (secondaryTailRemainingSamples.load(std::memory_order_acquire) > 0
                && candidate == secondaryTail))
            continue;

        const auto candidateIndex = static_cast<size_t>(candidate);
        const auto& candidateConfig = configs[candidateIndex];
        if (candidateConfig.ready.load(std::memory_order_acquire)
            && candidateConfig.desiredStateSequence == acceptedSequence)
        {
            // Retain the accepted baseline while a preview is pending. A later
            // publication requests cleanup again if this bank becomes stale;
            // keeping the flag set here would wake the bridge forever.
            continue;
        }

        if (configReaders[candidateIndex].load(std::memory_order_acquire) != 0)
        {
            inactiveProcessorCleanupPending.store(true, std::memory_order_release);
            continue;
        }

        const auto writeEpoch =
            configEpochs[candidateIndex].fetch_add(1, std::memory_order_acq_rel) + 1u;
        assert(! isReadableConfigEpoch(writeEpoch));
        (void) (writeEpoch);
        if (configReaders[candidateIndex].load(std::memory_order_acquire) != 0)
        {
            const auto restoredEpoch =
                configEpochs[candidateIndex].fetch_add(1, std::memory_order_release) + 1u;
            assert(isReadableConfigEpoch(restoredEpoch));
            (void) (restoredEpoch);
            inactiveProcessorCleanupPending.store(true, std::memory_order_release);
            continue;
        }

        auto& config = configs[candidateIndex];
        config.ready.store(false, std::memory_order_release);
        config.activeSlotCount = 0;
        config.desiredStateSequence = 0;
        config.requiresExplicitPublicationAcceptance = false;
        resetTransitionHistory(config);
        for (auto& slot : config.slots)
        {
            slot.processor.reset();
            slot.processorType = FxType::none;
            slot.type = FxType::none;
            slot.enabled = false;
            slot.fadeInRemaining.store(0, std::memory_order_release);
            slot.fadeInTotal.store(0, std::memory_order_release);
        }
        for (size_t slotIndex = 0; slotIndex < config.paramBits.size(); ++slotIndex)
        {
            config.paramCount[slotIndex].store(0, std::memory_order_relaxed);
            for (auto& paramBits : config.paramBits[slotIndex])
                paramBits.store(encodeParamValue(0.0f), std::memory_order_relaxed);
        }

        const auto readableEpoch =
            configEpochs[candidateIndex].fetch_add(1, std::memory_order_release) + 1u;
        assert(isReadableConfigEpoch(readableEpoch));
        (void) (readableEpoch);
    }
}

void DynamicFxChainCore::requestRebuild() noexcept
{
    if (batchUpdateDepth.load(std::memory_order_acquire) > 0 || ! tryRebuildChain())
        rebuildPending.store(true, std::memory_order_release);
}

bool DynamicFxChainCore::hasPendingRebuildForControlThread() const noexcept
{
    return rebuildPending.load(std::memory_order_acquire)
        || inactiveProcessorCleanupPending.load(std::memory_order_acquire);
}

void DynamicFxChainCore::tryApplyPendingRebuildOnControlThread() noexcept
{
    if (batchUpdateDepth.load(std::memory_order_acquire) > 0)
        return;

    if (rebuildPending.load(std::memory_order_acquire))
        (void) (tryRebuildChain());

    reclaimInactiveProcessorBanksOnControlThread();
}

// ── getActiveLatencySamples ────────────────────────────────────────
int DynamicFxChainCore::configLatencySamples(const ChainConfig& config) noexcept
{
    int totalLatency = 0;
    for (int i = 0; i < config.routingPlan.count; ++i)
    {
        const auto& stage = config.routingPlan.stages[static_cast<size_t>(i)];
        int stageLatency = 0;
        for (int lane = 0; lane < stage.count; ++lane)
            stageLatency = std::max(stageLatency, routingLaneLatency(config, stage.lanes[static_cast<size_t>(lane)]));
        totalLatency += stageLatency;
    }

    return totalLatency;
}

int DynamicFxChainCore::routingLaneLatency(const ChainConfig& config, const parameters::FxRoutingLane& lane) noexcept
{
    int latency = 0;
    for (int i = 0; i < lane.count; ++i)
    {
        const auto& slot = config.slots[static_cast<size_t>(lane.slots[static_cast<size_t>(i)])];
        if (slot.enabled)
            if (const auto* processor = slot.getProcessor(slot.type))
                latency += processor->getLatencySamples();
    }
    return latency;
}

void DynamicFxChainCore::prepareRoutingHistory(ChainConfig& config, bool hasProcessSpec)
{
    std::array<bool, parameters::ids::maxFxSlots> used{};
    config.routingHistoryPosition.fill(0);
    for (int s = 0; s < config.routingPlan.count; ++s)
    {
        const auto& stage = config.routingPlan.stages[static_cast<size_t>(s)];
        if (stage.count < 2) continue;
        for (int l = 0; l < stage.count; ++l)
            used[static_cast<size_t>(stage.lanes[static_cast<size_t>(l)].slots[0])] = true;
    }
    for (size_t i = 0; i < used.size(); ++i)
    {
        auto& history = config.routingHistory[i];
        if (used[i] && hasProcessSpec)
            history.setSize(std::max(1, static_cast<int>(currentSpec.numChannels)),
                maximumPublicationAlignmentSamples(currentSpec) + 1, false, false, true);
        else history.setSize(0, 0);
        history.clear();
    }
}

int DynamicFxChainCore::estimateRetiredTailSamples(const ChainConfig& config,
                                               const fad::engine::ProcessSpec& spec,
                                               double currentTransportBpm,
                                               int& silenceWindowSamples) noexcept
{
    const auto paramAt = [&config](int slotIndex, int paramIndex, float fallback) noexcept
    {
        const auto slot = static_cast<size_t>(slotIndex);
        if (paramIndex < 0
            || paramIndex >= config.paramCount[slot].load(std::memory_order_acquire))
        {
            return fallback;
        }

        return decodeParamValue(
            config.paramBits[slot][static_cast<size_t>(paramIndex)].load(std::memory_order_acquire));
    };

    const auto safeTempo = std::isfinite(currentTransportBpm)
        ? std::clamp(currentTransportBpm, 20.0, 400.0)
        : 120.0;
    double tailSeconds = 0.0;
    double silenceWindowSeconds = 0.0;

    for (int orderIndex = 0; orderIndex < config.activeSlotCount; ++orderIndex)
    {
        const auto slotIndex = config.processingOrder[static_cast<size_t>(orderIndex)];
        if (slotIndex < 0 || slotIndex >= parameters::ids::maxFxSlots)
            continue;

        const auto& slot = config.slots[static_cast<size_t>(slotIndex)];
        if (! slot.enabled)
            continue;

        const auto modulation = slot.tailModulationSeen.load(std::memory_order_acquire);

        if (slot.type == FxType::delay
            && (paramAt(slotIndex, 4, 0.0f) > 0.001f || paramAt(slotIndex, 5, 0.0f) > 0.0f || (modulation & wetTailModulation) != 0))
        {
            const auto timeIndex = fad::engine::math::roundToNearestInt(
                FxCatalog::denormalizeParamValue(FxType::delay,
                                                  0,
                                                  paramAt(slotIndex, 0, 0.0f)));
            const auto mode = fad::engine::math::roundToNearestInt(paramAt(slotIndex, 5, 0.0f) * 2.0f);
            const auto nativeTime = 16.0 * std::max(paramAt(slotIndex, 6, 0.0f), paramAt(slotIndex, 7, 0.0f));
            auto cycleSeconds = mode == 0 ? delayIndexToBeats(timeIndex) * 60.0 / safeTempo
                : std::clamp(nativeTime * (mode == 2 ? 60.0 / safeTempo : 1.0), 0.0, 12.0);
            const auto left=wideFxDecode(27,paramAt(slotIndex,27,0));
            const auto right=wideFxDecode(28,paramAt(slotIndex,28,0));
            if(left>=0 || right>=0)cycleSeconds=std::max(left>=0?left*.001:cycleSeconds, right>=0?right*.001:cycleSeconds);
            const auto feedback = static_cast<double>(
                FxCatalog::denormalizeParamValue(FxType::delay,
                                                  1,
                                                  paramAt(slotIndex, 1, 0.0f)));
            const auto filterTail = mode!=0 && paramAt(slotIndex,9,0.0f)>0.0f
                ? 6000.0 / fadmodel1::dsp::utils::sanitizeSampleRate(spec.sampleRate) : 0.0;
            tailSeconds += estimateFeedbackTailSeconds(cycleSeconds, feedback) + filterTail;
            silenceWindowSeconds += cycleSeconds + delayTimeSmoothingSeconds + filterTail;
        }
        else if (slot.type == FxType::stereo && paramAt(slotIndex, 5, 0.0f) >= .5f
                 && paramAt(slotIndex, 6, 0.0f) > .001f) {
            tailSeconds += .025;
            silenceWindowSeconds += .025;
        }
        // Native Wet may be driven by a transient macro even when its saved
        // base is zero. Publication reads the atomic activity flags and saved
        // native profile, never the running processor's mutable wet offset.
        else if (slot.type == FxType::reverb
                 && (paramAt(slotIndex, 2, 0.0f) > 0.001f || paramAt(slotIndex, 7, 0.0f) > 0.0f || (modulation & wetTailModulation) != 0))
        {
            const auto hall = reverbUsesHall(paramAt(slotIndex, 13, 0.0f), paramAt(slotIndex, 8, 0.0f));
            const bool timeModulated = (modulation & reverbTimeModulation) != 0;
            const auto extendedDelay = !hall && timeModulated ? 1.0f : paramAt(slotIndex, 14, 0.0f);
            const auto preDelaySeconds = static_cast<double>(reverbPreDelayMilliseconds(
                paramAt(slotIndex, 6, 0.0f), hall ? 0.0f : extendedDelay)) * 0.001;
            tailSeconds += reverbTailSeconds(paramAt(slotIndex, 0, 0.35f), paramAt(slotIndex, 7, 0.0f),
                hall && timeModulated ? 1.0f : paramAt(slotIndex, 8, 0.0f), paramAt(slotIndex, 13, 0.0f), paramAt(slotIndex, 6, 0.0f),
                extendedDelay, paramAt(slotIndex, 12, 0.0f));
            silenceWindowSeconds += preDelaySeconds + (hall
                ? NativeHallReverb::maximumNetworkSilenceSeconds()
                : maximumReverbNetworkDelaySeconds);
        }
    }

    const auto sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    silenceWindowSamples = tailSeconds > 0.0
        ? static_cast<int>(std::ceil(std::clamp(silenceWindowSeconds, 0.0, maximumRetiredTailSeconds)
                                     * sampleRate))
            + static_cast<int>(spec.maximumBlockSize)
        : 0;
    return static_cast<int>(std::ceil(
        std::clamp(tailSeconds, 0.0, maximumRetiredTailSeconds) * sampleRate));
}

int DynamicFxChainCore::getActiveLatencySamples() noexcept
{
    return publishedLatencySamples.load(std::memory_order_acquire);
}

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
int DynamicFxChainCore::testGetConfigReaderCount(int index) const noexcept
{
    if (index < 0 || index >= static_cast<int>(configReaders.size()))
        return -1;

    return configReaders[static_cast<size_t>(index)].load(std::memory_order_acquire);
}

int DynamicFxChainCore::testAcquireReadableConfigIndex() const noexcept
{
    return acquireReadableConfigIndex();
}

void DynamicFxChainCore::testReleaseReadableConfigIndex(int index) const noexcept
{
    releaseReadableConfigIndex(index);
}

int DynamicFxChainCore::testGetConfigActiveSlotCount(int index) const noexcept
{
    if (index < 0 || index >= static_cast<int>(configs.size()))
        return -1;

    return configs[static_cast<size_t>(index)].activeSlotCount;
}

int DynamicFxChainCore::testGetAllocatedProcessorCount(FxType type) const noexcept
{
    int count = 0;
    for (const auto& config : configs)
        for (const auto& slot : config.slots)
            if (slot.processor != nullptr && slot.processorType == type)
                ++count;
    return count;
}

int DynamicFxChainCore::testGetCachedParamCount(int slotIndex) const noexcept
{
    if (slotIndex < 0 || slotIndex >= static_cast<int>(cachedParamCount.size()))
        return -1;
    return cachedParamCount[static_cast<size_t>(slotIndex)].load(std::memory_order_acquire);
}

float DynamicFxChainCore::testGetCachedParamValue(int slotIndex, int paramIndex) const noexcept
{
    if (slotIndex < 0
        || slotIndex >= static_cast<int>(cachedParamBits.size())
        || paramIndex < 0
        || paramIndex >= maxParamsPerSlot)
    {
        return 0.0f;
    }

    return decodeParamValue(
        cachedParamBits[static_cast<size_t>(slotIndex)][static_cast<size_t>(paramIndex)].load(
            std::memory_order_acquire));
}

#endif

void DynamicFxChainCore::processConfigSlots(const ChainConfig& config,
                                        fad::engine::ProcessContextReplacing<float>& context,
                                        fad::engine::AudioBlock<float>& outputBlock,
                                        double currentTransportBpm,
                                        std::vector<diagnostics::CpuMeter>* meters,
                                        diagnostics::ClipDetector* clip)
{
    const auto renderSlot = [&](int slotIdx, fad::engine::ProcessContextReplacing<float>& slotContext,
                                fad::engine::AudioBlock<float>& block)
    {
        const auto& slot = config.slots[static_cast<size_t>(slotIdx)];
        auto* processor = slot.getProcessor(slot.type);
        if (slot.type != FxType::none && slot.enabled && processor != nullptr)
        {
            processor->setTransportBpm(currentTransportBpm);
            size_t meterIndex = 0;
            if (meters != nullptr)
                while (meterIndex < static_cast<size_t>(config.activeSlotCount)
                    && config.processingOrder[meterIndex] != slotIdx) ++meterIndex;
            const bool hasMeter = meters != nullptr && meterIndex < meters->size();
            if (hasMeter) (*meters)[meterIndex].beginMeasurement();
            processSlotWithOptionalFade(slot, *processor, slotContext, block);
            if (hasMeter) (*meters)[meterIndex].endMeasurement();
            if (clip != nullptr)
                if (const auto id = diagnostics::fxSlotModuleIdForIndex(slotIdx))
                {
                    float peak = 0;
                    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
                        for (size_t i = 0; i < block.getNumSamples(); ++i)
                        {
                            const auto magnitude = std::abs(block.getChannelPointer(ch)[i]);
                            if (magnitude > peak) peak = magnitude;
                        }
                    clip->checkSample(*id, peak, 0);
                }
        }
    };
    // Keep the existing serial path free of branch-buffer copies.
    if (!config.routingPlan.parallel)
    {
        for (int i = 0; i < config.routingPlan.slotCount; ++i)
            renderSlot(config.routingPlan.flattenedOrder[static_cast<size_t>(i)], context, outputBlock);
        return;
    }
    const auto channels = std::min(outputBlock.getNumChannels(), static_cast<size_t>(routingInputBuffer.getNumChannels()));
    const auto samples = static_cast<int>(outputBlock.getNumSamples());
    // Both public processors split oversized blocks before this function.
    assert(channels == outputBlock.getNumChannels());
    if (channels == 0 || samples > routingInputBuffer.getNumSamples()) { assert(false); return; }
    for (int s = 0; s < config.routingPlan.count; ++s)
    {
        const auto& stage = config.routingPlan.stages[static_cast<size_t>(s)];
        if (stage.count == 1)
        {
            const auto& lane = stage.lanes[0];
            for (int i = 0; i < lane.count; ++i)
                renderSlot(lane.slots[static_cast<size_t>(i)], context, outputBlock);
            continue;
        }
        bool audible = false;
        int stageLatency = 0;
        for (int l = 0; l < stage.count; ++l)
        {
            const auto& lane = stage.lanes[static_cast<size_t>(l)];
            stageLatency = std::max(stageLatency, routingLaneLatency(config, lane));
            for (int i = 0; i < lane.count; ++i)
            {
                const auto& slot = config.slots[static_cast<size_t>(lane.slots[static_cast<size_t>(i)])];
                audible = audible || (slot.enabled && slot.getProcessor(slot.type) != nullptr);
            }
        }
        if (!audible) continue; // All bypassed branches are the exact dry input.
        for (size_t ch = 0; ch < channels; ++ch)
            routingInputBuffer.copyFrom(static_cast<int>(ch), 0, outputBlock.getChannelPointer(ch), samples);
        routingSumBuffer.clear(0, samples);
        const auto gain = 1.0f / static_cast<float>(stage.count);
        for (int l = 0; l < stage.count; ++l)
        {
            const auto& lane = stage.lanes[static_cast<size_t>(l)];
            for (size_t ch = 0; ch < channels; ++ch)
                routingLaneBuffer.copyFrom(static_cast<int>(ch), 0, routingInputBuffer, static_cast<int>(ch), 0, samples);
            auto laneBlock = fad::engine::AudioBlock<float>(routingLaneBuffer).getSubsetChannelBlock(0, channels).getSubBlock(0, static_cast<size_t>(samples));
            fad::engine::ProcessContextReplacing<float> laneContext(laneBlock);
            for (int i = 0; i < lane.count; ++i)
                renderSlot(lane.slots[static_cast<size_t>(i)], laneContext, laneBlock);
            const auto key = static_cast<size_t>(lane.slots[0]);
            auto& history = config.routingHistory[key];
            const auto capacity = history.getNumSamples();
            if (capacity == 0) continue;
            const auto delay = std::clamp(stageLatency - routingLaneLatency(config, lane), 0, capacity - 1);
            auto position = config.routingHistoryPosition[key];
            for (int sample = 0; sample < samples; ++sample)
            {
                const auto read = (position + capacity - delay) % capacity;
                for (size_t ch = 0; ch < channels; ++ch)
                {
                    auto* ring = history.getWritePointer(static_cast<int>(ch));
                    ring[position] = utils::sanitizeFinite(laneBlock.getChannelPointer(ch)[sample]);
                    routingSumBuffer.getWritePointer(static_cast<int>(ch))[sample] += ring[read] * gain;
                }
                if (++position == capacity) position = 0;
            }
            config.routingHistoryPosition[key] = position;
        }
        for (size_t ch = 0; ch < channels; ++ch)
            std::copy_n(routingSumBuffer.getReadPointer(static_cast<int>(ch)), samples, outputBlock.getChannelPointer(ch));
    }
}

int DynamicFxChainCore::maximumPublicationAlignmentSamples(
    const fad::engine::ProcessSpec& spec) noexcept
{
    const auto sampleRate = utils::sanitizeSampleRate(spec.sampleRate);
    const auto perSlotLatency = static_cast<int>(
        std::lround(sampleRate * maximumLatencyPerSlotSeconds));
    return std::max(1, perSlotLatency * maximumLatencyBearingSlotCount);
}

void DynamicFxChainCore::resetTransitionHistory(ChainConfig& config) noexcept
{
    config.transitionHistoryWritePosition = 0;
    config.transitionHistorySamplesAvailable = 0;
    config.transitionHistoryDelaySamples = 0;
}

void DynamicFxChainCore::applyTransitionDelay(const ChainConfig& config,
                                          fad::engine::AudioBlock<float>& block,
                                          int delaySamples,
                                          bool preserveInputUntilPrimed) noexcept
{
    const auto blockChannels = static_cast<int>(block.getNumChannels());
    const auto numSamples = static_cast<int>(block.getNumSamples());
    const auto historyChannels = config.transitionHistoryBuffer.getNumChannels();
    const auto historyCapacity = config.transitionHistoryBuffer.getNumSamples();
    if (blockChannels <= 0
        || blockChannels > historyChannels
        || numSamples <= 0
        || historyCapacity <= 0)
    {
        return;
    }

    const auto safeDelay = std::clamp(delaySamples, 0, historyCapacity - 1);
    config.transitionHistoryDelaySamples = safeDelay;
    auto writePosition = config.transitionHistoryWritePosition;
    auto samplesAvailable = config.transitionHistorySamplesAvailable;
    for (int sample = 0; sample < numSamples; ++sample)
    {
        auto readPosition = writePosition - safeDelay;
        if (readPosition < 0)
            readPosition += historyCapacity;
        const auto delayIsPrimed = samplesAvailable >= safeDelay;

        for (int channel = 0; channel < blockChannels; ++channel)
        {
            auto* output = block.getChannelPointer(static_cast<size_t>(channel));
            auto* history = config.transitionHistoryBuffer.getWritePointer(channel);
            const auto input = utils::sanitizeFinite(output[sample]);
            const auto delayed = delayIsPrimed
                ? utils::sanitizeFinite(history[readPosition])
                : 0.0f;
            history[writePosition] = input;
            if (safeDelay > 0 && (delayIsPrimed || ! preserveInputUntilPrimed))
                output[sample] = delayed;
        }

        writePosition = (writePosition + 1) % historyCapacity;
        samplesAvailable = std::min(historyCapacity, samplesAvailable + 1);
    }

    config.transitionHistoryWritePosition = writePosition;
    config.transitionHistorySamplesAvailable = samplesAvailable;
}

int DynamicFxChainCore::preparePublicationFadeScratch(int activeConfigIndexForBlock,
                                                  fad::engine::AudioBlock<float>& outputBlock,
                                                  int& transitionSamples,
                                                  int& warmupRemaining,
                                                  int& fadeRemaining,
                                                  int& fadeTotal)
{
    transitionSamples = 0;
    warmupRemaining = 0;
    fadeRemaining = 0;
    fadeTotal = 0;

    const auto retiredConfigIndex = publicationFadeConfigIndex.load(std::memory_order_acquire);
    const auto warmup = publicationWarmupRemaining.load(std::memory_order_acquire);
    const auto remaining = publicationFadeRemaining.load(std::memory_order_acquire);
    const auto total = publicationFadeTotal.load(std::memory_order_acquire);
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());

    if (retiredConfigIndex < 0
        || retiredConfigIndex == activeConfigIndexForBlock
        || (warmup <= 0 && remaining <= 0)
        || total <= 0
        || blockChannels <= 0
        || blockChannels > publicationFadeScratchBuffer.getNumChannels()
        || numSamples <= 0
        || numSamples > publicationFadeScratchBuffer.getNumSamples())
    {
        return -1;
    }

    const auto acquiredConfigIndex = acquireSpecificReadableConfigIndex(retiredConfigIndex);
    if (acquiredConfigIndex < 0)
        return -1;

    transitionSamples = std::min({numSamples, warmup + remaining, publicationFadeScratchBuffer.getNumSamples()});
    if (transitionSamples <= 0)
    {
        releaseReadableConfigIndex(acquiredConfigIndex);
        return -1;
    }

    for (int channel = 0; channel < blockChannels; ++channel)
    {
        publicationFadeScratchBuffer.copyFrom(channel,
                                              0,
                                              outputBlock.getChannelPointer(static_cast<size_t>(channel)),
                                              transitionSamples);
        if (transitionSamples < numSamples)
            std::fill_n(publicationFadeScratchBuffer.getWritePointer(channel, transitionSamples), numSamples - transitionSamples, 0.0f);
    }

    if (publicationTailRemainingSamples.load(std::memory_order_acquire) > 0)
    {
        // Fade new excitation, not the already-running tail. The input ramp
        // precedes the output ramp by the common graph latency, so aligned dry
        // contributions remain complementary when a limiter is in either graph.
        const auto transitionLatency = std::max(publicationOldLatencySamples.load(std::memory_order_acquire), publicationNewLatencySamples.load(std::memory_order_acquire));
        const auto elapsedInputSamples = transitionLatency + total - warmup - remaining;
        const auto inverseTotal = 1.0f / static_cast<float>(total);
        for (int channel = 0; channel < blockChannels; ++channel)
        {
            auto* retiredInput = publicationFadeScratchBuffer.getWritePointer(channel);
            for (int sample = 0; sample < transitionSamples; ++sample)
            {
                const auto inputGain = 1.0f - std::clamp(static_cast<float>(elapsedInputSamples + sample) * inverseTotal, 0.0f, 1.0f);
                retiredInput[sample] *= inputGain;
            }
        }
    }

    warmupRemaining = warmup;
    fadeRemaining = remaining;
    fadeTotal = total;
    return acquiredConfigIndex;
}

void DynamicFxChainCore::renderAndBlendPublicationFade(int activeConfigIndex,
                                                   int fadeConfigIndex,
                                                   int transitionSamples,
                                                   int warmupRemaining,
                                                   int fadeRemaining,
                                                   int fadeTotal,
                                                   fad::engine::AudioBlock<float>& outputBlock,
                                                   double currentTransportBpm)
{
    if (activeConfigIndex < 0
        || activeConfigIndex >= static_cast<int>(configs.size())
        || fadeConfigIndex < 0
        || transitionSamples <= 0
        || (warmupRemaining <= 0 && fadeRemaining <= 0)
        || fadeTotal <= 0)
        return;

    const auto releaseConfig = fad::engine::detail::ScopeExit{[this, fadeConfigIndex]
    {
        releaseReadableConfigIndex(fadeConfigIndex);
    }};

    const auto renderedSamples = static_cast<int>(outputBlock.getNumSamples());
    auto scratchBlock = fad::engine::AudioBlock<float>(publicationFadeScratchBuffer)
        .getSubBlock(0, static_cast<size_t>(renderedSamples));
    auto scratchContext = fad::engine::ProcessContextReplacing<float>(scratchBlock);
    processConfigSlots(configs[static_cast<size_t>(fadeConfigIndex)],
                       scratchContext,
                       scratchBlock,
                       currentTransportBpm);
    sanitizeBlock(scratchBlock);

    const auto oldLatency = publicationOldLatencySamples.load(std::memory_order_acquire);
    const auto newLatency = publicationNewLatencySamples.load(std::memory_order_acquire);
    const auto transitionLatency = std::max(oldLatency, newLatency);
    applyTransitionDelay(configs[static_cast<size_t>(fadeConfigIndex)],
                         scratchBlock,
                         transitionLatency - oldLatency,
                         true);
    auto newTransitionBlock = outputBlock.getSubBlock(
        0,
        static_cast<size_t>(transitionSamples));
    applyTransitionDelay(configs[static_cast<size_t>(activeConfigIndex)],
                         newTransitionBlock,
                         transitionLatency - newLatency,
                         false);

    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());
    const auto tailRemainingBefore =
        publicationTailRemainingSamples.load(std::memory_order_acquire);
    const bool preservesRetiredTail = tailRemainingBefore > 0;
    const auto warmupSamples = std::min(warmupRemaining, transitionSamples);
    const auto fadeSamples = std::min(fadeRemaining, transitionSamples - warmupSamples);
    const auto alreadyFadedSamples = fadeTotal - fadeRemaining;
    const auto invFadeTotal = 1.0f / static_cast<float>(fadeTotal);
    for (int channel = 0; channel < blockChannels; ++channel)
    {
        const auto* oldGraph = publicationFadeScratchBuffer.getReadPointer(channel);
        auto* newGraph = outputBlock.getChannelPointer(static_cast<size_t>(channel));
        std::copy_n(oldGraph, warmupSamples, newGraph);
        for (int sample = 0; sample < fadeSamples; ++sample)
        {
            const auto transitionSample = warmupSamples + sample;
            const auto newGain = std::clamp(static_cast<float>(alreadyFadedSamples + sample) * invFadeTotal, 0.0f, 1.0f);
            newGraph[transitionSample] = utils::sanitizeFinite(
                oldGraph[transitionSample] * (preservesRetiredTail ? 1.0f : 1.0f - newGain)
                + newGraph[transitionSample] * newGain);
        }
    }

    const auto warmupAfterBlock = std::max(0, warmupRemaining - warmupSamples);
    const auto remainingAfterBlock = std::max(0, fadeRemaining - fadeSamples);
    const auto tailRemainingAfterBlock = std::max(0, tailRemainingBefore - renderedSamples);
    publicationWarmupRemaining.store(warmupAfterBlock, std::memory_order_release);
    publicationFadeRemaining.store(remainingAfterBlock, std::memory_order_release);
    publicationTailRemainingSamples.store(tailRemainingAfterBlock, std::memory_order_release);
    if (warmupAfterBlock == 0 && remainingAfterBlock == 0)
    {
        publishedLatencySamples.store(newLatency, std::memory_order_release);
        publicationWarmupRemaining.store(0, std::memory_order_release);
        publicationFadeTotal.store(0, std::memory_order_release);
        publicationOldLatencySamples.store(0, std::memory_order_release);
        publicationNewLatencySamples.store(0, std::memory_order_release);
        if (tailRemainingAfterBlock <= 0)
            finishPublicationRetirement();
    }

    if (transitionSamples < static_cast<int>(outputBlock.getNumSamples()))
    {
        auto remainder = outputBlock.getSubBlock(
            static_cast<size_t>(transitionSamples),
            outputBlock.getNumSamples() - static_cast<size_t>(transitionSamples));
        applyTransitionDelay(configs[static_cast<size_t>(activeConfigIndex)],
                             remainder,
                             0,
                             true);

        const auto tailRemainderSamples = std::min(static_cast<int>(remainder.getNumSamples()), std::max(0, tailRemainingBefore - transitionSamples));
        for (int channel = 0; channel < blockChannels; ++channel)
        {
            const auto* retiredTail = publicationFadeScratchBuffer.getReadPointer(
                channel,
                transitionSamples);
            auto* activeOutput = remainder.getChannelPointer(static_cast<size_t>(channel));
            for (int sample = 0; sample < tailRemainderSamples; ++sample)
                activeOutput[sample] = utils::sanitizeFinite(activeOutput[sample]
                                                             + retiredTail[sample]);
        }
    }
}

void DynamicFxChainCore::renderRetiredTail(int activeConfigIndex,
                                       fad::engine::AudioBlock<float>& outputBlock)
{
    if (tailPromotionInProgress.load(std::memory_order_acquire))
        return;

    const auto tailRemaining =
        publicationTailRemainingSamples.load(std::memory_order_acquire);
    const auto retiredConfigIndex = publicationFadeConfigIndex.load(std::memory_order_acquire);
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto numChannels = static_cast<int>(outputBlock.getNumChannels());
    if (tailRemaining <= 0
        || retiredConfigIndex < 0
        || retiredConfigIndex == activeConfigIndex
        || numSamples <= 0
        || numSamples > publicationFadeScratchBuffer.getNumSamples()
        || numChannels <= 0
        || numChannels > publicationFadeScratchBuffer.getNumChannels())
    {
        return;
    }

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    if (tailPromotionBoundaryHook)
        tailPromotionBoundaryHook(false);
#endif
    const auto acquiredConfigIndex = acquireSpecificReadableConfigIndex(retiredConfigIndex);
    if (acquiredConfigIndex < 0)
        return;
    const auto releaseConfig = fad::engine::detail::ScopeExit{[this, acquiredConfigIndex]
    {
        releaseReadableConfigIndex(acquiredConfigIndex);
    }};
    if (tailPromotionInProgress.load(std::memory_order_acquire))
        return;

    for (int channel = 0; channel < numChannels; ++channel)
        std::fill_n(publicationFadeScratchBuffer.getWritePointer(channel), numSamples, 0.0f);

    auto scratchBlock = fad::engine::AudioBlock<float>(publicationFadeScratchBuffer)
        .getSubBlock(0, static_cast<size_t>(numSamples));
    auto scratchContext = fad::engine::ProcessContextReplacing<float>(scratchBlock);
    processConfigSlots(configs[static_cast<size_t>(acquiredConfigIndex)],
                       scratchContext,
                       scratchBlock,
                       publicationTailTransportBpm.load(std::memory_order_acquire));
    sanitizeBlock(scratchBlock);

    const auto& retiredConfig = configs[static_cast<size_t>(acquiredConfigIndex)];
    applyTransitionDelay(retiredConfig, scratchBlock,
                         retiredConfig.transitionHistoryDelaySamples, true);

    const auto samplesToMix = std::min(tailRemaining, numSamples);
    const auto silenceWindow =
        publicationTailSilenceWindowSamples.load(std::memory_order_acquire);
    const auto previousSilentSamples =
        publicationTailSilentSamples.load(std::memory_order_acquire);
    const auto retiredPeak = measurePeakAbs(scratchBlock, samplesToMix);
    const auto silentSamplesAfterBlock = retiredPeak <= retiredTailSilenceThreshold
        ? std::min(silenceWindow, static_cast<int>(std::min<int64_t>(
                         std::numeric_limits<int>::max(),
                         static_cast<int64_t>(previousSilentSamples) + samplesToMix)))
        : 0;
    publicationTailSilentSamples.store(silentSamplesAfterBlock, std::memory_order_release);
    for (int channel = 0; channel < numChannels; ++channel)
    {
        const auto* retiredTail = publicationFadeScratchBuffer.getReadPointer(channel);
        auto* activeOutput = outputBlock.getChannelPointer(static_cast<size_t>(channel));
        for (int sample = 0; sample < samplesToMix; ++sample)
            activeOutput[sample] = utils::sanitizeFinite(activeOutput[sample]
                                                         + retiredTail[sample]);
    }

    const auto remainingAfterBlock = std::max(0, tailRemaining - numSamples);
    publicationTailRemainingSamples.store(remainingAfterBlock, std::memory_order_release);
    if (remainingAfterBlock == 0
        || (silenceWindow > 0 && silentSamplesAfterBlock >= silenceWindow))
        finishPublicationRetirement();
}

void DynamicFxChainCore::renderSecondaryRetiredTail(
    int activeConfigIndex,
    fad::engine::AudioBlock<float>& outputBlock)
{
    const auto remaining = secondaryTailRemainingSamples.load(std::memory_order_acquire);
    const auto retiredConfigIndex = secondaryTailConfigIndex.load(std::memory_order_acquire);
    const auto primaryRetiredConfigIndex =
        publicationFadeConfigIndex.load(std::memory_order_acquire);
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto numChannels = static_cast<int>(outputBlock.getNumChannels());
    if (remaining <= 0
        || retiredConfigIndex < 0
        || retiredConfigIndex == activeConfigIndex
        || retiredConfigIndex == primaryRetiredConfigIndex
        || numSamples <= 0
        || numSamples > publicationFadeScratchBuffer.getNumSamples()
        || numChannels <= 0
        || numChannels > publicationFadeScratchBuffer.getNumChannels())
    {
        return;
    }

    const auto acquiredConfigIndex = acquireSpecificReadableConfigIndex(retiredConfigIndex);
    if (acquiredConfigIndex < 0)
        return;
    const auto releaseConfig = fad::engine::detail::ScopeExit{[this, acquiredConfigIndex]
    {
        releaseReadableConfigIndex(acquiredConfigIndex);
    }};

    for (int channel = 0; channel < numChannels; ++channel)
        std::fill_n(publicationFadeScratchBuffer.getWritePointer(channel), numSamples, 0.0f);

    auto scratchBlock = fad::engine::AudioBlock<float>(publicationFadeScratchBuffer)
        .getSubBlock(0, static_cast<size_t>(numSamples));
    auto scratchContext = fad::engine::ProcessContextReplacing<float>(scratchBlock);
    processConfigSlots(configs[static_cast<size_t>(acquiredConfigIndex)],
                       scratchContext,
                       scratchBlock,
                       secondaryTailTransportBpm.load(std::memory_order_acquire));
    sanitizeBlock(scratchBlock);

    const auto& retiredConfig = configs[static_cast<size_t>(acquiredConfigIndex)];
    applyTransitionDelay(retiredConfig, scratchBlock,
                         retiredConfig.transitionHistoryDelaySamples, true);

    const auto samplesToMix = std::min(remaining, numSamples);
    const auto silenceWindow =
        secondaryTailSilenceWindowSamples.load(std::memory_order_acquire);
    const auto previousSilentSamples =
        secondaryTailSilentSamples.load(std::memory_order_acquire);
    const auto retiredPeak = measurePeakAbs(scratchBlock, samplesToMix);
    const auto silentSamplesAfterBlock = retiredPeak <= retiredTailSilenceThreshold
        ? std::min(silenceWindow, static_cast<int>(std::min<int64_t>(
                         std::numeric_limits<int>::max(),
                         static_cast<int64_t>(previousSilentSamples) + samplesToMix)))
        : 0;
    secondaryTailSilentSamples.store(silentSamplesAfterBlock, std::memory_order_release);
    for (int channel = 0; channel < numChannels; ++channel)
    {
        const auto* retiredTail = publicationFadeScratchBuffer.getReadPointer(channel);
        auto* activeOutput = outputBlock.getChannelPointer(static_cast<size_t>(channel));
        for (int sample = 0; sample < samplesToMix; ++sample)
            activeOutput[sample] = utils::sanitizeFinite(activeOutput[sample]
                                                         + retiredTail[sample]);
    }

    const auto remainingAfterBlock = std::max(0, remaining - numSamples);
    secondaryTailRemainingSamples.store(remainingAfterBlock, std::memory_order_release);
    if (remainingAfterBlock == 0
        || (silenceWindow > 0 && silentSamplesAfterBlock >= silenceWindow))
    {
        finishSecondaryRetirement();
    }
}

void DynamicFxChainCore::finishPublicationRetirement() noexcept
{
    publicationFadeConfigIndex.store(-1, std::memory_order_release);
    publicationWarmupRemaining.store(0, std::memory_order_release);
    publicationFadeRemaining.store(0, std::memory_order_release);
    publicationFadeTotal.store(0, std::memory_order_release);
    publicationOldLatencySamples.store(0, std::memory_order_release);
    publicationNewLatencySamples.store(0, std::memory_order_release);
    publicationTailRemainingSamples.store(0, std::memory_order_release);
    publicationTailSilentSamples.store(0, std::memory_order_release);
    publicationTailSilenceWindowSamples.store(0, std::memory_order_release);
    inactiveProcessorCleanupPending.store(true, std::memory_order_release);
}

void DynamicFxChainCore::finishSecondaryRetirement() noexcept
{
    secondaryTailConfigIndex.store(-1, std::memory_order_release);
    secondaryTailRemainingSamples.store(0, std::memory_order_release);
    secondaryTailSilentSamples.store(0, std::memory_order_release);
    secondaryTailSilenceWindowSamples.store(0, std::memory_order_release);
    inactiveProcessorCleanupPending.store(true, std::memory_order_release);
}

// ── processWithDiagnostics ─────────────────────────────────────────
void DynamicFxChainCore::processWithDiagnostics(
    fad::engine::AudioBufferView buffer,
    std::vector<diagnostics::CpuMeter>& meters,
    diagnostics::ClipDetector& clip)
{
    fad::engine::rt::ScopedNoDenormals noDenormals;

    if (bypassed.load(std::memory_order_relaxed))
        return;

    if (buffer.getNumSamples() == 0)
        return;


    if (! tryEnterProcessCallback())
    {
        rebuildPending.store(true, std::memory_order_release);
        return;
    }
    const auto leaveCallback = fad::engine::detail::ScopeExit{
        [this] { leaveProcessCallback(); }};

    hasProcessedSincePrepare.store(true, std::memory_order_release);

    if (buffer.getNumChannels() == 0)
    {
        advanceZeroChannelBlock(static_cast<size_t>(buffer.getNumSamples()));
        return;
    }

    const auto scratchCapacity = publicationFadeScratchBuffer.getNumSamples();
    if (scratchCapacity > 0 && buffer.getNumSamples() > scratchCapacity)
    {
        const auto* savedSources = nativeModulationSources;
        const auto savedCount = nativeModulationSourceCount;
        const auto restoreSources = fad::engine::detail::ScopeExit{[this, savedSources, savedCount] {
            nativeModulationSources = savedSources; nativeModulationSourceCount = savedCount;
        }};

        for (int offset = 0; offset < buffer.getNumSamples(); offset += scratchCapacity)
        {
            const auto chunkSamples = std::min(scratchCapacity, buffer.getNumSamples() - offset);
            auto chunk = buffer.subview(offset, chunkSamples);
            nativeModulationSourceCount = std::max(0, savedCount - offset);
            nativeModulationSources = savedSources != nullptr && nativeModulationSourceCount > 0 ? savedSources + offset : nullptr;
            processWithDiagnostics(chunk, meters, clip);
        }
        return;
    }

    ReadableConfigGuard readableConfig(*this);
    if (! readableConfig.isValid())
    {
        rebuildPending.store(true, std::memory_order_release);
        return;
    }

    const auto& config = readableConfig.get();
    const auto currentTransportBpm = transportBpm.load(std::memory_order_acquire);

    fad::engine::AudioBlock<float> block(buffer);
    auto context = fad::engine::ProcessContextReplacing<float>(block);
    sanitizeBlock(block);
    int publicationTransitionSamples = 0;
    int publicationWarmupRemainingBefore = 0;
    int publicationFadeRemainingBefore = 0;
    int publicationFadeTotalBefore = 0;
    const auto fadeSourceConfigIndex =
        preparePublicationFadeScratch(readableConfig.getIndex(),
                                      block,
                                      publicationTransitionSamples,
                                      publicationWarmupRemainingBefore,
                                      publicationFadeRemainingBefore,
                                      publicationFadeTotalBefore);

    processConfigSlots(config, context, block, currentTransportBpm, &meters, &clip);

    if (fadeSourceConfigIndex >= 0)
    {
        renderAndBlendPublicationFade(readableConfig.getIndex(),
                                      fadeSourceConfigIndex,
                                      publicationTransitionSamples,
                                      publicationWarmupRemainingBefore,
                                      publicationFadeRemainingBefore,
                                      publicationFadeTotalBefore,
                                      block,
                                      currentTransportBpm);
    }
    else
    {
        applyTransitionDelay(config, block, 0, true);
        renderRetiredTail(readableConfig.getIndex(), block);
    }
    renderSecondaryRetiredTail(readableConfig.getIndex(), block);
    sanitizeBlock(block);
}

void DynamicFxChainCore::processSlotWithOptionalFade(const ChainConfig::Slot& slot,
                                                 BaseFxProcessorCore& processor,
                                                 fad::engine::ProcessContextReplacing<float>& context,
                                                 fad::engine::AudioBlock<float>& outputBlock)
{
    const auto blockChannels = static_cast<int>(outputBlock.getNumChannels());
    const auto numSamples = static_cast<int>(outputBlock.getNumSamples());
    const auto fadeRemaining = slot.fadeInRemaining.load(std::memory_order_acquire);
    const auto fadeTotal = slot.fadeInTotal.load(std::memory_order_acquire);
    const auto fadeSamples = (fadeRemaining > 0
                              && fadeTotal > 0
                              && blockChannels <= slotFadeScratchBuffer.getNumChannels())
        ? std::min({numSamples, fadeRemaining, slotFadeScratchBuffer.getNumSamples()})
        : 0;

    for (int channel = 0; channel < blockChannels && fadeSamples > 0; ++channel)
        slotFadeScratchBuffer.copyFrom(channel,
                                       0,
                                       outputBlock.getChannelPointer(static_cast<size_t>(channel)),
                                       fadeSamples);

    const auto& activeSlots = configs[static_cast<size_t>(activeIndex.load(std::memory_order_acquire))].slots;
    const auto activeSlotIterator = std::find_if(activeSlots.begin(), activeSlots.end(),
        [&slot](const auto& candidate) { return &candidate == &slot; });
    const bool activeSlot = activeSlotIterator != activeSlots.end();
    auto* eq = activeSlot && eqCurveInput && slot.type==FxType::eq3Band
        ? static_cast<Eq3BandFxCore*>(&processor) : nullptr;
    const auto* eqCurve = eq ? &(*eqCurveInput)[parameters::eqCurveOwnerBegin
        +size_t(std::distance(activeSlots.begin(),activeSlotIterator))] : nullptr;
    if(eqCurve && !eqCurve->enabled && !eqCurve->history)
    {
        eq->setNodeCurve(parameters::CurveNodes{});
        eqCurve=nullptr;eq=nullptr;
    }
    const bool modulated = activeSlot && processor.supportsNativeModulation()
        && nativeModulationRoutes != nullptr && nativeModulationSources != nullptr
        && nativeModulationSourceCount >= numSamples;
    if (modulated || eqCurve) {
        for (size_t offset = 0; offset < outputBlock.getNumSamples();) {
            processor.clearNativeModulation();
            const auto sources = nativeModulationSources && nativeModulationSourceCount>=numSamples
                ? nativeModulationSources[offset] : modulation::SourceValues{};
            processor.setNativeVoiceActivity(true, sources.nativeVoiceActive);
            if(modulated)for (const auto& route : nativeModulationRoutes->slots) {
                if (route.nativeTarget < 0 || !route.hasSource()) continue;
                const auto value = modulation::ModMatrix::resolveRouteSourceValue(route, sources) * route.amount;
                if (std::isfinite(value))
                {
                    if (value != 0.0f)
                    {
                        unsigned observed = 0;
                        if ((slot.type == FxType::reverb && route.nativeTarget == 81)
                            || (slot.type == FxType::delay && route.nativeTarget == 124))
                            observed |= wetTailModulation;
                        if (slot.type == FxType::reverb && route.nativeTarget == 83)
                            observed |= reverbTimeModulation;
                        if (observed != 0 && (slot.tailModulationSeen.load(std::memory_order_relaxed) & observed) != observed)
                            slot.tailModulationSeen.fetch_or(observed, std::memory_order_release);
                    }
                    processor.addNativeModulation(route.nativeTarget, value);
                }
            }
            if(eqCurve && eq->nodeCurveUpdateDue())eq->setNodeCurve(modulation::modulatedCurve(*eqCurve,sources.nativeLfos));
            auto count = std::min(static_cast<size_t>(std::max(1, processor.nativeModulationQuantum())), outputBlock.getNumSamples() - offset);
            if(eq)count=std::min(count, size_t(eq->nodeCurveQuantum()));
            if (processor.needsNativeVoiceActivity())
                for (size_t i = 1; i < count; ++i)
                    if (nativeModulationSources[offset + i].nativeVoiceActive != sources.nativeVoiceActive)
                    {
                        count = i;
                        break;
                    }
            auto chunk = outputBlock.getSubBlock(offset, count);
            fad::engine::ProcessContextReplacing<float> chunkContext(chunk);
            processor.process(chunkContext);
            offset += count;
        }
        processor.setNativeVoiceActivity(false, false);
    } else {
        // Retired graphs retain their final offsets while rendering old tails.
        processor.setNativeVoiceActivity(false, false);
        if (activeSlot) processor.clearNativeModulation();
        processor.process(context);
    }

    if (fadeSamples <= 0)
        return;

    const auto alreadyFadedSamples = fadeTotal - fadeRemaining;
    const auto invFadeTotal = 1.0f / static_cast<float>(fadeTotal);
    for (int channel = 0; channel < blockChannels; ++channel)
    {
        const auto* dry = slotFadeScratchBuffer.getReadPointer(channel);
        auto* wet = outputBlock.getChannelPointer(static_cast<size_t>(channel));
        for (int sample = 0; sample < fadeSamples; ++sample)
        {
            const auto wetGain = std::clamp(static_cast<float>(alreadyFadedSamples + sample) * invFadeTotal, 0.0f, 1.0f);
            wet[sample] = utils::sanitizeFinite(dry[sample] * (1.0f - wetGain) + wet[sample] * wetGain);
        }
    }

    slot.fadeInRemaining.store(std::max(0, fadeRemaining - fadeSamples),
                               std::memory_order_release);
}

} // namespace fadmodel1::dsp::fx
