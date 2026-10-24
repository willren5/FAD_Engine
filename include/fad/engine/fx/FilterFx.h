#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/filters/TptStateVariable.h>
#include <fad/engine/math/RuntimeRange.h>

#include <limits>

#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/filters/FilterBlock.h>

namespace fadmodel1::dsp::fx
{
class FilterFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;

    void clearNativeModulation() noexcept override { nativeModulation.fill(0); }
    void addNativeModulation(int target, float offset) noexcept override {
        if (target >= 143 && target <= 146) nativeModulation[static_cast<size_t>(target - 143)] += offset;
    }
    bool supportsNativeModulation() const noexcept override { return nativeModel.load(std::memory_order_relaxed) >= 0; }
    bool needsNativeVoiceActivity() const noexcept override { return nativeModel.load(std::memory_order_relaxed) == 79; }
    void setNativeVoiceActivity(bool known, bool active) noexcept override
    { nativeActivityKnown = known; nativeVoiceActive = active; }
    int nativeModulationQuantum() const noexcept override { return coefficientUpdateSamplesRemaining > 0 ? coefficientUpdateSamplesRemaining : 16; }
private:
    std::array<float, 4> nativeModulation{};
    std::array<float, 2> nativePreviousInput{};
    float nativeInputGain = 1.0f;
    std::array<double, 2> nativeHoldPhase{}, nativeHoldIncrement{};
    std::array<float, 2> nativeHeldSample{};
    int nativeHoldIdleSamples = 0;
    int nativeHoldIdleLimit = 0;
    bool nativeActivityKnown = false;
    bool nativeVoiceActive = false;
    void resetNativeHold() noexcept;
    void processNativeBlock(const fad::engine::AudioBlock<float>& block);
    void updateFilterParams(int filterModeValue,
                            float cutoffHz,
                            float resonanceValue,
                            float hiCutHz,
                            float loCutHz) noexcept;

    std::array<fad::engine::filters::TptStateVariable<float>, 2> mainFilter;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputHP;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> outputLP;

    fad::engine::math::LinearSmoother<float> cutoff;
    fad::engine::math::LinearSmoother<float> resonance;
    fad::engine::math::LinearSmoother<float> driveGain;
    fad::engine::math::LinearSmoother<float> hiCut;
    fad::engine::math::LinearSmoother<float> loCut;
    std::array<filters::FilterBlock,2> nativeFilters;
    std::array<filters::SerumScreamFilter,2> nativeScreamFilters;
    std::array<filters::SerumCombFilter,2> nativeCombFilters;
    fad::engine::math::LinearSmoother<float> nativeVariation, nativeStereo, nativeWet, nativeLevel;
    fad::engine::math::LinearSmoother<float> nativeResonance, nativeCutoff;
    std::atomic<int> nativeModel{-1};
    int lastNativeModel = -1;

    std::atomic<int> filterMode{0}; // 0=LP, 1=BP, 2=HP
    double sampleRate = 0.0;
    int numChannels = 0;
    float lastAppliedCutoffHz = std::numeric_limits<float>::quiet_NaN();
    float lastAppliedResonance = std::numeric_limits<float>::quiet_NaN();
    float lastAppliedHiCutHz = std::numeric_limits<float>::quiet_NaN();
    float lastAppliedLoCutHz = std::numeric_limits<float>::quiet_NaN();
    int lastAppliedFilterMode = -1;
    int coefficientUpdateSamplesRemaining = 0;
    int modeFadeSamples = 132;
    int modeFadeRemaining = 0;
    std::array<float, 2> modeFadeStartSamples{};
    std::array<float, 2> lastOutputSamples{};
    bool hasProcessedSample = false;
    bool parametersPrimed = false;
    std::atomic<bool> bypassed{false};
};
}
