#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <numbers>
#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/filters/TptStateVariable.h>
#include <fad/engine/filters/FractionalDelay.h>
#include <fad/engine/filters/MonoChorus.h>
#include <fad/engine/filters/PreparedAllpass2x.h>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/math/RuntimeRange.h>

#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/fx/NativeWetModulation.h>
#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fadmodel1::dsp::fx
{
class ChorusFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;
    void clearNativeModulation() noexcept override { nativeWetModulation.clear(); }
    void addNativeModulation(int target, float offset) noexcept override { nativeWetModulation.add(target, offset); }
    bool supportsNativeModulation() const noexcept override { return nativeDual; }
    int nativeModulationQuantum() const noexcept override { return nativeWetModulation.quantum(); }

    void setSnapshot(const parameters::ChorusSnapshot& newSnapshot) noexcept;

private:
    float wideRateHz=-1;
    NativeWetModulationClock<116> nativeWetModulation;
    void processBlockInternal(fad::engine::ProcessContextReplacing<float>& context);
    float applyParameters();
    void processNative(fad::engine::AudioBlock<float>& block);
    void processChorus(size_t channel, fad::engine::ProcessContextReplacing<float>& context) noexcept;
    void applyModeTransition(fad::engine::AudioBlock<float>& block);
    void updateOutputFilters(float hiCutHz, float loCutHz) noexcept;

    parameters::ChorusSnapshot snapshot;
    std::array<fad::engine::filters::FractionalDelay<float, fad::engine::filters::DelayInterpolation::lagrange3>, 4> nativeDelays;
    std::array<fad::engine::filters::TptStateVariable<float>, 4> nativeFilters;
    std::array<float, 4> nativeFeedback{};
    std::array<float,2> nativeWetPrevious{};
    std::array<std::array<float,2>,2> nativeAlignment{};
    size_t nativeAlignmentIndex = 0;
    std::unique_ptr<fad::engine::filters::Oversample2x> nativeOversampling;
    fad::engine::math::LinearSmoother<float> nativeDelay2Ms, nativeDepthMs;
    bool nativeDual = false, activeNativeDual = false, activeWideRate = false, hasProcessed = false;
    int modeFadeRemaining = 0, modeFadeLength = 144;
    std::array<float,2> lastOutput{}, modeFadeStart{};
    double nativePhase = .5, nativeInitialPhase = .5;
    int nativeTailSamples = 0;
    fad::engine::detail::PlanarBuffer dryBuffer;
    std::array<fad::engine::filters::MonoChorus<float>, 2> choruses;
    std::array<fad::engine::filters::MonoChorus<double>,2> wideChoruses;
    std::array<double,16> wideScratch;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputHighPass;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputLowPass;
    fad::engine::math::LinearSmoother<float> rateHz;
    fad::engine::math::LinearSmoother<float> depth;
    fad::engine::math::LinearSmoother<float> centreDelayMs;
    fad::engine::math::LinearSmoother<float> feedback;
    fad::engine::math::LinearSmoother<float> mix;
    fad::engine::math::LinearSmoother<float> nativeMixBlend;
    float appliedDryGain = 1.0f;
    float appliedWetGain = 0.0f;
    fad::engine::math::LinearSmoother<float> highCutHz;
    fad::engine::math::LinearSmoother<float> lowCutHz;
    double sampleRate = 0.0;
    int numChannels = 0;
    int parameterQuantumSamplesRemaining = 0;
    float appliedMix = 0.0f;
    std::atomic<bool> bypassed{false};
};
}
