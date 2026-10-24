#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/filters/FractionalDelay.h>
#include <fad/engine/math/RuntimeRange.h>

#include <fad/engine/fx/BaseFxProcessor.h>

namespace fadmodel1::dsp::fx
{
class LimiterFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;
    bool supportsRuntimeStateCopy() const noexcept override { return true; }
    void copyRuntimeStateFrom(const BaseFxProcessorCore& other) override;

    // PDC: report FIXED maximum lookahead for stable host compensation.
    // Returning a dynamic value causes DAW PDC to constantly recalculate,
    // producing glitches when the user adjusts the lookahead knob.
    int getLatencySamples() const noexcept override
    {
        if (bypassed.load(std::memory_order_relaxed)) return 0;
        return fixedOutputDelaySamples;
    }

private:
    void processActiveBlock(fad::engine::ProcessContextReplacing<float>& context);
    void resetDelayLines() noexcept;

    fad::engine::filters::FractionalDelay<float> lookaheadL;
    fad::engine::filters::FractionalDelay<float> lookaheadR;
    fad::engine::filters::FractionalDelay<float> detectorDelay;
    float envelope = 1.0f;
    int holdCounter = 0;  // D2 fix: hold envelope during lookahead period

    fad::engine::math::LinearSmoother<float> ceilingDb;
    fad::engine::math::LinearSmoother<float> releaseMs;
    fad::engine::math::LinearSmoother<float> lookaheadMs;
    fad::engine::math::LinearSmoother<float> kneeDb;
    fad::engine::math::LinearSmoother<float> mix;

    double sampleRate = 0.0;
    int numChannels = 0;
    int fixedOutputDelaySamples = 0;
    std::atomic<bool> bypassed{false};
};
}
