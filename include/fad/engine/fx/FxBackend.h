#pragma once
#include <fad/engine/AudioView.h>
#include <fad/engine/fx/FxCatalog.h>
#include <fad/engine/diagnostics/CpuMeter.h>
#include <fad/engine/diagnostics/ClipDetector.h>
#include <fad/engine/modulation/ModMatrix.h>
#include <vector>

namespace fadmodel1::dsp::fx
{
// One graph owns publication/acceptance, latency and retired tails. T08 uses
// the private graph through this port; T09 supplies the independent graph.
// The port never transfers an audio buffer, asset owner, or host object.
class FxBackend
{
public:
    class ScopedBatchUpdate
    {
    public:
        explicit ScopedBatchUpdate(FxBackend& backend, bool explicitAcceptance = false) noexcept : backend_(backend)
        { backend_.beginBatchUpdate(explicitAcceptance); }
        ~ScopedBatchUpdate() { backend_.endBatchUpdate(); }
        ScopedBatchUpdate(const ScopedBatchUpdate&) = delete;
        ScopedBatchUpdate& operator=(const ScopedBatchUpdate&) = delete;
    private:
        FxBackend& backend_;
    };
    virtual ~FxBackend() = default;
    virtual void prepare(const fad::engine::ProcessSpec&) = 0;
    virtual void reset() = 0;
    virtual void process(fad::engine::AudioBufferView) = 0;
    virtual void processWithDiagnostics(fad::engine::AudioBufferView, std::vector<diagnostics::CpuMeter>&, diagnostics::ClipDetector&) = 0;
    virtual void setEqCurveInput(const std::array<parameters::CurveNodes, parameters::curveOwnerCount>*) noexcept = 0;
    virtual void setNativeModulationInput(const parameters::ModulationSnapshot*, const modulation::SourceValues*, int) noexcept = 0;
    virtual void setSlotCount(int) = 0;
    virtual void setSlotType(int, FxType) = 0;
    virtual void setSlotOrder(const std::array<int, parameters::ids::maxFxSlots>&, int) = 0;
    virtual void setSlotEnabled(int, bool) = 0;
    virtual void setSlotRouting(int, int, int) = 0;
    virtual void setSlotParam(int, int, float, bool updateReadableActiveProcessor = true) = 0;
    virtual void setTransportBpm(double) = 0;
    virtual void beginBatchUpdate(bool explicitAcceptance = false) noexcept = 0;
    virtual void endBatchUpdate() noexcept = 0;
    virtual uint64_t getLatestDesiredStateSequence() const noexcept = 0;
    virtual bool tryAcceptPreparedState(uint64_t) noexcept = 0;
    virtual bool tryAcceptLatestPreparedState() noexcept = 0;
    virtual void beginAudioCallback() noexcept = 0;
    virtual void closeAudioAcceptanceBoundary() noexcept = 0;
    virtual void endAudioCallback() noexcept = 0;
    virtual bool isAudioCallbackActive() const noexcept = 0;
    virtual int getActiveLatencySamples() noexcept = 0;
    virtual std::array<float, parameters::ids::maxFxSlots> getGainReductionDbForUi() noexcept = 0;
    virtual bool hasPendingRebuildForControlThread() const noexcept = 0;
    virtual void tryApplyPendingRebuildOnControlThread() noexcept = 0;
};
}
