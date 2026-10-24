#pragma once
#include <fad/engine/AudioView.h>

namespace fad::engine
{
// Borrowed preparation/reset hooks for an optional capture task service.
// prepare/reset run only at existing lifecycle boundaries. requestReset must
// perform only the service's bounded atomic request; no worker wait or signal
// is permitted on the render thread. The service outlives RenderEngine.
struct RenderServiceHooks
{
    void* owner = nullptr;
    void (*prepare)(void*, const ProcessSpec&) = nullptr;
    void (*reset)(void*) noexcept = nullptr;
    void (*requestReset)(void*) noexcept = nullptr;
};
}
