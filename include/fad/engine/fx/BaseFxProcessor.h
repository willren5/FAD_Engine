#pragma once
#include <fad/engine/AudioBlock.h>

namespace fadmodel1::dsp::fx
{
// FAD processor contract using non-owning engine types. Native algorithms
// and state are implemented in the shared Core, without a host wrapper.
class BaseFxProcessorCore
{
public:
    virtual ~BaseFxProcessorCore() = default;
    virtual void prepare(const fad::engine::ProcessSpec&) = 0;
    virtual void process(fad::engine::ProcessContextReplacing<float>&) = 0;
    virtual void reset() = 0;
    virtual void setBypassed(bool) = 0;
    virtual void setGenericParam(int, float) = 0;
    virtual void clearNativeModulation() noexcept {}
    virtual void addNativeModulation(int, float) noexcept {}
    virtual int nativeModulationQuantum() const noexcept { return 16; }
    virtual bool supportsNativeModulation() const noexcept { return false; }
    virtual bool needsNativeVoiceActivity() const noexcept { return false; }
    virtual void setNativeVoiceActivity(bool, bool) noexcept {}
    virtual void setTransportBpm(double) {}
    virtual int getLatencySamples() const noexcept { return 0; }
    virtual float getGainReductionDbForUi() const noexcept { return -1; }
    virtual bool supportsRuntimeStateCopy() const noexcept { return false; }
    virtual void copyRuntimeStateFrom(const BaseFxProcessorCore&) {}
};
}
