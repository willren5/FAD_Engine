#pragma once
#include <fad/engine/AudioBlock.h>
#include <fad/engine/filters/FractionalDelay.h>
#include <fad/engine/math/LinearSmoother.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace fad::engine::filters
{
template<class Sample> struct MonoChorusSettings
{
    Sample rate = 1, depth = static_cast<Sample>(.25), delayMilliseconds = 7;
    Sample feedback = 0, mix = static_cast<Sample>(.5);
};

// Streaming mono chorus for the existing two independently clocked channels
// (and the double-precision wide-rate path). A sinusoidal millisecond delay,
// subtractive feedback and linear dry/wet ramps own scalar state here; no
// oscillator function object, dry FIFO, JUCE object or render allocation.
template<class Sample> class MonoChorus
{
public:
    void prepare(double sampleRate)
    {
        rate_ = sampleRate;
        phaseRate_ = static_cast<Sample>(sampleRate);
        delay_.setMaximumDelayInSamples(static_cast<int>(std::ceil(110.0 * sampleRate / 1000.0)));
        delay_.prepare({sampleRate, 1, 1});
        configure(settings_);
        reset();
    }
    void configure(const MonoChorusSettings<Sample>& settings) noexcept
    {
        settings_ = settings;
        settings_.delayMilliseconds = std::clamp(settings.delayMilliseconds, Sample{1}, Sample{100});
        frequency_.setTargetValue(settings.rate);
        depth_.setTargetValue(settings.depth * static_cast<Sample>(.5));
        feedback_.setTargetValue(settings.feedback);
        dry_.setTargetValue(Sample{1} - settings.mix);
        wet_.setTargetValue(settings.mix);
    }
    void setRate(Sample value) noexcept { auto next = settings_; next.rate = value; configure(next); }
    void setDepth(Sample value) noexcept { auto next = settings_; next.depth = value; configure(next); }
    void setCentreDelay(Sample value) noexcept
    { settings_.delayMilliseconds = std::clamp(value, Sample{1}, Sample{100}); }
    void setFeedback(Sample value) noexcept { auto next = settings_; next.feedback = value; configure(next); }
    void setMix(Sample value) noexcept { auto next = settings_; next.mix = value; configure(next); }
    void reset() noexcept
    {
        phase_ = previousFeedback_ = 0;
        delay_.reset();
        frequency_.reset(phaseRate_, .05);
        depth_.reset(rate_, .05);
        feedback_.reset(rate_, .05);
        dry_.reset(rate_, .05);
        wet_.reset(rate_, .05);
    }
    void process(const ProcessContextReplacing<Sample>& context) noexcept
    {
        const auto& block = context.getOutputBlock();
        assert(block.getNumChannels() == 1);
        if (context.isBypassed) return;
        const Sample twoPi = Sample{2} * std::numbers::pi_v<Sample>;
        const Sample phaseScale = twoPi / phaseRate_;
        auto* samples = block.getChannelPointer(0);
        for (size_t sample = 0; sample < block.getNumSamples(); ++sample)
        {
            const Sample input = samples[sample];
            const Sample oldPhase = phase_;
            phase_ = advancePhase(phase_, phaseScale, frequency_.getNextValue(), twoPi);
            const Sample modulation = std::sin(oldPhase - std::numbers::pi_v<Sample>) * depth_.getNextValue();
            const Sample milliseconds = std::max(Sample{1}, Sample{20} * modulation + settings_.delayMilliseconds);
            const Sample delaySamples = static_cast<Sample>(milliseconds * rate_ / 1000.0);
            delay_.pushSample(0, input - previousFeedback_);
            const Sample delayed = delay_.popSample(0, delaySamples);
            previousFeedback_ = delayed * feedback_.getNextValue();
            samples[sample] = mixProducts(delayed, wet_.getNextValue(), input, dry_.getNextValue());
        }
    }
private:
    static Sample advancePhase(Sample phase, Sample scale, Sample frequency, Sample period) noexcept
    {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        const Sample increment = scale * frequency;
        Sample next = phase + increment;
        while (next >= period) next -= period;
        return next;
    }
    static Sample mixProducts(Sample wet, Sample wetGain, Sample dry, Sample dryGain) noexcept
    {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        const Sample wetContribution = wet * wetGain;
        const Sample dryContribution = dry * dryGain;
        return wetContribution + dryContribution;
    }
    MonoChorusSettings<Sample> settings_;
    FractionalDelay<Sample> delay_;
    math::LinearSmoother<Sample> frequency_{Sample{1}}, depth_{static_cast<Sample>(.125)},
        feedback_{Sample{}}, dry_{static_cast<Sample>(.5)}, wet_{static_cast<Sample>(.5)};
    double rate_ = 44100;
    Sample phaseRate_ = 44100, phase_ = 0, previousFeedback_ = 0;
};
}
