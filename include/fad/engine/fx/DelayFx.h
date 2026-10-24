#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/filters/TptStateVariable.h>
#include <fad/engine/filters/FractionalDelay.h>

#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fadmodel1::dsp::fx
{
class DelayFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;
    void clearNativeModulation() noexcept override { nativeWetOffset = 0.0f; }
    void addNativeModulation(int target, float offset) noexcept override;
    bool supportsNativeModulation() const noexcept override { return nativeTimeMode != 0; }
    int nativeModulationQuantum() const noexcept override
    { return filterUpdateCountdown > 0 ? filterUpdateCountdown : 16; }
    bool supportsRuntimeStateCopy() const noexcept override { return true; }
    void copyRuntimeStateFrom(const BaseFxProcessorCore& other) override;

    void setSnapshot(const parameters::DelaySnapshot& newSnapshot) noexcept;
    void setTransportBpm(double newTempoBpm) noexcept override;
    static float delayIndexToBeats(int index) noexcept;

private:
    float wideLeftMs=-1, wideRightMs=-1;
    void invalidateDelayHistory() noexcept;
    float popValidDelaySample(int channel, float delayInSamples) noexcept;
    void advanceValidDelayHistory() noexcept;
    void updateNativeFilter(float frequency, float bandwidth) noexcept;
    float processNativeFilter(int channel, float input) noexcept;
    void initialiseFilterTypes() noexcept;
    void updateFilters(float lowCutHz, float highCutHz) noexcept;
    void recoverFromInvalidSample(fad::engine::AudioBlock<float>& outputBlock,
                                  int sampleIndex,
                                  float safeLeft,
                                  float safeRight,
                                  bool delayReadAdvanced) noexcept;

    parameters::DelaySnapshot snapshot;
    fad::engine::filters::FractionalDelay<float> delayLine;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> feedbackHighPass;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> feedbackLowPass;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputHighPass;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputLowPass;
    fad::engine::math::LinearSmoother<float> feedbackAmount;
    fad::engine::math::LinearSmoother<float> wetMix;
    fad::engine::math::LinearSmoother<float> delaySamples;
    fad::engine::math::LinearSmoother<float> rightDelaySamples;
    fad::engine::math::LinearSmoother<float> pingPongMix;
    int nativeTimeMode = 0; // 0 legacy, 1 seconds, 2 beats
    float nativeLeftTime = 0.0f, nativeRightTime = 0.0f;
    bool sameChannelFeedback = false;
    fad::engine::math::LinearSmoother<float> nativeFilterFrequency, nativeFilterBandwidth, nativeProfileMix;
    std::array<std::array<float,2>,2> nativeFilterStates{};
    float nativeB0=0, nativeA1=0, nativeA2=0;
    float nativeLastFrequency=-1, nativeLastBandwidth=-1;
    float nativeMixInput=-1, nativeWetGain=0;
    float nativeWetOffset = 0.0f, appliedNativeWetOffset = 0.0f;
    fad::engine::math::LinearSmoother<float> highCutHz;
    fad::engine::math::LinearSmoother<float> lowCutHz;
    double sampleRate = 0.0;
    double tempoBpm = 120.0;
    int numChannels = 0;
    int validDelaySamples = 0;
    int filterUpdateCountdown = 0;
    std::atomic<bool> bypassed{false};
};
}
