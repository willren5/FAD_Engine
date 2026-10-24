#pragma once
#include <fad/engine/AudioBlock.h>
#include <fad/engine/math/DspMath.h>
#include <array>
#include <span>
#include <stdexcept>
#include <vector>

namespace fad::engine::filters
{
struct Allpass2xCoefficients
{
    std::span<const float> upDirect, upDelayed, downDirect, downDelayed;
};

// The two actual FAD native paths process one base-rate frame at a time at 2x.
// Each polyphase branch is a cascade of first-order allpasses; the down path
// has one delayed branch and averages the two phases. Coefficients are a
// prepared numerical asset, copied only in prepare. No JUCE/backend is owned.
class Oversample2x
{
public:
    Oversample2x() = default;
    Oversample2x(const Oversample2x&) = delete;
    Oversample2x& operator=(const Oversample2x&) = delete;
    Oversample2x(Oversample2x&&) = delete;
    Oversample2x& operator=(Oversample2x&&) = delete;
    void prepare(uint32_t channels, const Allpass2xCoefficients& coefficients)
    {
        if (channels == 0 || channels > 2) throw std::invalid_argument("2x frame processor requires mono/stereo");
        channels_ = channels;
        directUp_.prepare(coefficients.upDirect, channels);
        delayedUp_.prepare(coefficients.upDelayed, channels);
        directDown_.prepare(coefficients.downDirect, channels);
        delayedDown_.prepare(coefficients.downDelayed, channels);
        reset();
    }
    void reset() noexcept
    {
        directUp_.reset(); delayedUp_.reset(); directDown_.reset(); delayedDown_.reset();
        frames_ = {}; delayedDownOutput_ = {};
    }
    AudioBlock<float> processSamplesUp(const AudioBlock<float>& input) noexcept
    {
        assert(input.getNumSamples() == 1 && input.getNumChannels() == channels_);
        for (uint32_t channel = 0; channel < channels_; ++channel)
        {
            const float value = input.getSample(channel, 0);
            frames_[channel][0] = directUp_.process(channel, value);
            frames_[channel][1] = delayedUp_.process(channel, value);
            planes_[channel] = frames_[channel].data();
        }
        return {planes_.data(), channels_, 2};
    }
    void processSamplesDown(AudioBlock<float>& output) noexcept
    {
        assert(output.getNumSamples() == 1 && output.getNumChannels() == channels_);
        for (uint32_t channel = 0; channel < channels_; ++channel)
        {
            const auto direct = directDown_.process(channel, frames_[channel][0]);
            const auto delayed = delayedDown_.process(channel, frames_[channel][1]);
            output.setSample(channel, 0, .5f * (direct + delayedDownOutput_[channel]));
            delayedDownOutput_[channel] = delayed;
        }
    }
private:
    class Branch
    {
    public:
        void prepare(std::span<const float> coefficients, uint32_t channels)
        {
            if (coefficients.size() > 16) throw std::invalid_argument("too many prepared allpass sections");
            coefficients_.assign(coefficients.begin(), coefficients.end());
            states_.assign(coefficients.size() * channels, 0);
        }
        void reset() noexcept { std::fill(states_.begin(), states_.end(), 0.0f); }
        float process(uint32_t channel, float value) noexcept
        {
            const auto base = channel * coefficients_.size();
            for (size_t section = 0; section < coefficients_.size(); ++section)
            {
                auto& state = states_[base + section];
                const auto output = value * coefficients_[section] + state;
                state = value - output * coefficients_[section];
                math::snapSmallState(state);
                value = output;
            }
            return value;
        }
    private:
        std::vector<float> coefficients_, states_;
    };
    Branch directUp_, delayedUp_, directDown_, delayedDown_;
    std::array<std::array<float, 2>, 2> frames_{};
    std::array<float*, 2> planes_{};
    std::array<float, 2> delayedDownOutput_{};
    uint32_t channels_ = 0;
};
}
