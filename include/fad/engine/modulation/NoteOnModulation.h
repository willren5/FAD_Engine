#pragma once

#include <array>
#include <cstdint>

namespace fadmodel1::dsp::modulation
{
// Serum sources 20/21 hold two 0..0.999 random values per note; 22/23
// alternate complementary 0/1 values. The sequence belongs to the instrument,
// not its voice allocator. FAD uses a private, declared-seed random stream.
class NoteOnModulation final
{
public:
    void reset(uint32_t seed = 0) noexcept
    {
        state = 1u + seed % 2147483646u;
        alternate = false;
    }

    std::array<float, 4> next() noexcept
    {
        const auto first = nextRandom();
        const auto second = nextRandom();
        const auto value = alternate ? 1.0f : 0.0f;
        alternate = !alternate;
        return {first, second, value, 1.0f - value};
    }

private:
    float nextRandom() noexcept
    {
        state = static_cast<uint32_t>((static_cast<uint64_t>(state) * 16807u) % 2147483647u);
        return static_cast<float>(state % 1000u) * .001f;
    }

    uint32_t state = 1;
    bool alternate = false;
};
}
