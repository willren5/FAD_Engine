#pragma once

#include <algorithm>
#include <cstdint>
#include <fad/engine/AudioView.h>

#include <array>

#include "SignalFlowGraph.h"

namespace fadmodel1::dsp::diagnostics
{
class ClipDetector
{
public:
    void reset() noexcept;
    void checkSample(ModuleId module, float sample, int channel) noexcept;
    template<class Buffer> void checkBuffer(ModuleId module, const Buffer& buffer) noexcept
{
    const auto idx = static_cast<int>(module);
    if (idx < 0 || idx >= moduleCount)
        return;

    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* data = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto mag = std::abs(data[i]);
            if (mag > peak)
                peak = mag;
        }
    }

    if (peak > peakLevels[static_cast<size_t>(idx)])
        peakLevels[static_cast<size_t>(idx)] = peak;

    if (peak > threshold)
    {
        clipFlags[static_cast<size_t>(idx)] = true;
        clipHoldCounters[static_cast<size_t>(idx)] = holdBlocks;
    }
}

    bool isClipping(ModuleId module) const noexcept;
    float getPeakLevel(ModuleId module) const noexcept;

    void setThreshold(float thresholdLinear) noexcept;
    void decayClipFlags() noexcept;

private:
    static constexpr int moduleCount = static_cast<int>(ModuleId::count);
    std::array<float, moduleCount> peakLevels{};
    std::array<bool, moduleCount> clipFlags{};
    std::array<int, moduleCount> clipHoldCounters{};
    float threshold = 0.99f;
    int holdBlocks = 20;
};
} // namespace fadmodel1::dsp::diagnostics
