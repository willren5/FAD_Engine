#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>

namespace fad::engine::math
{
// Knuth/Java-family 48-bit linear congruential recurrence. FAD's established
// jitter stream uses the supplied raw state (no Java constructor seed xor).
// The full 32-bit float conversion and upper clamp preserve that stream's
// quantisation. One processor/audio owner; no global entropy or locking.
class Lcg48
{
public:
    explicit Lcg48(uint64_t seed) noexcept : state_(seed) {}
    void setSeed(uint64_t seed) noexcept { state_ = seed; }
    uint64_t getSeed() const noexcept { return state_; }
    int32_t nextInt() noexcept
    {
        state_ = (state_ * uint64_t{25214903917} + uint64_t{11}) & ((uint64_t{1} << 48) - 1);
        return std::bit_cast<int32_t>(static_cast<uint32_t>(state_ >> 16));
    }
    float nextFloat() noexcept
    {
        const auto word = std::bit_cast<uint32_t>(nextInt());
        return std::min(static_cast<float>(word) / 4294967296.0f, 1.0f - std::numeric_limits<float>::epsilon());
    }
private:
    uint64_t state_;
};
}
