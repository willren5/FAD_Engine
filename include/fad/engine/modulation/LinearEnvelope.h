#pragma once
#include <array>
#include <cassert>

namespace fad::engine
{
struct EnvelopeParameters
{
    float attack = .1f, decay = .1f, sustain = 1.f, release = .1f;
};

// Independent linear stage machine for the existing FAD envelope contract.
// Coefficients use float distances / double sample-time, then float storage.
// Reset, retrigger and live-parameter transition behavior are checked against
// the pinned private backend; this is not a new preset/time interpretation.
class LinearEnvelope final
{
public:
    LinearEnvelope() noexcept { setParameters(settings_); }
    void setSampleRate(double rate) noexcept { assert(rate > 0); rate_ = rate; }
    const EnvelopeParameters& getParameters() const noexcept { return settings_; }
    bool isActive() const noexcept { return mode_ != Mode::off; }
    void reset() noexcept { mode_ = Mode::off; value_ = 0; }

    void setParameters(const EnvelopeParameters& settings) noexcept
    {
        settings_ = settings;
        slopes_[0] = slope(1.f, settings.attack);
        slopes_[1] = slope(1.f - settings.sustain, settings.decay);
        slopes_[2] = slope(settings.sustain, settings.release);
        bool completed = false;
        if (mode_ == Mode::rising) completed = slopes_[0] <= 0;
        if (mode_ == Mode::falling) completed = slopes_[1] <= 0 || value_ <= settings_.sustain;
        if (mode_ == Mode::releasing) completed = slopes_[2] <= 0;
        if (completed) finishStage();
    }
    void noteOn() noexcept
    {
        mode_ = slopes_[0] > 0 ? Mode::rising : slopes_[1] > 0 ? Mode::falling : Mode::holding;
        if (mode_ != Mode::rising) value_ = mode_ == Mode::falling ? 1.f : settings_.sustain;
    }
    void noteOff() noexcept
    {
        if (!isActive()) return;
        if (!(settings_.release > 0)) { reset(); return; }
        slopes_[2] = static_cast<float>(value_ / (settings_.release * rate_));
        mode_ = Mode::releasing;
    }
    float getNextSample() noexcept
    {
        if (mode_ == Mode::off) return 0;
        if (mode_ == Mode::holding) return value_ = settings_.sustain;
        const bool rising = mode_ == Mode::rising;
        const auto index = rising ? 0u : mode_ == Mode::falling ? 1u : 2u;
        const float boundary = rising ? 1.f : mode_ == Mode::falling ? settings_.sustain : 0.f;
        if (rising) value_ += slopes_[index];
        else value_ -= slopes_[index];
        if (rising ? value_ >= boundary : value_ <= boundary)
        { value_ = boundary; finishStage(); }
        return value_;
    }
private:
    enum class Mode { off, rising, falling, holding, releasing };
    float slope(float distance, float seconds) const noexcept
    { return seconds > 0 ? static_cast<float>(distance / (seconds * rate_)) : -1.f; }
    void finishStage() noexcept
    {
        if (mode_ == Mode::releasing) { reset(); return; }
        mode_ = mode_ == Mode::rising && slopes_[1] > 0 ? Mode::falling : Mode::holding;
    }
    Mode mode_ = Mode::off;
    EnvelopeParameters settings_;
    double rate_ = 44100;
    float value_ = 0;
    std::array<float, 3> slopes_{};
};
}
