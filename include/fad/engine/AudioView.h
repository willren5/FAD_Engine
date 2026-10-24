#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>

namespace fad::engine
{
struct ProcessSpec
{
    double sampleRate = 48000.0;
    uint32_t maximumBlockSize = 512;
    uint32_t numChannels = 2;
};

// Non-owning planar samples. The pointer table and samples must outlive this view.
// Host/adapters validate their buffer layout before constructing it. No allocation,
// reference counting, ownership transfer, or buffer copy occurs at this boundary.
template<class Sample> class AudioView
{
public:
    AudioView(Sample* const* data, int channels, int frames, int offset = 0) noexcept
        : data_(data), channels_(channels), frames_(frames), offset_(offset)
    { assert(channels >= 0 && frames >= 0 && offset >= 0 && (data != nullptr || channels == 0)); }

    int channels() const noexcept { return channels_; }
    int frames() const noexcept { return frames_; }
    Sample* channel(int index) const noexcept
    { assert(index >= 0 && index < channels_); return data_[index] + offset_; }

    AudioView subview(int start, int length) const noexcept
    {
        assert(start >= 0 && length >= 0 && start <= frames_ && length <= frames_ - start);
        return {data_, channels_, length, offset_ + start};
    }
    void clear() const noexcept
    { for (int c = 0; c < channels_; ++c) std::fill_n(channel(c), frames_, Sample{}); }
    void clearChannel(int index) const noexcept { std::fill_n(channel(index), frames_, Sample{}); }
    // Buffer-shaped accessors let existing FAD algorithms consume this view
    // without an owning buffer or a second copy of the algorithm.
    int getNumChannels() const noexcept { return channels_; }
    int getNumSamples() const noexcept { return frames_; }
    Sample* getWritePointer(int index, int start = 0) const noexcept
    { assert(start >= 0 && start <= frames_); return channel(index) + start; }
    const Sample* getReadPointer(int index, int start = 0) const noexcept
    { assert(start >= 0 && start <= frames_); return channel(index) + start; }
    Sample getSample(int index, int sample) const noexcept
    { assert(sample >= 0 && sample < frames_); return channel(index)[sample]; }
    void setSample(int index, int sample, Sample value) const noexcept
    { assert(sample >= 0 && sample < frames_); channel(index)[sample] = value; }
    void addSample(int index, int sample, Sample value) const noexcept
    { assert(sample >= 0 && sample < frames_); channel(index)[sample] += value; }
    void clear(int index, int start, int count) const noexcept
    { assert(start >= 0 && count >= 0 && count <= frames_ - start); std::fill_n(channel(index) + start, count, Sample{}); }
    template<class Buffer> void addFrom(int index, int start, const Buffer& source,
                                         int sourceChannel, int sourceStart, int count) const noexcept
    {
        assert(start >= 0 && count >= 0 && count <= frames_ - start);
        const auto* input = source.getReadPointer(sourceChannel, sourceStart);
        auto* output = channel(index) + start;
        for (int sample = 0; sample < count; ++sample) output[sample] += input[sample];
    }

private:
    Sample* const* data_;
    int channels_, frames_, offset_;
};
using AudioBufferView = AudioView<float>;
using ConstAudioBufferView = AudioView<const float>;
}
