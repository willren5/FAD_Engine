#pragma once
#include <fad/engine/AudioView.h>
#include <fad/engine/math/DspMath.h>
#include <array>
#include <cassert>
#include <cmath>
#include <numbers>
#include <vector>

namespace fad::engine::filters
{
// LR4 split from two Butterworth low-pass sections. Their complementary sum
// is the first section's second-order allpass, so high = allpass - low4.
// The current native multiband compressor uses this double-precision dual
// output path only. The four integrator states remain physical sample history.
class LinkwitzRileySplit
{
public:
    LinkwitzRileySplit() { update(); }
    void prepare(const ProcessSpec& spec)
    { assert(spec.sampleRate > 0); rate_ = spec.sampleRate; states_.assign(spec.numChannels, {}); update(); }
    void setCutoffFrequency(double cutoff) noexcept
    { assert(cutoff > 0 && cutoff < rate_ * .5); cutoff_ = cutoff; update(); }
    void reset() noexcept { std::fill(states_.begin(), states_.end(), std::array<double, 4>{}); }
    void snapToZero() noexcept
    { for (auto& state : states_) for (auto& value : state) math::snapSmallState(value); }
    void processSample(int channel, double input, double& low, double& high) noexcept
    {
        assert(channel >= 0 && static_cast<size_t>(channel) < states_.size());
        auto& state = states_[static_cast<size_t>(channel)];
        const auto first = section(input, state[0], state[1]);
        const auto allpass = first.low - std::numbers::sqrt2_v<double> * first.band + first.high;
        low = section(first.low, state[2], state[3]).low;
        high = allpass - low;
    }
private:
    struct Frame { double low, band, high; };
    Frame section(double input, double& bandState, double& lowState) const noexcept
    {
        const double high = inverse_ * (input - (frequency_ + std::numbers::sqrt2_v<double>) * bandState - lowState);
        const double band = high * frequency_ + bandState;
        const double low = band * frequency_ + lowState;
        bandState = high * frequency_ + band;
        lowState = band * frequency_ + low;
        return {low, band, high};
    }
    void update() noexcept
    {
        frequency_ = std::tan(std::numbers::pi_v<double> * cutoff_ / rate_);
        inverse_ = 1.0 / (1.0 + std::numbers::sqrt2_v<double> * frequency_ + frequency_ * frequency_);
    }
    std::vector<std::array<double, 4>> states_;
    double rate_ = 44100, cutoff_ = 2000, frequency_ = 0, inverse_ = 1;
};
}
