#pragma once

#include <numbers>
#include <cstdint>

#include <fad/engine/parameters/ParameterSnapshot.h>
#include "NativeLfoCurveRuntime.h"

namespace fadmodel1::dsp::modulation
{
class Lfo final
{
public:
    void prepare(double newSampleRate);
    void reset() noexcept;
    void setRate(float newRateHz) noexcept;
    // Transport sync deliberately restarts the random cycle sequence at the
    // aligned phase; this is the declared reset-per-transport grain.
    void setPhase(float newPhase) noexcept;
    float renderSampleAtOffset(parameters::LfoShape shape, int sampleOffset, bool oneShot = false) const noexcept;
    float renderCurveSampleAtOffset(const float* samples, size_t count, int sampleOffset, bool oneShot = false, const float* endpoint = nullptr, float loopStart = -1.0f) const noexcept;
    float renderPointCurveAtOffset(const NativeLfoCurveRuntime& curve, int sampleOffset, bool oneShot = false) const noexcept;
    float renderNextSample(parameters::LfoShape shape) noexcept;
    double nodePhaseAtOffset(int offset,bool oneShot=false,float loopStart=-1) const noexcept;
    float getRateHz() const noexcept { return static_cast<float>(phaseDelta * sampleRate / (2.0 * std::numbers::pi)); }
    void advance(int numSamples) noexcept;

private:
    static double wrapPhase(double phase) noexcept;

    double sampleRate = 0.0;
    double phaseOffset = 0.0;
    double phaseDelta = 0.0;
    uint64_t samplePosition = 0;
    uint64_t cycleBase = 0;
};
}
