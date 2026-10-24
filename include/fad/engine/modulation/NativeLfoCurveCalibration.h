#pragma once
#include <array>
namespace fadmodel1::dsp::modulation {
// Empirical exponential segment coefficients, Serum 1.368, 48 kHz.
// Derived from isolated sub-sine amplitude modulation, not third-party DSP source.
// Compatibility measurements are described in docs/PROVENANCE.md.
// Curvature positions are i/40. Endpoints approach a step. This remains an approximation.
inline constexpr std::array<double, 41> serumLfoCurveCoefficients{
    -100.000000, -19.880000, -15.185000, -12.880000, -11.215000, -9.710000, -8.660000, -7.620000, -6.845000, -6.145000, -5.410000, -4.825000, -4.280000, -3.675000, -3.180000, -2.620000, -2.140000, -1.665000, -1.100000, -0.590000, 0.000000, 0.580000, 1.085000, 1.650000, 2.125000, 2.595000, 3.155000, 3.650000, 4.160000, 4.790000, 5.365000, 6.090000, 6.780000, 7.540000, 8.560000, 9.585000, 11.050000, 12.670000, 14.905000, 19.450000, 100.000000
};
}
