#include <fad/engine/diagnostics/CpuMeter.h>

namespace fadmodel1::dsp::diagnostics
{
CpuMeter::CpuMeter() noexcept
{
}

void CpuMeter::beginMeasurement() noexcept
{
    measurementRunning = true;
}

float CpuMeter::endMeasurement() noexcept
{
    if (! measurementRunning)
        return smoothedMicroseconds;

    measurementRunning = false;
    observedProcessing = true;

    // Audio-thread diagnostics deliberately do not sample wall clocks or high
    // resolution timers. Timing fields therefore remain "unavailable" instead
    // of publishing synthetic microseconds as if they were measured CPU cost.
    const auto durationUs = 0.0f;
    smoothedMicroseconds += smoothingAlpha * (durationUs - smoothedMicroseconds);
    return durationUs;
}

float CpuMeter::getSmoothedMicroseconds() const noexcept
{
    return smoothedMicroseconds;
}

bool CpuMeter::hasObservedProcessing() const noexcept
{
    return observedProcessing;
}

bool CpuMeter::hasTimingData() const noexcept
{
    return false;
}

void CpuMeter::setSmoothingCoefficient(float alpha) noexcept
{
    smoothingAlpha = alpha >= 0.0f && alpha <= 1.0f ? alpha : 0.1f;
}
} // namespace fadmodel1::dsp::diagnostics
