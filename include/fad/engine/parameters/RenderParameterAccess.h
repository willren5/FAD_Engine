#pragma once
#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fad::engine
{
// The private parameter publisher retains its mutation, restore-epoch and
// render-batch ownership. This borrowed port exposes only its existing RT
// operations; every begin is a bounded try, and every successful batch ends
// synchronously at the same sample point.
class RenderParameterAccess
{
public:
    virtual ~RenderParameterAccess() = default;
    virtual uint64_t getRenderRestoreEpoch() const noexcept = 0;
    virtual bool tryBeginRenderParameterBatch() noexcept = 0;
    virtual bool writeRenderParameter(int index, float normalized) noexcept = 0;
    virtual float readRenderParameter(int index) const noexcept = 0;
    virtual bool captureRenderParameterBatch(fadmodel1::parameters::ParameterSnapshot&, uint32_t&) noexcept = 0;
    virtual void endRenderParameterBatch() noexcept = 0;
};
}
