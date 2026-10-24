#include <fad/engine/parameters/ParameterTypes.h>
// Copyright (c) 2024-2026 阿勒泰自由意文化服务有限责任公司 (FAD Records). All rights reserved.
// Licensed under the openFAD 0.1 or a separate Commercial License. See LICENSE.

#pragma once

#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/fx/FxBackend.h>
#include <fad/engine/fx/FxFactory.h>
#include <fad/engine/parameters/ParameterSnapshot.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/fx/FxCatalog.h>
#include <fad/engine/parameters/FxRouting.h>
#include <fad/engine/parameters/CurveNodes.h>
#include <fad/engine/diagnostics/CpuMeter.h>
#include <fad/engine/diagnostics/ClipDetector.h>
#include <fad/engine/modulation/ModMatrix.h>

namespace fadmodel1::dsp::fx
{

class DynamicFxChainCore : public BaseFxProcessorCore, public FxBackend
{
public:
    using BaseFxProcessorCore::getGainReductionDbForUi;
    // Audio-thread, callback-scoped view of the accepted parameter snapshot.
    void setEqCurveInput(const std::array<parameters::CurveNodes,parameters::curveOwnerCount>* curves) noexcept override
    { eqCurveInput=curves; }
    void setNativeModulationInput(const parameters::ModulationSnapshot* routes,
                                  const modulation::SourceValues* sources, int count) noexcept override
    { nativeModulationRoutes=routes;nativeModulationSources=sources;nativeModulationSourceCount=count; }
    class ScopedBatchUpdate
    {
    public:
        explicit ScopedBatchUpdate(DynamicFxChainCore& owner,
                                   bool requireExplicitPublicationAcceptance = false) noexcept;
        ~ScopedBatchUpdate();

        ScopedBatchUpdate(const ScopedBatchUpdate&) = delete;
        ScopedBatchUpdate& operator=(const ScopedBatchUpdate&) = delete;

    private:
        DynamicFxChainCore& chain;
    };

    DynamicFxChainCore();

    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void process(fad::engine::AudioBufferView view) override
    {
        fad::engine::AudioBlock<float> block(view);
        fad::engine::ProcessContextReplacing<float> context(block);
        process(context);
    }
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;
    void setTransportBpm(double bpm) override;

    void setSlotCount(int count) override;
    void setSlotType(int slotIndex, FxType type) override;
    void setSlotOrder(const std::vector<int>& order);
    void setSlotOrder(const std::array<int, parameters::ids::maxFxSlots>& order, int activeSlotCount) override;
    void setSlotEnabled(int slotIndex, bool enabled) override;
    void setSlotRouting(int slotIndex, int stage, int lane) override;
    void setSlotParam(int slotIndex,
                      int paramIndex,
                      float value01,
                      bool updateReadableActiveProcessor = true) override;

    void beginBatchUpdate(bool requireExplicitPublicationAcceptance = false) noexcept override;
    void endBatchUpdate() noexcept override;
    uint64_t getLatestDesiredStateSequence() const noexcept override;
    bool tryAcceptPreparedState(uint64_t expectedSequence) noexcept override;
    bool tryAcceptLatestPreparedState() noexcept override;
    // Audio-thread-only whole callback boundary. Inner render chunks cannot
    // retry an acceptance after this callback captured its snapshot.
    void beginAudioCallback() noexcept override { audioCallbackActive = true; audioAcceptanceOpen = true; }
    void closeAudioAcceptanceBoundary() noexcept override { audioAcceptanceOpen = false; }
    void endAudioCallback() noexcept override { audioCallbackActive = false; }
    bool isAudioCallbackActive() const noexcept override { return audioCallbackActive; }

