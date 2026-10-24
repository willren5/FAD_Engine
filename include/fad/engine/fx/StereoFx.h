#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/filters/TptStateVariable.h>

#include <fad/engine/fx/BaseFxProcessor.h>

namespace fadmodel1::dsp::fx
{
class StereoFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;

    void clearNativeModulation() noexcept override { nativeModulation.fill(0); }
    void addNativeModulation(int target, float offset) noexcept override {
        const auto index = target == 147 ? 0 : target == 149 ? 1 : target == 153 ? 2 : target == 148 ? 3 : target == 152 ? 4 : -1;
        if (index >= 0 && std::isfinite(offset)) {
            auto& value = nativeModulation[static_cast<size_t>(index)];
            value = std::clamp(value + offset, -32.0f, 32.0f);
        }
    }
    bool supportsNativeModulation() const noexcept override { return nativeHyper; }
    int nativeModulationQuantum() const noexcept override { return nativeModulationRemaining > 0 ? nativeModulationRemaining : 16; }
private:
    std::array<float, 5> nativeModulation{}, appliedNativeModulation{};
    int nativeModulationRemaining = 0;
    bool nativeModulationStarted = false;
    void processActiveBlock(fad::engine::ProcessContextReplacing<float>& context);
    void processNativeHyper(fad::engine::AudioBlock<float>& block) noexcept;

    fad::engine::detail::PlanarBuffer dryBuffer;
    fad::engine::math::LinearSmoother<float> width;
    fad::engine::math::LinearSmoother<float> pan;
    fad::engine::math::LinearSmoother<float> midGainDb;
    fad::engine::math::LinearSmoother<float> sideGainDb;
    fad::engine::math::LinearSmoother<float> mix;
    fad::engine::detail::PlanarBuffer dimensionHistory;
    std::array<double, 4> dimensionDelays{};
    double dimensionPhase = 0.0, dimensionSmoothing = 0.0, dimensionInputGain = 0.0;
    float dimensionSize = 0.0f;
    fad::engine::math::LinearSmoother<float> dimensionMix;
    int dimensionWrite = 0;
    fad::engine::detail::PlanarBuffer hyperHistory;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> hyperHighPass;
    fad::engine::math::LinearSmoother<float> hyperWet, hyperRate, hyperDetune, nativeBlend;
    std::array<std::array<double, 7>, 2> hyperPhase{};
    std::array<float, 14> savedHyperPhases{};
    int hyperWrite = 0, hyperVoices = 0, hyperWarmupRemaining = 0, hyperIdleLimit = 0;
    bool nativeHyper = false;
    double sampleRate = 0.0;
    int numChannels = 0;
    bool parametersPrimed = false;
    std::atomic<bool> bypassed{false};
};
}
