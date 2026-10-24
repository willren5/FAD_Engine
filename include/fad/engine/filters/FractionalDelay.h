#pragma once
#include <fad/engine/AudioView.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace fad::engine::filters
{
enum class DelayInterpolation { linear, lagrange3 };

// Prepared planar ring with independent push/read clocks per channel. Moving
// those clocks forward is equivalent to reading successively older samples
// behind the current read head. Only prepare/setMaximum allocate or clear.
template<class Sample, DelayInterpolation Interpolation = DelayInterpolation::linear>
class FractionalDelay
{
public:
    explicit FractionalDelay(int maximum = 0) { setMaximumDelayInSamples(maximum); }
    void setMaximumDelayInSamples(int maximum)
    {
        assert(maximum >= 0 && maximum <= std::numeric_limits<int>::max() - 2);
        capacity_ = std::max(4, maximum + 2);
        for (auto& plane : samples_) plane.assign(static_cast<size_t>(capacity_), Sample{});
        reset();
    }
    int getMaximumDelayInSamples() const noexcept { return capacity_ - 2; }
    void prepare(const ProcessSpec& spec)
    {
        samples_.assign(spec.numChannels, std::vector<Sample>(static_cast<size_t>(capacity_)));
        write_.assign(spec.numChannels, 0);
        read_.assign(spec.numChannels, 0);
    }
    void reset() noexcept
    {
        for (auto& plane : samples_) std::fill(plane.begin(), plane.end(), Sample{});
        std::fill(write_.begin(), write_.end(), 0);
        std::fill(read_.begin(), read_.end(), 0);
    }
    void setDelay(Sample samples) noexcept
    {
        assert(std::isfinite(samples) && samples >= 0 && samples <= getMaximumDelayInSamples());
        delay_ = std::clamp(samples, Sample{}, static_cast<Sample>(getMaximumDelayInSamples()));
        whole_ = static_cast<int>(std::floor(delay_));
        fraction_ = delay_ - static_cast<Sample>(whole_);
        if constexpr (Interpolation == DelayInterpolation::lagrange3)
            if (whole_ > 0) { --whole_; fraction_ += Sample{1}; }
    }
    Sample getDelay() const noexcept { return delay_; }
    void pushSample(int channel, Sample sample) noexcept
    {
        const auto c = checkedChannel(channel);
        samples_[c][static_cast<size_t>(write_[c])] = sample;
        if (++write_[c] == capacity_) write_[c] = 0;
    }
    Sample popSample(int channel, Sample delay = Sample{-1}, bool advance = true) noexcept
    {
        if (delay >= 0) setDelay(delay);
        const auto c = checkedChannel(channel);
        const auto sample = [&](int distance) noexcept {
            int index = read_[c] - distance;
            if (index < 0) index += capacity_;
            return samples_[c][static_cast<size_t>(index)];
        };
        Sample value{};
        if constexpr (Interpolation == DelayInterpolation::linear)
        {
            const auto current = sample(whole_);
            value = current + fraction_ * (sample(whole_ + 1) - current);
        }
        else
        {
            // Degree-three Lagrange basis at t=fraction, with nodes 0..3.
            // Factor t from the last three terms to retain their evaluation
            // boundary while keeping the interpolation polynomial explicit.
            const Sample a = fraction_ - Sample{1}, b = fraction_ - Sample{2}, c3 = fraction_ - Sample{3};
            const std::array<Sample, 4> weights{-a * b * c3 / Sample{6},
                b * c3 / Sample{2}, -a * c3 / Sample{2}, a * b / Sample{6}};
            const Sample rest = sample(whole_ + 1) * weights[1]
                              + sample(whole_ + 2) * weights[2]
                              + sample(whole_ + 3) * weights[3];
            value = sample(whole_) * weights[0] + fraction_ * rest;
        }
        if (advance && ++read_[c] == capacity_) read_[c] = 0;
        return value;
    }
private:
    size_t checkedChannel(int channel) const noexcept
    { assert(channel >= 0 && static_cast<size_t>(channel) < samples_.size()); return static_cast<size_t>(channel); }
    std::vector<std::vector<Sample>> samples_;
    std::vector<int> write_, read_;
    int capacity_ = 4, whole_ = 0;
    Sample delay_ = 0, fraction_ = 0;
};
}
