#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <numbers>
#include <fad/engine/math/DspMath.h>
#include <fad/engine/math/Numeric.h>

#include <array>

#include <fad/engine/parameters/ParameterSnapshot.h>
#include "SerumPhaseFilter.h"
#include "CurveNodeFilter.h"
#include "SerumScreamFilter.h"
#include "SerumCombFilter.h"

namespace fadmodel1::dsp::filters
{
class FilterBlock final
{
public:
    struct RuntimeState
    {
        float cutoffHz = 18000.0f;
        float resonance = 0.1f;
        float driveLinear = 1.0f;
    };

    void prepare(double sampleRate);
    void reset();
    void setNativeInputPreShaped(bool enabled) noexcept { nativeInputPreShaped = enabled; }

    // Set coefficients once per block (or sub-block). Avoids per-sample trig.
    void updateCoefficients(const parameters::FilterSnapshot& snapshot,
                            int smoothingSamples = 1) noexcept;

    // Process a single sample using pre-computed coefficients.
    // Legacy drive is in the feedback loop; native drive precedes the filter.
    float processSample(float inputSample) noexcept;

    // Legacy convenience: update coefficients + process (for transition)
    float processSample(float inputSample, const parameters::FilterSnapshot& snapshot) noexcept;

    RuntimeState getRuntimeState() const noexcept;

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    int testGetCoefficientUpdateCount() const noexcept { return coefficientUpdateCount; }
    void testResetCoefficientUpdateCount() noexcept { coefficientUpdateCount = 0; }
    int testGetCoefficientRecalculationCount() const noexcept
    {
        return coefficientRecalculationCount;
    }
    void testResetCoefficientRecalculationCount() noexcept
    {
        coefficientRecalculationCount = 0;
    }
#endif

private:
    CurveNodeFilter nodeFilter;
    double currentSampleRate = 44100.0;
    bool nativeInputPreShaped = false;

    // SVF coefficients (recomputed per block, not per sample)
    float g = 0.0f;   // tan(pi * fc / fs)
    float k = 2.0f;   // 2 * (1 - resonance), controls damping
    float a1 = 0.0f;
    float a2 = 0.0f;
    float a3 = 0.0f;
    float onePoleG = 0.0f;
    float nativeOnePoleF = 0.0f;
    float nativeFeedback = 0.0f;
    float cascadeA1 = 0.0f, cascadeA2 = 0.0f, cascadeA3 = 0.0f, cascadeK = 2.0f;
    float variationA1 = 0.0f, variationA2 = 0.0f, variationA3 = 0.0f, variationK = 2.0f;

    // SVF integrator state
    float s1 = 0.0f;
    float s2 = 0.0f;
    std::array<float, 4> onePoleStates{{0.0f, 0.0f, 0.0f, 0.0f}};
    float nativeBasicPreviousInput = 0.0f;
    std::array<float, 4> stageS1{{0.0f, 0.0f, 0.0f, 0.0f}};
    std::array<float, 4> stageS2{{0.0f, 0.0f, 0.0f, 0.0f}};
    SerumPhaseFilter nativePhase;
    SerumCombFilter nativeComb;
    SerumScreamFilter nativeScream;
    float nativePhasePreviousInput = 0.0f;

    // Drive amount for resonance feedback saturation
    float driveAmount = 0.0f;
    float nativePassbandGain = 1.0f;

    // Current filter mode
    parameters::FilterMode filterMode = parameters::FilterMode::lowPass;
    int nativeFilterModel = -1;
    int filterOrder = 2;
    parameters::FilterMode modeFadeSourceMode = parameters::FilterMode::lowPass;

    // Smoothed parameter targets
    float targetCutoffHz = 18000.0f;
    float targetResonance = 0.1f;
    float targetDriveDb = 0.0f;
    float currentCutoffHz = 18000.0f;
    float currentResonance = 0.1f;
    float currentDriveDb = 0.0f;
    float targetVariation = 0.5f, currentVariation = 0.5f;
    bool coefficientTargetsValid = false;
    int cachedSmoothingSampleCount = 0;
    float cachedSmoothingAlpha = 1.0f;
    int modeFadeSamples = 132;
    int modeFadeRemaining = 0;
    float modeFadeSourceCorrection = 0.0f;
    bool modeFadeNeedsCorrection = false;
    float lastOutputSample = 0.0f;
    bool hasProcessedSample = false;

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    int coefficientUpdateCount = 0;
    int coefficientRecalculationCount = 0;
#endif
};
} // namespace fadmodel1::dsp::filters
