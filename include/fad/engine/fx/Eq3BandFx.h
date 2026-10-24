#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/filters/Biquad.h>
#include <fad/engine/math/RuntimeRange.h>
#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/filters/CurveNodeFilter.h>

namespace fadmodel1::dsp::fx
{
class Eq3BandFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;
    // Static response of this exclusively owned, settled processor. The UI
    // constructs a private processor; never inspect live audio coefficients.
    double getStaticMagnitudeForFrequency(double frequencyHz) const noexcept;
    // The chain resolves immutable history and local LFO bindings first.
    void setNodeCurve(const parameters::CurveNodes&) noexcept;
    int nodeCurveQuantum() const noexcept { return std::max(1, nodeUpdateSamplesRemaining); }
    bool nodeCurveUpdateDue() const noexcept { return nodeUpdateSamplesRemaining<=0; }
    void clearNativeModulation() noexcept override;
    void addNativeModulation(int target, float offset) noexcept override;
    int nativeModulationQuantum() const noexcept override
    {
        return nativeMode ? 1 : (filterUpdateSamplesRemaining > 0 ? filterUpdateSamplesRemaining : 16);
    }
    bool supportsNativeModulation() const noexcept override { return nativeMode; }

private:
    void updateFilters() noexcept;
    void advanceSmoothers(int numSamples) noexcept;

    // 3 bands x 2 channels = 6 IIR filters
    std::array<fad::engine::filters::Biquad<float>, 2> lowBand;
    std::array<fad::engine::filters::Biquad<float>, 2> midBand;
    std::array<fad::engine::filters::Biquad<float>, 2> highBand;
    std::array<filters::CurveNodeFilter,2> nodeFilters;
    int nodeUpdateSamplesRemaining=0;
    bool nodeLayerEnabled=false;

    // Smoothed parameters
    fad::engine::math::LinearSmoother<float> lowFreq, lowGainDb;
    fad::engine::math::LinearSmoother<float> midFreq, midGainDb, midQ;
    fad::engine::math::LinearSmoother<float> highFreq, highGainDb;

    // Imported two-band EQ: frequency L/H, Q L/H, gain L/H. Legacy controls
    // retain their original ranges and do not own the imported signal path.
    std::array<fad::engine::math::LinearSmoother<float>, 6> nativeControls;
    std::array<int, 2> nativeTypes{}; // shelf, peak, HP (low) / LP (high)
    bool nativeMode = false;
    std::array<float,6> nativeModulation{};

    std::array<std::array<double, 5>, 2> nativeCoefficients{};
    std::array<std::array<double, 5>, 2> nativeCoefficientTargets{};
    // Native cut filters preserve trapezoidal integrator states, not biquad
    // input/output history. Equal static transfer functions are not equivalent
    // when frequency or Q changes (independent native square-LFO probes).
    std::array<std::array<double, 4>, 2> nativeCutCoefficients{};
    std::array<std::array<std::array<double, 2>, 2>, 2> nativeCutStates{};
    double nativeCoefficientStep = 0.0;
    bool nativeCoefficientsPrimed = false;
    std::array<std::array<std::array<double, 4>, 2>, 2> nativeHistory{};

    double sampleRate = 0.0;
    int numChannels = 0;
    bool parametersPrimed = false;
    int filterUpdateSamplesRemaining = 0;
    std::atomic<bool> bypassed{false};
    std::atomic<bool> needsFilterUpdate{true};
};
}
