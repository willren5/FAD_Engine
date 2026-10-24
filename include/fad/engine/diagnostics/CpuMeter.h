#pragma once

#include <cstdint>

namespace fadmodel1::dsp::diagnostics
{
class CpuMeter
{
public:
    CpuMeter() noexcept;

    void beginMeasurement() noexcept;
    float endMeasurement() noexcept;

    float getSmoothedMicroseconds() const noexcept;
    bool hasObservedProcessing() const noexcept;
    bool hasTimingData() const noexcept;
    void setSmoothingCoefficient(float alpha) noexcept;

private:
    bool measurementRunning = false;
    bool observedProcessing = false;
    float smoothedMicroseconds = 0.0f;
    float smoothingAlpha = 0.1f;
};
} // namespace fadmodel1::dsp::diagnostics
