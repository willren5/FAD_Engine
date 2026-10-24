#pragma once
#include <fad/engine/AudioView.h>
#include <cassert>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace fad::engine
{
// A channel/sample window into caller-owned planes. Child windows retain the
// original pointer table and explicit channel offset; no temporary pointer
// array, allocation, sample copy, or ownership transfer is needed.
template<class Sample> class AudioBlock
{
public:
    explicit AudioBlock(AudioView<Sample> view) noexcept
        : view_(view), channelCount_(view.channels()) {}
    AudioBlock(Sample* const* planes, size_t channels, size_t frames) noexcept
        : AudioBlock(AudioView<Sample>{planes, checked(channels), checked(frames)}) {}
    template<class Buffer> requires requires(Buffer& buffer) {
        buffer.getArrayOfWritePointers(); buffer.getNumChannels(); buffer.getNumSamples();
    }
    explicit AudioBlock(Buffer& buffer) noexcept
        : AudioBlock(buffer.getArrayOfWritePointers(), static_cast<size_t>(buffer.getNumChannels()),
                     static_cast<size_t>(buffer.getNumSamples())) {}

    size_t getNumChannels() const noexcept { return static_cast<size_t>(channelCount_); }
    size_t getNumSamples() const noexcept { return static_cast<size_t>(view_.frames()); }
    Sample* getChannelPointer(size_t channel) const noexcept
    { assert(channel < getNumChannels()); return view_.channel(firstChannel_ + static_cast<int>(channel)); }
    Sample getSample(size_t channel, size_t sample) const noexcept
    { assert(sample < getNumSamples()); return getChannelPointer(channel)[sample]; }
    void setSample(size_t channel, size_t sample, std::remove_const_t<Sample> value) const noexcept
        requires (!std::is_const_v<Sample>)
    { assert(sample < getNumSamples()); getChannelPointer(channel)[sample] = value; }
    AudioBlock getSubBlock(size_t start, size_t frames) const noexcept
    { return {view_.subview(checked(start), checked(frames)), firstChannel_, channelCount_}; }
    AudioBlock getSubBlock(size_t start) const noexcept
    { assert(start <= getNumSamples()); return getSubBlock(start, getNumSamples() - start); }
    AudioBlock getSingleChannelBlock(size_t channel) const noexcept
    { return getSubsetChannelBlock(channel, 1); }
    AudioBlock getSubsetChannelBlock(size_t first, size_t channels) const noexcept
    {
        assert(first <= getNumChannels() && channels <= getNumChannels() - first);
        return {view_, firstChannel_ + checked(first), checked(channels)};
    }
    void clear() const noexcept requires (!std::is_const_v<Sample>)
    { for (size_t channel = 0; channel < getNumChannels(); ++channel) std::fill_n(getChannelPointer(channel), getNumSamples(), Sample{}); }
    template<class Other> void copyFrom(const AudioBlock<Other>& source) const noexcept
        requires (!std::is_const_v<Sample>)
    {
        assert(source.getNumChannels() == getNumChannels() && source.getNumSamples() == getNumSamples());
        for (size_t channel = 0; channel < getNumChannels(); ++channel)
            if (getChannelPointer(channel) != source.getChannelPointer(channel))
                std::copy_n(source.getChannelPointer(channel), getNumSamples(), getChannelPointer(channel));
    }
    template<class Other> void add(const AudioBlock<Other>& source) const noexcept
        requires (!std::is_const_v<Sample>)
    {
        assert(source.getNumChannels() == getNumChannels() && source.getNumSamples() == getNumSamples());
        for (size_t channel = 0; channel < getNumChannels(); ++channel)
            for (size_t sample = 0; sample < getNumSamples(); ++sample)
                getChannelPointer(channel)[sample] += source.getChannelPointer(channel)[sample];
    }
    void multiplyBy(std::remove_const_t<Sample> gain) const noexcept requires (!std::is_const_v<Sample>)
    {
        for (size_t channel = 0; channel < getNumChannels(); ++channel)
            for (size_t sample = 0; sample < getNumSamples(); ++sample) getChannelPointer(channel)[sample] *= gain;
    }
private:
    AudioBlock(AudioView<Sample> view, int first, int count) noexcept
        : view_(view), firstChannel_(first), channelCount_(count) {}
    static int checked(size_t value) noexcept
    { assert(value <= static_cast<size_t>(std::numeric_limits<int>::max())); return static_cast<int>(value); }
    AudioView<Sample> view_;
    int firstChannel_ = 0, channelCount_ = 0;
};

template<class Sample> class ProcessContextReplacing
{
public:
    explicit ProcessContextReplacing(const AudioBlock<Sample>& block) noexcept : block_(block) {}
    AudioBlock<Sample>& getOutputBlock() noexcept { return block_; }
    const AudioBlock<Sample>& getOutputBlock() const noexcept { return block_; }
    const AudioBlock<Sample>& getInputBlock() const noexcept { return block_; }
    static constexpr bool usesSeparateInputAndOutputBlocks() noexcept { return false; }
    bool isBypassed = false;
private:
    AudioBlock<Sample> block_;
};
}
