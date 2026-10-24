#pragma once

#include <fad/engine/AudioView.h>

#include <fad/engine/math/DspSafetyUtils.h>

#include <array>
#include <cmath>

namespace fad::engine
{
// Musical output gain control, separate from AudioSafetyStage's quarantine
// guard. No lookahead or host latency; stereo shares one gain envelope. Normal
// levels pass unchanged. Overloads reduce gain, hold briefly, then recover
// smoothly instead of flattening individual samples with a hard clip.
class OutputPeakLimiter
{
public:
    static constexpr float ceiling = 0.891250938f; // -1 dBFS sample-peak headroom

    void prepare(double sampleRate) noexcept
    {
        const auto rate = fadmodel1::dsp::utils::sanitizeSampleRate(sampleRate);
        releaseStep = -std::expm1(-1.0 / (0.080 * rate));
        holdSamples = static_cast<int>(std::ceil(0.005 * rate));
        reset();
    }

    void reset() noexcept
    {
        gain = 1.0f;
        holdRemaining = 0;
    }

    // Returns post-limiter peaks without a second metering pass. Processing is
    // sample based so host block partitioning cannot change the gain envelope.
    std::array<float, 2> process(AudioBufferView buffer) noexcept
    {
        const auto channels = std::min(2, buffer.channels());
        std::array<float, 2> peaks{};
        if (channels <= 0)
            return peaks;

        auto* left = buffer.channel(0);
        auto* right = channels > 1 ? buffer.channel(1) : nullptr;
        for (int sample = 0; sample < buffer.frames(); ++sample)
        {
            const auto inputL = fadmodel1::dsp::utils::sanitizeFinite(left[sample]);
            const auto inputR = right != nullptr ? fadmodel1::dsp::utils::sanitizeFinite(right[sample]) : 0.0f;
            const auto peak = std::max(std::abs(inputL), std::abs(inputR));
            const auto target = peak > ceiling ? ceiling / peak : 1.0f;
            if (target <= gain && target < 1.0f)
            {
                gain = target;
                holdRemaining = holdSamples;
            }
            else if (holdRemaining > 0)
            {
                --holdRemaining;
            }
            else
            {
                gain += (target - gain) * releaseStep;
                // End the recovery at unity rather than retaining float
                // rounding residue on the next quiet passage.
                if (target >= 1.0f && gain > 0.99999)
                    gain = 1.0f;
            }

            const auto appliedGain = static_cast<float>(gain);
            left[sample] = inputL * appliedGain;
            peaks[0] = std::max(peaks[0], std::abs(left[sample]));
            if (right != nullptr)
            {
                right[sample] = inputR * appliedGain;
                peaks[1] = std::max(peaks[1], std::abs(right[sample]));
            }
        }
        if (right == nullptr)
            peaks[1] = peaks[0];
        return peaks;
    }

private:
    double gain = 1.0;
    double releaseStep = 0.000260383f; // 48 kHz, 80 ms
    int holdSamples = 240;
    int holdRemaining = 0;
};
}
