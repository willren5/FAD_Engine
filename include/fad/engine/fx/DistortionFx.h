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
#include <fad/engine/filters/SerumDistortionFilter.h>

namespace fadmodel1::dsp::fx
{
class DistortionFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;

    void clearNativeModulation() noexcept override { nativeModulation.fill(0); }
    void addNativeModulation(int target, float offset) noexcept override {
        if ((target == 96 || target == 97 || target == 100) && std::isfinite(offset)) {
            const auto index = target == 100 ? 2 : target - 96;
            auto& value = nativeModulation[static_cast<size_t>(index)];
            value = std::clamp(value+offset, -32.0f, 32.0f);
        }
    }
    bool supportsNativeModulation() const noexcept override {
        return serumSoftClip.load(std::memory_order_relaxed) || serumZeroSquareEnabled.load(std::memory_order_relaxed) || serumTube.load(std::memory_order_relaxed) || serumAsym.load(std::memory_order_relaxed) || serumDownsample.load(std::memory_order_relaxed) || serumDiode2.load(std::memory_order_relaxed);
    }
    bool needsNativeVoiceActivity() const noexcept override {
        return nativeFilterPlacement.load(std::memory_order_relaxed) > 0
            || serumDownsample.load(std::memory_order_relaxed)
            || (serumZeroSquareEnabled.load(std::memory_order_relaxed) && nativePostHP.load(std::memory_order_relaxed));
    }
    void setNativeVoiceActivity(bool known, bool active) noexcept override { nativeActivityKnown=known; nativeVoiceActive=active; }
    int nativeModulationQuantum() const noexcept override {
        return nativeModulationRemaining > 0 ? nativeModulationRemaining
            : (serumDownsample.load(std::memory_order_relaxed) ? 4 : 16);
    }
private:
    std::array<float,3> nativeModulation{}, appliedNativeModulation{};
    int nativeModulationRemaining = 0;
    void processActiveBlock(fad::engine::ProcessContextReplacing<float>& context);
    static float processNonlinearity(float input, int mode) noexcept;
    void updatePreFilters(float toneValue) noexcept;
    void updatePostFilters(float toneValue) noexcept;

    fad::engine::detail::PlanarBuffer dryBuffer;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> preFilters;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> postFilters;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> dcBlockers;
    fad::engine::math::LinearSmoother<float> amount;
    fad::engine::math::LinearSmoother<float> nativeDrive;
    std::atomic<bool> serumSoftClip{false};
    bool activeSerumSoftClip = false;
    std::atomic<bool> serumTube{false};
    bool activeSerumTube = false;
    std::atomic<bool> serumAsym{false};
    bool activeSerumAsym = false;
    std::atomic<bool> serumDownsample{false};
    bool activeSerumDownsample = false;
    std::atomic<bool> serumDiode2{false};
    bool activeSerumDiode2 = false;
    std::atomic<int> nativeFilterPlacement{0}; // 0 Off, 1 Pre, 2 Post
    int activeNativeFilterPlacement = 0;
    std::array<filters::SerumDistortionFilter, 2> nativePlacementFilters;
    fad::engine::math::LinearSmoother<float> nativeFilterBandwidth;
    fad::engine::math::LinearSmoother<float> nativeFilterMorphTarget;
    float nativeFilterMorph = 0.0f;
    int nativePlacementIdleSamples = 0;
    bool nativeActivityKnown = false, nativeVoiceActive = false;
    int nativeHoldCounter = 0, nativeHoldIdleSamples = 0;
    std::array<float, 2> nativeHeldSamples{};
    std::atomic<bool> serumZeroSquareEnabled{false};
    bool activeSerumZeroSquare = false;
    std::unique_ptr<fad::engine::filters::Oversample2x> nativeOversampling;
    fad::engine::math::LinearSmoother<float> nativePostCutoff;
    std::atomic<bool> nativePostHP{false};
    bool activePostHP = false;
    std::array<fad::engine::filters::TptStateVariable<float>,2> nativeHP, nativeLP;
    float nativePostMorph = 0.0f, nativePostMorphCoefficient = 0.0f;
    int nativeFilterIdleSamples = 0, nativeFilterIdleLimit = 0;
    std::array<float,2> nativePrevious{};
    std::array<std::array<float,4>,2> nativeAlignment{};
    size_t nativeAlignmentIndex = 0;
    std::array<std::array<float,7>,2> nativeDryAlignment{};
    size_t nativeDryAlignmentIndex = 0;
    fad::engine::math::LinearSmoother<float> tone;
    fad::engine::math::LinearSmoother<float> mix;
    fad::engine::math::LinearSmoother<float> bias;
    std::atomic<int> distortionMode{0};
    int activeDistortionMode = 0;
    int modeFadeSamples = 132;
    int modeFadeRemaining = 0;
    std::array<float, 2> modeFadeStartSamples{};
    std::array<float, 2> lastOutputSamples{};
    bool hasProcessedSample = false;
    float preFilterFreq = 8000.0f;
    float postFilterFreq = 20000.0f;
    int toneFilterUpdateSamplesRemaining = 0;
    double sampleRate = 0.0;
    int numChannels = 0;
    std::atomic<bool> bypassed{false};
};
}
