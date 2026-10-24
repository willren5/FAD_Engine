#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fadmodel1::dsp::oscillators
{
using SerumPhaseRandomValues = std::array<std::array<float, 16>, 2>;

// Private equivalent of the macOS Serum 1.368 reference's initial rand stream.
// Keeps FAD deterministic and independent of other plug-ins' process-global RNG.
// Evidence: native-phase-generator-proof.json in serum-takeover-4e040911.
class SerumPhaseRandom
{
public:
    SerumPhaseRandom() noexcept { reset(); }

    void reset() noexcept
    {
        state = 1;
        for (int i = 0; i < 33; ++i) nextInteger();
    }

    SerumPhaseRandomValues nextNote() noexcept
    {
        SerumPhaseRandomValues result{};
        // The native stream consumes all 70 draws regardless of active unison count.
        for (int i = 0; i < 70; ++i)
        {
            const auto value = static_cast<float>(nextInteger() % 100u) * .01f;
            if (i == 0) result[0][0] = result[1][0] = value;
            else if (i >= 37 && i <= 51) result[0][static_cast<size_t>(i - 36)] = value;
            else if (i >= 53 && i <= 67) result[1][static_cast<size_t>(i - 52)] = value;
        }
        return result;
    }

private:
    uint32_t nextInteger() noexcept
    {
        constexpr uint32_t modulus = 2147483647u;
        const uint64_t product = static_cast<uint64_t>(state) * 16807u;
        state = static_cast<uint32_t>((product & modulus) + (product >> 31u));
        if (state >= modulus) state -= modulus;
        return state;
    }
    uint32_t state = 1;
};

inline float serumStartPhase(float basePhase, float randomAmount, float randomValue) noexcept
{
    basePhase = std::isfinite(basePhase) ? basePhase - std::floor(basePhase) : 0.0f;
    randomAmount = std::isfinite(randomAmount) ? std::clamp(randomAmount, 0.0f, 1.0f) : 0.0f;
    randomValue = std::isfinite(randomValue) ? std::clamp(randomValue, 0.0f, 1.0f) : 0.0f;
    const auto phase = std::floor(2048.0f * (basePhase + randomValue * randomAmount * randomAmount)) / 2048.0f;
    return phase - std::floor(phase);
}
}
