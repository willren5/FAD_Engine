#pragma once
#include <fad/engine/AudioView.h>
#include <algorithm>
#include <cassert>
#include <vector>

namespace fad::engine::detail
{
// Preparation/control-thread storage. Render code borrows view(); it never
// changes dimensions. Copying repairs the pointer table instead of retaining
// pointers into the source buffer. No allocator is used by sample operations.
class PlanarBuffer
{
public:
    PlanarBuffer() = default;
    PlanarBuffer(int channels, int frames) { setSize(channels, frames); }
    PlanarBuffer(const PlanarBuffer& other) : samples_(other.samples_), frames_(other.frames_)
    { refreshPointers(); }
    PlanarBuffer& operator=(const PlanarBuffer& other)
    { if (this != &other) { PlanarBuffer copy(other); *this = std::move(copy); } return *this; }
    PlanarBuffer(PlanarBuffer&&) noexcept = default;
    PlanarBuffer& operator=(PlanarBuffer&&) noexcept = default;

    void setSize(int channels, int frames, bool retain = false,
                 bool /*clearNewSpace*/ = false, bool /*avoidReallocation*/ = false)
    {
        assert(channels >= 0 && frames >= 0);
        samples_.resize(static_cast<size_t>(channels));
        for (auto& plane : samples_)
        {
            plane.resize(static_cast<size_t>(frames));
            if (!retain) std::fill(plane.begin(), plane.end(), 0.0f);
        }
        frames_ = frames;
        refreshPointers();
    }
    int getNumChannels() const noexcept { return static_cast<int>(samples_.size()); }
    int getNumSamples() const noexcept { return frames_; }
    float* getWritePointer(int channel, int offset = 0) noexcept
    { assert(channel >= 0 && channel < getNumChannels() && offset >= 0 && offset <= frames_); return pointers_[static_cast<size_t>(channel)] + offset; }
    const float* getReadPointer(int channel, int offset = 0) const noexcept
    { assert(channel >= 0 && channel < getNumChannels() && offset >= 0 && offset <= frames_); return pointers_[static_cast<size_t>(channel)] + offset; }
    float* const* getArrayOfWritePointers() noexcept { return pointers_.data(); }
    const float* const* getArrayOfReadPointers() const noexcept { return pointers_.data(); }
    AudioBufferView view() noexcept { return {pointers_.data(), getNumChannels(), frames_}; }
    ConstAudioBufferView view() const noexcept { return {pointers_.data(), getNumChannels(), frames_}; }
    void clear() noexcept { for (auto& plane : samples_) std::fill(plane.begin(), plane.end(), 0.0f); }
    void clear(int offset, int count) noexcept
    { for (int channel = 0; channel < getNumChannels(); ++channel) clear(channel, offset, count); }
    void clear(int channel, int offset, int count) noexcept
    { assert(count >= 0 && count <= frames_ - offset); std::fill_n(getWritePointer(channel, offset), count, 0.0f); }
    float getSample(int channel, int sample) const noexcept { return getReadPointer(channel)[sample]; }
    void setSample(int channel, int sample, float value) noexcept { getWritePointer(channel)[sample] = value; }
    void addSample(int channel, int sample, float value) noexcept { getWritePointer(channel)[sample] += value; }
    void copyFrom(int channel, int offset, const float* source, int count) noexcept
    { assert(count >= 0 && count <= frames_ - offset); std::copy_n(source, count, getWritePointer(channel, offset)); }
    template<class Buffer> void copyFrom(int channel, int offset, const Buffer& source,
                                         int sourceChannel, int sourceOffset, int count) noexcept
    { copyFrom(channel, offset, source.getReadPointer(sourceChannel, sourceOffset), count); }
    template<class Buffer> void makeCopyOf(const Buffer& source, bool /*avoidReallocation*/ = false)
    {
        setSize(source.getNumChannels(), source.getNumSamples());
        for (int channel = 0; channel < getNumChannels(); ++channel)
            copyFrom(channel, 0, source.getReadPointer(channel), frames_);
    }
private:
    void refreshPointers()
    {
        pointers_.resize(samples_.size());
        for (size_t i = 0; i < samples_.size(); ++i) pointers_[i] = samples_[i].data();
    }
    std::vector<std::vector<float>> samples_;
    std::vector<float*> pointers_;
    int frames_ = 0;
};
}
