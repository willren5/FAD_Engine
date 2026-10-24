#pragma once
#include <fad/engine/filters/Oversample2x.h>
#include <array>

namespace fad::engine::filters
{
// Prepared numerical filter-design output; origin and review scope are
// documented in docs/PROVENANCE.md. This is not a claim of third-party rights
// clearance. No JUCE code or runtime dependency is included.
namespace allpass_design_data
{
inline constexpr std::array<float, 3> standard_up_direct{
    0x1.3210ba0000000p-4f, 0x1.f3bafc0000000p-2f, 0x1.cc5f8c0000000p-1f};
inline constexpr std::array<float, 2> standard_up_delayed{
    0x1.0c3baa0000000p-2f, 0x1.679ebe0000000p-1f};
inline constexpr std::array<float, 2> standard_down_direct{
    0x1.87f6d20000000p-4f, 0x1.30f7620000000p-1f};
inline constexpr std::array<float, 2> standard_down_delayed{
    0x1.4ed1040000000p-2f, 0x1.b821020000000p-1f};
inline constexpr std::array<float, 4> high_up_direct{
    0x1.769ae00000000p-5f, 0x1.547b2c0000000p-2f, 0x1.538f380000000p-1f, 0x1.de225a0000000p-1f};
inline constexpr std::array<float, 3> high_up_delayed{
    0x1.583e480000000p-3f, 0x1.024bf40000000p-1f, 0x1.9b892a0000000p-1f};
inline constexpr std::array<float, 3> high_down_direct{
    0x1.bc26640000000p-5f, 0x1.88480c0000000p-2f, 0x1.7f585a0000000p-1f};
inline constexpr std::array<float, 3> high_down_delayed{
    0x1.930ad00000000p-3f, 0x1.2572220000000p-1f, 0x1.d41e4e0000000p-1f};
}
inline Allpass2xCoefficients preparedAllpass2x(bool highQuality) noexcept
{
    using namespace allpass_design_data;
    return highQuality ? Allpass2xCoefficients{high_up_direct, high_up_delayed, high_down_direct, high_down_delayed}
                       : Allpass2xCoefficients{standard_up_direct, standard_up_delayed, standard_down_direct, standard_down_delayed};
}
}
