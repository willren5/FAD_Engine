#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <type_traits>

namespace fad::engine::math
{
// Behavior contract: finite target values; reset cancels the current ramp at its
// previous target; the last sample lands exactly on the new target; retargeting
// starts a full-duration ramp from the current value. Thresholds below reproduce
// the documented floating comparison domain used by the private consumer.
// Written for this extraction from that behavior contract; no vendor code copied.
template<class Float> class LinearSmoother
{
    static_assert(std::is_floating_point_v<Float>);
public:
    explicit LinearSmoother(Float initial = Float{}) noexcept : value_(initial), target_(initial) {}
    void reset(int samples) noexcept
    {
        assert(samples >= 0);
        duration_ = samples;
        setCurrentAndTargetValue(target_);
    }
    void reset(double rate, double seconds) noexcept
    {
        const double count = std::floor(rate * seconds);
        assert(std::isfinite(count) && count >= 0 && count <= std::numeric_limits<int>::max());
        reset(static_cast<int>(count));
    }
    void setCurrentAndTargetValue(Float next) noexcept
    { value_ = target_ = next; remaining_ = 0; }
    Float getCurrentValue() const noexcept { return value_; }
    Float getTargetValue() const noexcept { return target_; }
    bool isSmoothing() const noexcept { return remaining_ > 0; }
    int getNumSamplesToReachTarget() const noexcept { return remaining_; }

    void setTargetValue(Float next) noexcept
    {
        if (sameTarget(next)) return;
        if (duration_ == 0) { setCurrentAndTargetValue(next); return; }
        target_ = next;
        remaining_ = duration_;
        increment_ = (target_ - value_) / static_cast<Float>(duration_);
    }
    Float getNextValue() noexcept
    {
        if (remaining_ == 0) return target_;
        --remaining_;
        if (remaining_ == 0) value_ = target_;
        else value_ += increment_;
        return value_;
    }
    Float skip(int samples) noexcept
    {
        assert(samples >= 0);
        if (samples >= remaining_) { setCurrentAndTargetValue(target_); return target_; }
        value_ += increment_ * static_cast<Float>(samples);
        remaining_ -= samples;
        return value_;
    }
private:
    bool sameTarget(Float next) const noexcept
    {
        if (next == target_) return true;
        if (!std::isfinite(next) || !std::isfinite(target_)) return false;
        const Float difference = std::abs(next - target_);
        return difference <= std::numeric_limits<Float>::min()
            || difference <= std::max(std::abs(next), std::abs(target_)) * std::numeric_limits<Float>::epsilon();
    }
    Float value_{}, target_{}, increment_{};
    int duration_ = 0, remaining_ = 0;
};
}