    // PDC: audible FX latency; graph transitions expose max(old, new).
    int getActiveLatencySamples() noexcept override;
    // UI/message-thread only; protects active-bank lifetime and reads only
    // lock-free meter publications. -1 denotes unavailable/non-compressor.
    std::array<float, parameters::ids::maxFxSlots> getGainReductionDbForUi() noexcept override;
    static int maximumPublicationAlignmentSamples(const fad::engine::ProcessSpec& spec) noexcept;
    bool hasPendingRebuildForControlThread() const noexcept override;
    void tryApplyPendingRebuildOnControlThread() noexcept override;

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    // Test hooks: expose minimal internal counters without access-control hacks.
    int testAcquireReadableConfigIndex() const noexcept;
    int testGetActiveConfigIndex() const noexcept { return activeIndex.load(std::memory_order_acquire); }
    void testReleaseReadableConfigIndex(int index) const noexcept;
    int testGetConfigReaderCount(int index) const noexcept;
    int testGetConfigActiveSlotCount(int index) const noexcept;
    int testGetAllocatedProcessorCount(FxType type) const noexcept;
    int testGetCachedParamCount(int slotIndex) const noexcept;
    float testGetCachedParamValue(int slotIndex, int paramIndex) const noexcept;
    void testSetOversizedChunkBoundaryHook(std::function<void()> hook)
    {
        oversizedChunkBoundaryHook = std::move(hook);
    }
    void testSetTailPromotionBoundaryHook(std::function<void(bool)> hook)
    {
        tailPromotionBoundaryHook = std::move(hook);
    }
#endif

    void processWithDiagnostics(
        fad::engine::AudioBufferView buffer,
        std::vector<diagnostics::CpuMeter>& meters,
        diagnostics::ClipDetector& clip) override;

private:
    const std::array<parameters::CurveNodes,parameters::curveOwnerCount>* eqCurveInput=nullptr;
    static constexpr int maxParamsPerSlot = parameters::ids::maxFxParamsPerSlot + 3;

    struct DesiredStateSnapshot
    {
        uint64_t sequence = 0;
        int slotCount = 0;
        std::array<FxType, parameters::ids::maxFxSlots> types{};
        std::array<bool, parameters::ids::maxFxSlots> enabled{true, true, true, true, true, true, true, true, true, true};
        std::array<int, parameters::ids::maxFxSlots> order{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        std::array<int, parameters::ids::maxFxSlots> routingStages{}, routingLanes{};
        int orderCount = 0;
        std::array<std::array<float, maxParamsPerSlot>, parameters::ids::maxFxSlots> params{};
        std::array<int, parameters::ids::maxFxSlots> paramCount{};
        bool requiresExplicitPublicationAcceptance = false;
    };

    struct ChainConfig
    {
        struct Slot
        {
            FxType type = FxType::none;
            bool enabled = true;
            mutable std::atomic<int> fadeInRemaining{0};
            mutable std::atomic<int> fadeInTotal{0};
            // Audio publishes whether this generation has processed routed
            // Wet or mode-dependent time/decay; retirement reads only these
            // flags, never the running processor's mutable modulation state.
            mutable std::atomic<unsigned> tailModulationSeen{0};
            FxType processorType = FxType::none;
            std::unique_ptr<BaseFxProcessorCore> processor;

            BaseFxProcessorCore* getProcessor(FxType requestedType) noexcept;
            // Intentional const escape for const slot views: callers may only use
            // the processor setters to refresh parameters/bypass on an existing
            // processor. The pointer must not be retained, re-owned, or treated as
            // a lifetime/threading guarantee.
            BaseFxProcessorCore* getProcessor(FxType requestedType) const noexcept;
        };

        std::array<Slot, parameters::ids::maxFxSlots> slots;
        std::array<int, parameters::ids::maxFxSlots> processingOrder{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        int activeSlotCount = 0;
        parameters::FxRoutingPlan routingPlan;
        std::array<int, parameters::ids::maxFxSlots> routingStages{}, routingLanes{};
        mutable std::array<fad::engine::detail::PlanarBuffer, parameters::ids::maxFxSlots> routingHistory;
        mutable std::array<int, parameters::ids::maxFxSlots> routingHistoryPosition{};
        uint64_t desiredStateSequence = 0;
        bool requiresExplicitPublicationAcceptance = false;
        int publicationPreviousIndex = -1;
        int preparedFadeSamples = 0;
        bool preparedNeedsGraphFade = false;
        int preparedOldLatency = 0;
        int preparedNewLatency = 0;
        int preparedTransitionLatency = 0;
        int preparedTailSamples = 0;
        int preparedTailSilenceWindow = 0;
        double preparedTailBpm = 120.0;
        mutable std::array<std::array<std::atomic<uint32_t>, maxParamsPerSlot>, parameters::ids::maxFxSlots> paramBits{};
        mutable std::array<std::atomic<int>, parameters::ids::maxFxSlots> paramCount{};
        mutable fad::engine::detail::PlanarBuffer transitionHistoryBuffer;
        mutable int transitionHistoryWritePosition = 0;
        mutable int transitionHistorySamplesAvailable = 0;
        mutable int transitionHistoryDelaySamples = 0;
        std::atomic<bool> ready{false};
    };

