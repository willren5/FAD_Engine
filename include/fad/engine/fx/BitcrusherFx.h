#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/detail/PlanarBuffer.h>
#include <fad/engine/math/Lcg48.h>
#include <fad/engine/math/RuntimeRange.h>

#include <fad/engine/fx/BaseFxProcessor.h>

namespace fadmodel1::dsp::fx
{
class BitcrusherFxCore : public BaseFxProcessorCore
{
public:
    void prepare(const fad::engine::ProcessSpec& spec) override;
    void process(fad::engine::ProcessContextReplacing<float>& context) override;
    void reset() override;
    void setBypassed(bool shouldBeBypassed) override;
    void setGenericParam(int index, float normalizedValue) override;

private:
    void processActiveBlock(fad::engine::ProcessContextReplacing<float>& context);

    static constexpr int64_t jitterSeed = 0x4d595df4d0f33173LL;

    float holdL = 0.0f;
    float holdR = 0.0f;
    float counterL = 0.0f;
    float counterR = 0.0f;

    fad::engine::math::Lcg48 rng{jitterSeed};
    fad::engine::detail::PlanarBuffer dryBuffer;

    fad::engine::math::LinearSmoother<float> bits;
    fad::engine::math::LinearSmoother<float> crushRate;
    fad::engine::math::LinearSmoother<float> mix;
    fad::engine::math::LinearSmoother<float> jitter;
    fad::engine::math::LinearSmoother<float> driveGain;
    float crushRateNormalized = 1.0f;

    double sampleRate = 0.0;
    int numChannels = 0;
    bool parametersPrimed = false;
    std::atomic<bool> bypassed{false};
};
}
