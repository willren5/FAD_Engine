#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/filters/LinkwitzRileySplit.h>
#include <fad/engine/math/RuntimeRange.h>

#include <fad/engine/fx/BaseFxProcessor.h>

namespace fadmodel1::dsp::fx
{
class CompressorFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;
    float getGainReductionDbForUi() const noexcept override
    {
        return bypassed.load(std::memory_order_relaxed) ? 0.0f
            : publishedGainReductionDb.load(std::memory_order_relaxed);
    }

    void clearNativeModulation() noexcept override { nativeModulation.fill(0); }
    void addNativeModulation(int target, float offset) noexcept override {
        if (target == 135) nativeModulation[0] += offset;
        if (target == 138) nativeModulation[3] += offset;
        if (target == 139) nativeModulation[1] += offset;
        if (target == 269) nativeModulation[2] += offset;
    }
    bool supportsNativeModulation() const noexcept override { return nativeMode > 0; }
    int nativeModulationQuantum() const noexcept override { return nativeModulationRemaining > 0 ? nativeModulationRemaining : 16; }
private:
    std::array<float, 4> nativeModulation{}, appliedNativeModulation{};
    int nativeModulationRemaining = 0;
    float modulatedNativeThreshold() const noexcept;
    float modulatedNativeMakeup() const noexcept;
    float modulatedNativeReleaseMs() const noexcept;
    void processActiveBlock(fad::engine::ProcessContextReplacing<float>& context);
    void processNativeNormalBlock(fad::engine::ProcessContextReplacing<float>& context) noexcept;
    void processNativeMultibandBlock(fad::engine::ProcessContextReplacing<float>& context) noexcept;

    fad::engine::math::LinearSmoother<float> threshold;
    fad::engine::math::LinearSmoother<float> ratio;
    fad::engine::math::LinearSmoother<float> attackMs;
    fad::engine::math::LinearSmoother<float> releaseMs;
    fad::engine::math::LinearSmoother<float> makeupGainDb;
    fad::engine::math::LinearSmoother<float> mix;
    fad::engine::math::LinearSmoother<float> kneeDb;

    // One lock-free publication per processed chunk. Native multiband
    // reports maximum band attenuation before band/makeup gain and wet mix.
    static_assert(std::atomic<float>::is_always_lock_free);
    std::atomic<float> publishedGainReductionDb{0.0f};
    float envelopeL = 1.0f;
    float envelopeR = 1.0f;

    // 0 preserves FAD, 1 is native normal, 2 is native multiband.
    int nativeMode = 0;
    // threshold dB, ratio amount, attack ms, release ms, makeup dB, band thresholds.
    std::array<fad::engine::math::LinearSmoother<float>, 8> nativeControls;
    double nativeReductionDb = 0.0;
    std::array<double, 2> nativeDcInput{}, nativeDcOutput{};
    double nativeDcPole = 0.0;
    bool nativeParametersPrimed = false;
    std::array<fad::engine::filters::LinkwitzRileySplit, 2> nativeCrossovers;
    std::array<double, 3> nativeRmsPower{}, nativePeakEnvelope{};
    double nativeRmsPole = 0.0;

    fad::engine::detail::PlanarBuffer dryBuffer;
    double sampleRate = 0.0;
    int numChannels = 0;
    std::atomic<bool> bypassed{false};
};
}