    class ReadableConfigGuard
    {
    public:
        explicit ReadableConfigGuard(DynamicFxChainCore& owner,
                                     bool requireActiveMatch = false) noexcept;
        ~ReadableConfigGuard();

        ReadableConfigGuard(const ReadableConfigGuard&) = delete;
        ReadableConfigGuard& operator=(const ReadableConfigGuard&) = delete;

        const ChainConfig& get() const noexcept;
        int getIndex() const noexcept { return index; }
        bool isValid() const noexcept { return index >= 0; }

    private:
        DynamicFxChainCore& chain;
        int index = -1;
    };

    std::array<ChainConfig, 3> configs;
    std::atomic<int> activeIndex{0};
    // Control prepares this bank; only audio acceptance makes it audible.
    std::atomic<int> preparedIndex{-1};
    bool audioCallbackActive = false;
    bool audioAcceptanceOpen = false;
    mutable std::array<std::atomic<int>, 3> configReaders{};
    std::array<std::atomic<uint64_t>, 3> configEpochs{};
    std::atomic<bool> rebuildWriterInProgress{false};
    std::atomic<uint64_t> acceptedStateSequence{0};
    // Host-visible latency is a published scalar, not a live config-bank query.
    // The audio thread may lower it after a graph transition completes.
    std::atomic<int> publishedLatencySamples{0};
    fad::engine::ProcessSpec currentSpec{};
    std::atomic<bool> bypassed{false};
    std::atomic<bool> rebuildPending{false};
    std::atomic<int> batchUpdateDepth{0};
    std::atomic<double> transportBpm{120.0};
    bool prepared = false;
    std::atomic<bool> hasProcessedSincePrepare{false};
    const parameters::ModulationSnapshot* nativeModulationRoutes = nullptr;
    const modulation::SourceValues* nativeModulationSources = nullptr;
    int nativeModulationSourceCount = 0;
    fad::engine::detail::PlanarBuffer slotFadeScratchBuffer;
    fad::engine::detail::PlanarBuffer publicationFadeScratchBuffer;
    fad::engine::detail::PlanarBuffer zeroChannelAdvanceBuffer;
    fad::engine::detail::PlanarBuffer routingInputBuffer, routingSumBuffer, routingLaneBuffer;
    std::atomic<int> processCallbackReaders{0};
    std::atomic<int> publicationFadeConfigIndex{-1};
    std::atomic<int> publicationWarmupRemaining{0};
    std::atomic<int> publicationFadeRemaining{0};
    std::atomic<int> publicationFadeTotal{0};
    std::atomic<int> publicationOldLatencySamples{0};
    std::atomic<int> publicationNewLatencySamples{0};
    std::atomic<int> publicationTailRemainingSamples{0};
    std::atomic<int> publicationTailSilentSamples{0};
    std::atomic<int> publicationTailSilenceWindowSamples{0};
    std::atomic<double> publicationTailTransportBpm{120.0};
    // The third config bank retains one older tail while a newer graph change
    // crossfades. Further structural edits coalesce until one retired bank is
    // silent, keeping ownership bounded without hard-cutting the oldest tail.
    std::atomic<int> secondaryTailConfigIndex{-1};
    std::atomic<int> secondaryTailRemainingSamples{0};
    std::atomic<int> secondaryTailSilentSamples{0};
    std::atomic<int> secondaryTailSilenceWindowSamples{0};
    std::atomic<double> secondaryTailTransportBpm{120.0};
    std::atomic<bool> tailPromotionInProgress{false};
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    std::function<void()> oversizedChunkBoundaryHook;
    std::function<void(bool)> tailPromotionBoundaryHook;
#endif
    std::atomic<bool> inactiveProcessorCleanupPending{false};

