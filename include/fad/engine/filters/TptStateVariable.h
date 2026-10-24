#pragma once
#include <fad/engine/AudioView.h>
#include <fad/engine/AudioBlock.h>
#include <fad/engine/math/DspMath.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>
#include <vector>

namespace fad::engine::filters
{
enum class TptMode { lowpass, bandpass, highpass };

// Independent trapezoidal state-variable realization. Solve
// h = x - k*b - l, b = g*h + sB, l = g*b + sL, then advance
// each trapezoidal integrator by twice its current increment.
// The band output is unnormalised (its resonance gain is intentional).
// Allocation is confined to prepare; per-sample state is two scalars/channel.
template<class Sample> class TptStateVariable
{
public:
    using Type = TptMode;
    TptStateVariable() { updateCoefficients(); }
    void prepare(const ProcessSpec& spec)
    {
        assert(spec.sampleRate > 0 && spec.numChannels > 0);
        sampleRate_ = spec.sampleRate;
        state_.assign(spec.numChannels, {});
        updateCoefficients();
    }
    void setType(Type type) noexcept { type_ = type; }
    Type getType() const noexcept { return type_; }
    void setCutoffFrequency(Sample frequency) noexcept
    {
        assert(frequency > 0 && frequency <= sampleRate_ * .5);
        cutoff_ = frequency;
        updateCoefficients();
    }
    Sample getCutoffFrequency() const noexcept { return cutoff_; }
    void setResonance(Sample resonance) noexcept
    { assert(resonance > 0); resonance_ = resonance; updateCoefficients(); }
    Sample getResonance() const noexcept { return resonance_; }
    void reset(Sample initial = Sample{}) noexcept
    { std::fill(state_.begin(), state_.end(), State{initial, initial}); }
    void snapToZero() noexcept
    {
        for (auto& state : state_)
        { math::snapSmallState(state.band); math::snapSmallState(state.low); }
    }
    Sample processSample(int channel, Sample input) noexcept
    {
        assert(channel >= 0 && static_cast<size_t>(channel) < state_.size());
        auto& state = state_[static_cast<size_t>(channel)];
        const Sample high = inverse_ * (input - (frequency_ + damping_) * state.band - state.low);
        const Sample band = high * frequency_ + state.band;
        const Sample low = band * frequency_ + state.low;
        state.band = high * frequency_ + band;
        state.low = band * frequency_ + low;
        switch (type_)
        {
            case Type::lowpass: return low;
            case Type::bandpass: return band;
            case Type::highpass: return high;
        }
        return low;
    }
    void process(const ProcessContextReplacing<Sample>& context) noexcept
    {
        if (context.isBypassed) return;
        const auto& output = context.getOutputBlock();
        assert(output.getNumChannels() == state_.size());
        for (size_t channel = 0; channel < output.getNumChannels(); ++channel)
        {
            auto* samples = output.getChannelPointer(channel);
            for (size_t sample = 0; sample < output.getNumSamples(); ++sample)
                samples[sample] = processSample(static_cast<int>(channel), samples[sample]);
        }
        snapToZero();
    }
private:
    void updateCoefficients() noexcept
    {
        // Preserve the established coefficient quantisation boundaries: angle
        // and reciprocals use double, while the two products use Sample.
        const double angle = std::numbers::pi_v<double> * static_cast<double>(cutoff_) / sampleRate_;
        frequency_ = static_cast<Sample>(std::tan(angle));
        damping_ = static_cast<Sample>(1.0 / static_cast<double>(resonance_));
        const double denominator = 1.0 + static_cast<double>(damping_ * frequency_)
                                      + static_cast<double>(frequency_ * frequency_);
        inverse_ = static_cast<Sample>(1.0 / denominator);
    }
    struct State { Sample band = 0, low = 0; };
    std::vector<State> state_;
    double sampleRate_ = 44100;
    Sample cutoff_ = 1000;
    Sample resonance_ = static_cast<Sample>(0.70710678118654752440084436210485);
    Sample frequency_ = 0, damping_ = 0, inverse_ = 1;
    Type type_ = Type::lowpass;
};
}
