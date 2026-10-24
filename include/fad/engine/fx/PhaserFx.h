#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/filters/TptStateVariable.h>
#include <fad/engine/math/RuntimeRange.h>

#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/fx/NativeWetModulation.h>
#include <fad/engine/fx/NativePhaserCore.h>

namespace fadmodel1::dsp::fx
{
class PhaserFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;
    void clearNativeModulation() noexcept override { nativeWetModulation.clear(); nativeOffsets.fill(0.0f); }
    void addNativeModulation(int target, float offset) noexcept override;
    bool supportsNativeModulation() const noexcept override { return nativeMixEnabled.load(std::memory_order_relaxed) || nativeProfile.load(std::memory_order_relaxed); }
    int nativeModulationQuantum() const noexcept override { return nativeProfile.load(std::memory_order_relaxed) ? std::max(1, nativeRemaining) : nativeWetModulation.quantum(); }
    bool needsNativeVoiceActivity() const noexcept override { return nativeProfile.load(std::memory_order_relaxed); }
    void setNativeVoiceActivity(bool known, bool active) noexcept override { activityKnown = known; voiceActive = active; }
    void setTransportBpm(double bpm) override;

private:
    float legacyRateHz=.1f;
    float wideRateHz=-1;
    NativeWetModulationClock<109> nativeWetModulation;
    fad::engine::math::LinearSmoother<float> nativeMixBlend;
    std::atomic<bool> nativeMixEnabled{false};
    bool hasProcessedSample = false;
    void processActiveBlock(fad::engine::ProcessContextReplacing<float>& context);
    void processLegacyBlock(fad::engine::ProcessContextReplacing<float>& context);
    void processNativeBlock(fad::engine::ProcessContextReplacing<float>& context);
    NativePhaserCore nativeCore;
    // Appended slot controls preserve the original seven controls and serumMix.
    // Rate Hz, normalized frequency, feedback, stereo cycles, cycle beats.
    std::array<fad::engine::math::LinearSmoother<float>, 5> nativeControls;
    std::array<float, 2> nativeOffsets{}, lastOutput{}, transitionFrom{};
    std::atomic<bool> nativeProfile{false};
    bool activeProfile = false, activityKnown = false, voiceActive = false;
    int nativeRemaining = 0, profileFadeRemaining = 0, profileFadeLength = 1;
    double transportBpm = 120.0;

    struct AllPassState { float z1 = 0.0f; };

    // Max 8 stages, 2 channels
    std::array<std::array<AllPassState, 8>, 2> allPasses;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> feedbackDcBlockers;
    float lfoPhase = 0.0f;
    double wideLfoPhase = 0;
    std::array<float, 2> feedbackSample{};
    std::array<float, 2> allPassCoefficients{};
    int modulationUpdateCountdown = 0;
    int activeNumStages = 4;
    int transitionFromStages = 4;
    int transitionToStages = 4;
    int stageTransitionSamplesRemaining = 0;
    int stageTransitionLengthSamples = 1;

    // Smoothed params
    fad::engine::math::LinearSmoother<float> rate, depth, centerFreq, feedback, mix, stereoOffset;
    std::atomic<int> numStages{4};

    fad::engine::detail::PlanarBuffer dryBuffer;
    double sampleRate = 0.0;
    int numChannels = 0;
    std::atomic<bool> bypassed{false};
};
}
