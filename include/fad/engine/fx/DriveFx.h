#pragma once

#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/filters/TptStateVariable.h>
#include <fad/engine/math/LinearSmoother.h>
#include <array>
#include <atomic>

#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fadmodel1::dsp::fx
{
class DriveFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;

    void setSnapshot(const parameters::DriveSnapshot& newSnapshot) noexcept;

private:
    void processActiveBlock(fad::engine::ProcessContextReplacing<float>& context);
    static float processWave(float input, parameters::DriveMode mode) noexcept;
    void updateToneFilters() noexcept;

    parameters::DriveSnapshot snapshot;
    fad::engine::detail::PlanarBuffer dryBuffer;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> highPassFilters;
    std::array<fad::engine::filters::TptStateVariable<float>, 2> lowPassFilters;
    fad::engine::math::LinearSmoother<float> driveGain;
    fad::engine::math::LinearSmoother<float> wetMix;
    std::atomic<int> requestedMode{0};
    int activeMode = 0;
    int modeFadeSamples = 132;
    int modeFadeRemaining = 0;
    std::array<float, 2> modeFadeStartSamples{};
    std::array<float, 2> lastOutputSamples{};
    bool hasProcessedSample = false;
    double sampleRate = 0.0;
    int numChannels = 0;
    std::atomic<bool> bypassed{false};
};
}
