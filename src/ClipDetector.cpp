#include <fad/engine/diagnostics/ClipDetector.h>

#include <cmath>

namespace fadmodel1::dsp::diagnostics
{
void ClipDetector::reset() noexcept
{
    peakLevels.fill(0.0f);
    clipFlags.fill(false);
    clipHoldCounters.fill(0);
}

void ClipDetector::checkSample(ModuleId module, float sample, int channel) noexcept
{
    (void) channel;
    const auto idx = static_cast<int>(module);
    if (idx < 0 || idx >= moduleCount)
        return;

    const auto magnitude = std::abs(sample);
    if (magnitude > peakLevels[static_cast<size_t>(idx)])
        peakLevels[static_cast<size_t>(idx)] = magnitude;

    if (magnitude > threshold)
    {
        clipFlags[static_cast<size_t>(idx)] = true;
        clipHoldCounters[static_cast<size_t>(idx)] = holdBlocks;
    }
}


bool ClipDetector::isClipping(ModuleId module) const noexcept
{
    const auto idx = static_cast<int>(module);
    if (idx < 0 || idx >= moduleCount)
        return false;
    return clipFlags[static_cast<size_t>(idx)];
}

float ClipDetector::getPeakLevel(ModuleId module) const noexcept
{
    const auto idx = static_cast<int>(module);
    if (idx < 0 || idx >= moduleCount)
        return 0.0f;
    return peakLevels[static_cast<size_t>(idx)];
}

void ClipDetector::setThreshold(float thresholdLinear) noexcept
{
    threshold = thresholdLinear;
}

void ClipDetector::decayClipFlags() noexcept
{
    for (int i = 0; i < moduleCount; ++i)
    {
        peakLevels[static_cast<size_t>(i)] *= 0.95f;

        if (clipHoldCounters[static_cast<size_t>(i)] > 0)
        {
            --clipHoldCounters[static_cast<size_t>(i)];
            if (clipHoldCounters[static_cast<size_t>(i)] == 0)
                clipFlags[static_cast<size_t>(i)] = false;
        }
    }
}
} // namespace fadmodel1::dsp::diagnostics
