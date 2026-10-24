#pragma once
#include "LinearEnvelope.h"
#include <fad/engine/math/LinearSmoother.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fad/engine/math/DspSafetyUtils.h>
#include "PreciseEnvelope.h"

namespace fadmodel1::dsp::modulation
{
// Reuses the established ADSR when hold is zero. A positive hold pauses the
// ADSR at its attack peak, leaving its current value intact for early note-off.
class HoldEnvelope final
{
public:
    void setSampleRate(double rate) noexcept
    {
        sampleRate = utils::sanitizeSampleRate(rate);
        envelope.setSampleRate(sampleRate);
        for (auto& curve : curves) curve.reset(sampleRate, .005);
        curvesNeedUpdate = true;
        parametersValid = false;
        holdSamplesValid = false;
        setHoldSeconds(holdSeconds);
    }
    void setCurves(const std::array<float,3>& values) noexcept
    {
        // Identical active targets leave the ramp and its pending update intact.
        // Once idle, a still-moving curve must retain the existing snap-to-target
        // behavior rather than carry that ramp into the next note.
        const bool active = envelope.isActive();
        if (sameValue(values[0], curves[0].getTargetValue())
            && sameValue(values[1], curves[1].getTargetValue())
            && sameValue(values[2], curves[2].getTargetValue())
            && (active || (!curves[0].isSmoothing() && !curves[1].isSmoothing()
                && !curves[2].isSmoothing())))
            return;
        curvesNeedUpdate = false;
        for (size_t i=0; i<curves.size(); ++i) {
            const auto k = utils::sanitizeFiniteClamped(values[i],0.0f,-100.0f,100.0f);
            if (active) curves[i].setTargetValue(k);
            else curves[i].setCurrentAndTargetValue(k);
            curvesNeedUpdate = curvesNeedUpdate || curves[i].isSmoothing()
                || !sameValue(curves[i].getCurrentValue(), appliedCurves[i]);
        }
    }
    void setHoldSeconds(float seconds) noexcept
    {
        const auto next = utils::sanitizeFiniteClamped(seconds, 0.0f, 0.0f, 32.0f);
        if (holdSamplesValid && sameValue(holdSeconds, next)) return;
        holdSeconds = next;
        holdSamples = static_cast<uint64_t>(std::llround(sampleRate * holdSeconds));
        holdSamplesValid = true;
    }
    void setParameters(const fad::engine::EnvelopeParameters& parameters) noexcept
    {
        auto applied = parameters;
        // With an instantaneous attack, the first rendered sample must reach
        // the peak even when decay and sustain are both zero.
        if (holdSamples > 0 && applied.attack <= 0.0f)
            applied.attack = static_cast<float>(1.0 / sampleRate);
        if (parametersValid && sameValue(baseParameters.attack,applied.attack) && sameValue(baseParameters.decay,applied.decay)
            && sameValue(baseParameters.sustain,applied.sustain) && sameValue(baseParameters.release,applied.release))
            return;
        baseParameters = applied;
        parametersValid = true;
        if (decayModulationEnabled && gateHeld) applied.decay = envelope.getParameters().decay;
        if (sustainModulationEnabled && gateHeld) applied.sustain = envelope.getParameters().sustain;
        if (releaseModulationEnabled) applied.release = envelope.getParameters().release;
        envelope.setParameters(applied);
    }
    void reset() noexcept {
        envelope.reset();
        lastLinear = lastOutput = releaseLinearStart = releaseOutputStart = 0;
        attackLinearStart = attackOutputStart = 0;
        attackFromCurrentOutput = false;
        for (auto& curve : curves) curve.setCurrentAndTargetValue(curve.getTargetValue());
        curvesNeedUpdate = true;
        if (parametersValid) envelope.setParameters(baseParameters);
        gateHeld = false;
        sustainModulationEnabled = false;
        decayModulationEnabled = false;
        releaseModulationEnabled = false;
        phase = Phase::afterHold; elapsed = 0;
    }
    void noteOn() noexcept {
        // JUCE retains the linear ADSR position on retrigger. A different curve
        // cannot reuse that position as an audible level: anchor the remaining
        // attack to the last rendered output instead. Keep linear ADSR exact.
        attackFromCurrentOutput = envelope.isActive() && baseParameters.attack > 0.0f
            && (appliedCurves[0] != 0.0f || !sameValue(lastOutput,lastLinear));
        attackLinearStart = lastLinear;
        attackOutputStart = lastOutput;
        gateHeld = true;
        envelope.noteOn();
        phase = baseParameters.attack > 0 ? Phase::attack : Phase::afterHold;
        elapsed = 0;
    }
    void noteOff() noexcept {
        if (!gateHeld) return;
        releaseLinearStart = lastLinear; releaseOutputStart = lastOutput;
        gateHeld = false; phase = Phase::afterHold; envelope.noteOff();
    }
    void setSustainModulationEnabled(bool enabled) noexcept {
        if (sustainModulationEnabled == enabled) return;
        sustainModulationEnabled = enabled;
        if (!enabled && gateHeld && parametersValid) {
            auto parameters = baseParameters;
            if (decayModulationEnabled) parameters.decay = envelope.getParameters().decay;
            envelope.setParameters(parameters);
        }
    }
    void setDecayModulationEnabled(bool enabled) noexcept {
        if (decayModulationEnabled == enabled) return;
        decayModulationEnabled = enabled;
        if (!enabled && gateHeld && parametersValid) {
            auto parameters = baseParameters;
            if (sustainModulationEnabled) parameters.sustain = envelope.getParameters().sustain;
            envelope.setParameters(parameters);
        }
    }
    void setReleaseModulationEnabled(bool enabled) noexcept {
        if (releaseModulationEnabled == enabled) return;
        releaseModulationEnabled = enabled;
        if (!enabled && parametersValid) {
            auto parameters = envelope.getParameters();
            parameters.release = baseParameters.release;
            envelope.setParameters(parameters);
        }
    }
    static float serumDecayPosition(float seconds) noexcept {
        // Preserve the five-power coordinate above the legacy 32-second range.
        // Wide envelope times must reach the modulation writer unsaturated.
        const auto bounded = utils::sanitizeFiniteClamped(seconds, 0.0f, 0.0f, 300.0f);
        return std::pow(bounded / 32.0f, 0.2f);
    }
    static float serumReleasePosition(float seconds) noexcept { return serumDecayPosition(seconds); }
    void setModulatedSerumDecay(float basePosition, float offset) noexcept {
        if (!decayModulationEnabled || !gateHeld) return;
        const auto seconds = modulatedSerumSeconds(baseParameters.decay, basePosition, offset);
        auto parameters = envelope.getParameters();
        if (sameValue(parameters.decay, seconds)) return;
        parameters.decay = seconds;
        envelope.setParameters(parameters);
    }
    void setModulatedSustain(float level) noexcept {
        if (!sustainModulationEnabled || !gateHeld) return;
        auto parameters = envelope.getParameters();
        level = utils::sanitizeFiniteClamped(level, 0.0f, 0.0f, 1.0f);
        if (sameValue(parameters.sustain,level)) return;
        parameters.sustain = level;
        envelope.setParameters(parameters);
    }
    void setModulatedSerumRelease(float basePosition, float offset) noexcept {
        if (!releaseModulationEnabled || !parametersValid) return;
        const auto seconds = modulatedSerumSeconds(baseParameters.release, basePosition, offset);
        auto parameters = envelope.getParameters();
        if (sameValue(parameters.release, seconds)) return;
        parameters.release = seconds;
        envelope.setParameters(parameters);
    }
    bool isActive() const noexcept { return envelope.isActive(); }
    bool canSkipSilentSamples() const noexcept
    {
        return ! envelope.isActive() && phase != Phase::hold && ! curvesNeedUpdate
            && lastLinear == 0.0f && lastOutput == 0.0f;
    }
    float getNextSample() noexcept
    {
        if (curvesNeedUpdate) {
            curvesNeedUpdate = false;
            for (size_t i=0; i<curves.size(); ++i) {
                const auto k = curves[i].getNextValue();
                if (!sameValue(k, appliedCurves[i])) {
                    appliedCurves[i] = k;
                    curveEnd[i] = std::exp(-std::abs(k));
                    curveScale[i] = std::abs(k) < 1.e-5f ? 1.0f : 1.0f/(1.0f-curveEnd[i]);
                }
                curvesNeedUpdate = curvesNeedUpdate || curves[i].isSmoothing();
            }
            linearCurves = appliedCurves[0] == 0.0f && appliedCurves[1] == 0.0f
                && appliedCurves[2] == 0.0f;
        }
        if (phase == Phase::hold)
        {
            if (elapsed < holdSamples) { ++elapsed; lastLinear = lastOutput = 1.0f; return 1.0f; }
            phase = Phase::afterHold;
        }
        const bool attacking = phase == Phase::attack;
        const auto value = envelope.getNextSample();
        if (attacking && value >= 1.0f)
            phase = holdSamples > 0 ? Phase::hold : Phase::afterHold;
        // Flat curves need no shape dispatch. Keep retrigger/release anchors
        // on the shaped path when their audible and linear levels differ.
        if (linearCurves && !(gateHeld && attacking && attackFromCurrentOutput)
            && (gateHeld || !(releaseLinearStart > 0.0f)
                || sameValue(releaseOutputStart, releaseLinearStart)))
        {
            lastLinear = value;
            lastOutput = utils::sanitizeFiniteClamped(value,0.0f,0.0f,1.0f);
            return lastOutput;
        }
        auto output = value;
        if (gateHeld && attacking && attackFromCurrentOutput)
        {
            const auto progress = attackLinearStart < 1.0f
                ? (value - attackLinearStart) / (1.0f - attackLinearStart) : 1.0f;
            output = attackOutputStart + (1.0f - attackOutputStart) * shape(progress,0);
        }
        else if (gateHeld && attacking && appliedCurves[0] != 0.0f)
            output = shape(value,0);
        else if (gateHeld && !attacking && appliedCurves[1] != 0.0f) {
            const auto sustain = envelope.getParameters().sustain;
            if (sustain < 1.0f && value > sustain)
                output = 1.0f-(1.0f-sustain)*shape((1.0f-value)/(1.0f-sustain),1);
        } else if (!gateHeld && releaseLinearStart > 0.0f
                 && (appliedCurves[2] != 0.0f || !sameValue(releaseOutputStart,releaseLinearStart)))
            output = releaseOutputStart * (1.0f-shape(1.0f-value/releaseLinearStart,2));
        lastLinear = value;
        lastOutput = utils::sanitizeFiniteClamped(output,0.0f,0.0f,1.0f);
        return lastOutput;
    }

private:
    static float modulatedSerumSeconds(float baseSeconds, float basePosition, float offset) noexcept {
        const bool wide = baseSeconds > 32.0f;
        // A zero-valued source leaves a wide baseline exact, including during
        // release. Legacy times retain their original modulation curve and cap.
        if (wide && offset == 0.0f)
            return utils::sanitizeFiniteClamped(baseSeconds, 0.0f, 0.0f, 300.0f);
        // 2^5 * 32 exceeds the wide limit; clamp seconds after decoding. This
        // bounds the arithmetic without a pow() in the per-sample writer.
        const auto position = utils::sanitizeFiniteClamped(basePosition + offset, 0.0f, 0.0f, wide ? 2.0f : 1.0f);
        const auto squared = position * position;
        const auto seconds = 32.0f * squared * squared * position;
        return wide ? std::min(seconds, 300.0f) : seconds;
    }
    float shape(float t,size_t index) const noexcept {
        t=std::clamp(t, 0.0f, 1.0f);
        const auto k=appliedCurves[index];
        if (std::abs(k)<1.e-5f || t==0.0f || t==1.0f) return t;
        // The exponent is always nonpositive, including extreme imported
        // curvatures. No overflow, allocation or table rebuild in the callback.
        return k>0 ? (std::exp(k*(t-1.0f))-curveEnd[index])*curveScale[index]
                   : (1.0f-std::exp(k*t))*curveScale[index];
    }
    std::array<fad::engine::math::LinearSmoother<float>,3> curves;
    std::array<float,3> appliedCurves{}, curveEnd{1,1,1}, curveScale{1,1,1};
    float lastLinear=0, lastOutput=0, releaseLinearStart=0, releaseOutputStart=0;
    float attackLinearStart=0, attackOutputStart=0;
    bool attackFromCurrentOutput=false;
    bool curvesNeedUpdate=true;
    bool linearCurves=true;
    static bool sameValue(float a,float b) noexcept { return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b); }
    enum class Phase { attack, hold, afterHold };
    PreciseEnvelope envelope;
    fad::engine::EnvelopeParameters baseParameters;
    bool parametersValid = false, gateHeld = false, sustainModulationEnabled = false, decayModulationEnabled = false,
         releaseModulationEnabled = false;
    double sampleRate = 44100.0;
    float holdSeconds = 0.0f;
    bool holdSamplesValid = false;
    uint64_t holdSamples = 0, elapsed = 0;
    Phase phase = Phase::afterHold;
};
}