    // Desired state can be written from the audio-thread snapshot path while a
    // deferred control-thread rebuild is pending. Keep storage atomic and copy
    // a stable snapshot before publishing a rebuilt config.
    std::atomic<int> desiredSlotCount{0};
    std::array<std::atomic<int>, parameters::ids::maxFxSlots> desiredTypes{};
    std::array<std::atomic<bool>, parameters::ids::maxFxSlots> desiredEnabled{};
    std::array<std::atomic<int>, parameters::ids::maxFxSlots> desiredOrder{};
    std::array<std::atomic<int>, parameters::ids::maxFxSlots> desiredRoutingStages{}, desiredRoutingLanes{};
    std::atomic<int> desiredOrderCount{0};
    std::atomic<int> desiredStateActiveWriters{0};
    std::atomic<uint64_t> desiredStateSequence{0};
    std::atomic<bool> desiredPublicationRequiresAcceptance{false};

    // Cached param values so they survive processor recreation during rebuild
    std::array<std::array<std::atomic<uint32_t>, maxParamsPerSlot>, parameters::ids::maxFxSlots> cachedParamBits{};
    std::array<std::atomic<int>, parameters::ids::maxFxSlots> cachedParamCount{};

    static uint32_t encodeParamValue(float value) noexcept;
    static float decodeParamValue(uint32_t bits) noexcept;
    int acquireSpecificReadableConfigIndex(int candidate) const noexcept;
    int acquireReadableConfigIndex(bool requireActiveMatch = false) const noexcept;
    void releaseReadableConfigIndex(int index) const noexcept;
    void initialiseProcessorBanks();
    static int configLatencySamples(const ChainConfig& config) noexcept;
    static int routingLaneLatency(const ChainConfig& config, const parameters::FxRoutingLane& lane) noexcept;
    void prepareRoutingHistory(ChainConfig& config, bool hasProcessSpec);
    static int estimateRetiredTailSamples(const ChainConfig& config,
                                          const fad::engine::ProcessSpec& spec,
                                          double transportBpm,
                                          int& silenceWindowSamples) noexcept;
    void beginDesiredStateWrite() noexcept;
    void endDesiredStateWrite() noexcept;
    bool copyDesiredStateSnapshot(DesiredStateSnapshot& snapshot) const noexcept;
    bool tryEnterProcessCallback() noexcept;
    void leaveProcessCallback() noexcept;
    void advanceZeroChannelBlock(size_t numSamples);
    bool tryRebuildChain() noexcept;
    bool publishPreparedConfig(int candidate) noexcept;
    void reclaimInactiveProcessorBanksOnControlThread() noexcept;
    void requestRebuild() noexcept;
    void processConfigSlots(const ChainConfig& config,
                            fad::engine::ProcessContextReplacing<float>& context,
                            fad::engine::AudioBlock<float>& outputBlock,
                            double currentTransportBpm,
                            std::vector<diagnostics::CpuMeter>* meters = nullptr,
                            diagnostics::ClipDetector* clip = nullptr);
    static void resetTransitionHistory(ChainConfig& config) noexcept;
    static void applyTransitionDelay(const ChainConfig& config,
                                     fad::engine::AudioBlock<float>& block,
                                     int delaySamples,
                                     bool preserveInputUntilPrimed) noexcept;
    int preparePublicationFadeScratch(int activeConfigIndex,
                                      fad::engine::AudioBlock<float>& outputBlock,
                                      int& transitionSamples,
                                      int& warmupRemaining,
                                      int& fadeRemaining,
                                      int& fadeTotal);
    void renderAndBlendPublicationFade(int activeConfigIndex,
                                       int fadeConfigIndex,
                                       int transitionSamples,
                                       int warmupRemaining,
                                       int fadeRemaining,
                                       int fadeTotal,
                                       fad::engine::AudioBlock<float>& outputBlock,
                                       double currentTransportBpm);
    void renderRetiredTail(int activeConfigIndex,
                           fad::engine::AudioBlock<float>& outputBlock);
    void renderSecondaryRetiredTail(int activeConfigIndex,
                                    fad::engine::AudioBlock<float>& outputBlock);
    void finishPublicationRetirement() noexcept;
    void finishSecondaryRetirement() noexcept;
    void processSlotWithOptionalFade(const ChainConfig::Slot& slot,
                                     BaseFxProcessorCore& processor,
                                     fad::engine::ProcessContextReplacing<float>& context,
                                     fad::engine::AudioBlock<float>& outputBlock);
};

} // namespace fadmodel1::dsp::fx
