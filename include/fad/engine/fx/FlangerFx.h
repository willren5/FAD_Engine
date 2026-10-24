#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/filters/TptStateVariable.h>
#include <fad/engine/filters/FractionalDelay.h>
#include <fad/engine/math/RuntimeRange.h>

#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/fx/NativeWetModulation.h>

namespace fadmodel1::dsp::fx
{
class FlangerFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;
    bool supportsRuntimeStateCopy() const noexcept override { return true; }
    void copyRuntimeStateFrom(const BaseFxProcessorCore& other) override;
    void clearNativeModulation() noexcept override { nativeWetModulation.clear(); }
    void addNativeModulation(int target, float offset) noexcept override { nativeWetModulation.add(target, offset); }
    bool supportsNativeModulation() const noexcept override { return nativeMixEnabled.load(std::memory_order_relaxed); }
    int nativeModulationQuantum() const noexcept override { return nativeWetModulation.quantum(); }

private:
    float legacyRateHz=.1f;
    float wideRateHz=-1;
    NativeWetModulationClock<103> nativeWetModulation;
    fad::engine::math::LinearSmoother<float> nativeMixBlend;
    std::atomic<bool> nativeMixEnabled{false};
    bool hasProcessedSample = false;
    void processActiveBlock(fad::engine::ProcessContextReplacing<float>& context);
    void updateOutputFilters(float currentLoCutHz, float currentHiCutHz) noexcept;

    std::array<fad::engine::filters::FractionalDelay<float>, 2> delayLines;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> feedbackDcBlockers;
    float lfoPhase = 0.0f;
    double wideLfoPhase = 0;
    std::array<float, 2> feedbackSample{};
    std::array<float, 2> heldLfoValues{};
    int modulationUpdateCountdown = 0;
    int filterUpdateCountdown = 0;

    // Output EQ
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputHP;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputLP;

    // Smoothed params
    fad::engine::math::LinearSmoother<float> rate, depth, manualMs, feedback, mix, hiCutHz, loCutHz;

    fad::engine::detail::PlanarBuffer dryBuffer;
    double sampleRate = 0.0;
    int numChannels = 0;
    std::atomic<bool> bypassed{false};
};
}
