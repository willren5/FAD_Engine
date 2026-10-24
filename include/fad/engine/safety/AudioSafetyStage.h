#pragma once

#include <fad/engine/AudioView.h>
#include <fad/engine/math/LinearSmoother.h>
#include <array>

#include <cstdint>

namespace fad::engine
{
class AudioSafetyStage
{
public:
    static constexpr float emergencyKneeStart = 4.0f;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setNativeCompatibility(bool enabled) noexcept;
    /// Processes the buffer in place and returns the exact number of samples
    /// whose post-DC value crossed the emergency knee and was shaped.
    uint64_t process(AudioBufferView buffer) noexcept;

private:
    std::array<float, 2> previousInput{};
    std::array<float, 2> previousOutput{};
    double sampleRate = 48000.0;
    bool nativeCompatibility = false;
    bool hasProcessedSamples = false;
    math::LinearSmoother<float> dcBlockCoeff{0.995f};
};
} // namespace fad::engine
