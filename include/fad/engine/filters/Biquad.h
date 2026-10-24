#pragma once
#include <fad/engine/AudioView.h>
#include <fad/engine/math/DspMath.h>
#include <array>
#include <cassert>
#include <cmath>
#include <complex>
#include <numbers>

namespace fad::engine::filters
{
enum class EqShape { lowShelf, highShelf, peak };

// Bilinear low/high-pass forms used when a native EQ band changes topology.
// The reciprocal-tangent low-pass form preserves its existing quantisation.
template<class Sample> std::array<Sample, 6> eqPassCoefficients(
    bool highPass, double rate, Sample frequency, Sample q) noexcept
{
    const Sample tangent = std::tan(std::numbers::pi_v<Sample> * frequency / static_cast<Sample>(rate));
    const Sample n = highPass ? tangent : Sample{1} / tangent;
    const Sample n2 = n * n, inverseQ = Sample{1} / q;
    const Sample first = Sample{1} / (Sample{1} + inverseQ * n + n2);
    return {first, highPass ? Sample{-2} * first : Sample{2} * first, first, Sample{1},
            highPass ? Sample{2} * first * (n2 - Sample{1}) : Sample{2} * first * (Sample{1} - n2),
            first * (Sample{1} - inverseQ * n + n2)};
}

// RBJ/W3C Audio EQ Cookbook transfer functions, expressed as six raw
// coefficients [b0,b1,b2,a0,a1,a2]. These are the three forms used by FAD's
// existing EQ; the native cut-filter path keeps its separate TPT history.
template<class Sample> std::array<Sample, 6> eqCoefficients(
    EqShape shape, double rate, Sample frequency, Sample q, Sample gain) noexcept
{
    assert(rate > 0 && frequency > 0 && frequency <= rate * .5 && q > 0 && gain > 0);
    const Sample amplitude = std::sqrt(gain);
    const Sample angle = (Sample{2} * std::numbers::pi_v<Sample> * std::max(frequency, Sample{2})) / static_cast<Sample>(rate);
    const Sample cosine = std::cos(angle), sine = std::sin(angle);
    if (shape == EqShape::peak)
    {
        const Sample bandwidth = sine / (q * Sample{2});
        const Sample boost = bandwidth * amplitude, cut = bandwidth / amplitude;
        const Sample middle = Sample{-2} * cosine;
        return {Sample{1} + boost, middle, Sample{1} - boost,
                Sample{1} + cut, middle, Sample{1} - cut};
    }
    const Sample plus = amplitude + Sample{1}, minus = amplitude - Sample{1};
    const Sample slope = sine * std::sqrt(amplitude) / q;
    const Sample difference = minus * cosine;
    if (shape == EqShape::lowShelf)
        return {amplitude * (plus - difference + slope),
                amplitude * Sample{2} * (minus - plus * cosine),
                amplitude * (plus - difference - slope),
                plus + difference + slope,
                Sample{-2} * (minus + plus * cosine),
                plus + difference - slope};
    return {amplitude * (plus + difference + slope),
            amplitude * Sample{-2} * (minus + plus * cosine),
            amplitude * (plus + difference - slope),
            plus - difference + slope,
            Sample{2} * (minus - plus * cosine),
            plus - difference - slope};
}

// A fixed second-order transposed direct-form-II state. No ref-counted
// coefficient owner, order change, allocation or final destructor in render.
template<class Sample> class Biquad
{
public:
    void prepare(const ProcessSpec&) noexcept { reset(); }
    void reset(Sample value = Sample{}) noexcept { state1_ = state2_ = value; }
    void setRawCoefficients(const std::array<Sample, 6>& raw) noexcept
    {
        assert(raw[3] != 0);
        const Sample reciprocal = Sample{1} / raw[3];
        coefficients_ = {raw[0] * reciprocal, raw[1] * reciprocal, raw[2] * reciprocal,
                         raw[4] * reciprocal, raw[5] * reciprocal};
    }
    const std::array<Sample, 5>& coefficients() const noexcept { return coefficients_; }
    Sample processSample(Sample input) noexcept
    {
        const Sample output = input * coefficients_[0] + state1_;
        state1_ = input * coefficients_[1] - output * coefficients_[3] + state2_;
        state2_ = input * coefficients_[2] - output * coefficients_[4];
        return output;
    }
    void snapToZero() noexcept { math::snapSmallState(state1_); math::snapSmallState(state2_); }
    double getMagnitudeForFrequency(double frequency, double rate) const noexcept
    {
        const auto z = std::polar(1.0, -2.0 * std::numbers::pi_v<double> * frequency / rate);
        const auto z2 = z * z;
        const auto numerator = static_cast<double>(coefficients_[0]) + static_cast<double>(coefficients_[1]) * z
                             + static_cast<double>(coefficients_[2]) * z2;
        const auto denominator = 1.0 + static_cast<double>(coefficients_[3]) * z + static_cast<double>(coefficients_[4]) * z2;
        return std::abs(numerator / denominator);
    }
private:
    std::array<Sample, 5> coefficients_{Sample{1}, Sample{}, Sample{}, Sample{}, Sample{}};
    Sample state1_ = 0, state2_ = 0;
};
}
