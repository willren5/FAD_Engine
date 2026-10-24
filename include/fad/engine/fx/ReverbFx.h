#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/filters/TptStateVariable.h>
#include <fad/engine/filters/FractionalDelay.h>

#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/fx/NativeHallReverb.h>
#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fadmodel1::dsp::fx
{
class ReverbFxCore : public BaseFxProcessorCore
{
public:
    static constexpr size_t networkLineCount = 8;

    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;
    bool supportsRuntimeStateCopy() const noexcept override { return true; }
    void copyRuntimeStateFrom(const BaseFxProcessorCore& other) override;

    void clearNativeModulation() noexcept override { nativeWetOffset = nativeDecayOffset = 0.0f; }
    void addNativeModulation(int target, float offset) noexcept override;
    // Wet modulation is independent of the decay profile and Hall/Original mode.
    // A zero base Wet still needs the routed offset in updateMix().
    bool supportsNativeModulation() const noexcept override
    { return true; }
    int nativeModulationQuantum() const noexcept override { return parameterQuantumSamplesRemaining > 0 ? parameterQuantumSamplesRemaining : 16; }

    void setSnapshot(const parameters::ReverbSnapshot& newSnapshot) noexcept;

private:
    void processBlockInternal(fad::engine::ProcessContextReplacing<float>& context);
    void updateOutputFilters(float hiCutHz, float loCutHz) noexcept;
    void updateNetworkState(float currentSize, float currentDamping) noexcept;
    std::array<float, 2> processNetworkSample(float leftIn, float rightIn, float currentWidth) noexcept;
    void refreshAppliedParameters();
    void updatePreDelayTarget() noexcept;
    void updateMix(float wet) noexcept;
    void discardOriginalTail() noexcept;

    parameters::ReverbSnapshot snapshot;
    fad::engine::detail::PlanarBuffer dryBuffer;
    fad::engine::filters::FractionalDelay<float> preDelayLine{9600};
    std::array<fad::engine::filters::FractionalDelay<float>, networkLineCount> networkDelays;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputHighPass;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputLowPass;
    std::array<fad::engine::filters::TptStateVariable<float>, networkLineCount> feedbackHighPass;
    std::array<float, networkLineCount> networkDelaySamples{};
    std::array<float, networkLineCount> feedbackLowpassState{};
    std::array<float, networkLineCount> stereoInputSigns{};
    fad::engine::math::LinearSmoother<float> roomSize;
    fad::engine::math::LinearSmoother<float> damping;
    fad::engine::math::LinearSmoother<float> wetLevel;
    fad::engine::math::LinearSmoother<float> width;
    fad::engine::math::LinearSmoother<float> highCutHz;
    fad::engine::math::LinearSmoother<float> lowCutHz;
    fad::engine::math::LinearSmoother<float> preDelaySamples;
    float feedbackGain = 0.75f;
    // Zero keeps the legacy room-size/decay relationship.
    fad::engine::math::LinearSmoother<float> nativeDecaySeconds, nativeProfileBlend;
    float appliedNativeBlend = 0.0f;
    NativeHallReverb nativeHall;
    fad::engine::math::LinearSmoother<float> hallDecay, hallHiCut, hallLoCut, hallSpinRate, hallSpinDepth, hallBlend;
    float appliedHallBlend = 0.0f;
    // -1 means the network contains only valid samples. Nonnegative counts
    // mask stale reads until fresh samples have overwritten its delay history.
    int originalTailValidSamples = -1;
    float modeNormalized = 0.0f, legacyHallDecayNormalized = 0.0f;
    float extendedPreDelayNormalized = 0.0f;
    std::array<float, networkLineCount> nativeFeedbackGains{};
    float dampingCoefficient = 0.25f;
    int parameterQuantumSamplesRemaining = 0;
    float nativeWetOffset = 0.0f, nativeDecayOffset = 0.0f;
    float appliedWetGain = 0.0f;
    float appliedDryGain = 1.0f;
    float appliedWidth = 1.0f;
    double sampleRate = 0.0;
    int numChannels = 0;
    std::atomic<bool> bypassed{false};
};
}
