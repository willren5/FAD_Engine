// Keep implicit multiply/add rounding aligned with the Windows precise build,
// including inline pitch and phase calculations from headers. Explicit fma remains fused.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include <fad/engine/oscillators/OscillatorBlock.h>
#include <fad/engine/oscillators/WarpPhase.h>
#include <fad/engine/oscillators/SerumBendWarp.h>

#include <fad/engine/math/DspSafetyUtils.h>

#include <bit>
#include <fad/engine/math/RationalTanh.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
#include <immintrin.h>
#endif

#if defined(_MSC_VER) && defined(_M_X64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#endif

namespace fadmodel1::dsp::oscillators
{
namespace
{
#if defined(_MSC_VER) && defined(_M_X64)
#define FAD_OSC_LOCAL_INLINE __forceinline
#else
#define FAD_OSC_LOCAL_INLINE inline
#endif

constexpr int maxOversampleFactor = 4;
constexpr double minimumFmOversampleRate = 44100.0 * maxOversampleFactor;
constexpr double minimumWarpOversampleRate = 96000.0;

#if defined(_MSC_VER) && defined(_M_X64)
// Same per-lane divide/subtract/multiply/add order as the scalar decimator.
// Snap-to-zero and finite recovery remain at the existing scalar boundary.
void decimateFmEight(float* previous, const float* accumulated, int factor) noexcept
{
    const auto divisor = _mm_set1_ps(static_cast<float>(factor));
    for (int offset = 0; offset < 8; offset += 4)
    {
        const auto before = _mm_loadu_ps(previous + offset);
        const auto decimated = _mm_div_ps(_mm_loadu_ps(accumulated + offset), divisor);
        _mm_storeu_ps(previous + offset, _mm_add_ps(before,
            _mm_mul_ps(_mm_set1_ps(0.55f), _mm_sub_ps(decimated, before))));
    }
}
#endif

float foldFmCarrierAngle(float angle) noexcept
{
    if (! utils::isFinite(angle))
        return 0.0f;

    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float halfPi = (std::numbers::pi_v<float> / static_cast<float>(2));
    constexpr float twoPi = (static_cast<float>(2) * std::numbers::pi_v<float>);
    constexpr float inverseTwoPi = 1.0f / twoPi;
    const auto turns = static_cast<int>(
        angle * inverseTwoPi + (angle >= 0.0f ? 0.5f : -0.5f));
    angle -= static_cast<float>(turns) * twoPi;
    const auto sign = std::copysign(1.0f, angle);
    auto magnitude = std::abs(angle);
    if (magnitude > halfPi)
        magnitude = pi - magnitude;
    return sign * magnitude;
}

#if defined(_MSC_VER) && defined(_M_X64)
__m128 foldFmCarrierAnglesFour(__m128 angles) noexcept
{
    // Match the scalar operation order, including truncation and signed zero.
    // FM callers bound the carrier angle with phase in [0, 1) and index <= 100.
    const auto signBits = _mm_set1_ps(-0.0f);
    const auto magnitudeBits = _mm_andnot_ps(signBits, angles);
    const auto finite = _mm_castsi128_ps(_mm_cmplt_epi32(
        _mm_castps_si128(magnitudeBits), _mm_set1_epi32(0x7f800000)));
    angles = _mm_and_ps(angles, finite);

    const auto nonnegative = _mm_cmpge_ps(angles, _mm_setzero_ps());
    const auto bias = _mm_or_ps(_mm_and_ps(nonnegative, _mm_set1_ps(0.5f)),
                              _mm_andnot_ps(nonnegative, _mm_set1_ps(-0.5f)));
    const auto turns = _mm_cvttps_epi32(_mm_add_ps(
        _mm_mul_ps(angles, _mm_set1_ps(1.0f / (static_cast<float>(2) * std::numbers::pi_v<float>))),
        bias));
    angles = _mm_sub_ps(angles, _mm_mul_ps(
        _mm_cvtepi32_ps(turns), _mm_set1_ps((static_cast<float>(2) * std::numbers::pi_v<float>))));
    const auto magnitude = _mm_andnot_ps(signBits, angles);
    const auto reflect = _mm_cmpgt_ps(
        magnitude, _mm_set1_ps((std::numbers::pi_v<float> / static_cast<float>(2))));
    const auto folded = _mm_or_ps(
        _mm_and_ps(reflect, _mm_sub_ps(_mm_set1_ps(std::numbers::pi_v<float>), magnitude)),
        _mm_andnot_ps(reflect, magnitude));
    return _mm_xor_ps(folded, _mm_and_ps(signBits, angles));
}
#endif

float fusedMultiplyAdd(float left, float right, float addend,
                       [[maybe_unused]] bool nativeFma) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    if (nativeFma)
        return _mm_cvtss_f32(_mm_fmadd_ss(
            _mm_set_ss(left), _mm_set_ss(right), _mm_set_ss(addend)));
#endif
    return std::fma(left, right, addend);
}

float fastFmCarrierSin(float angle, bool nativeFma = false) noexcept
{
    const auto foldedAngle = foldFmCarrierAngle(angle);

    const auto squared = foldedAngle * foldedAngle;
    const auto fourth = squared * squared;
    const auto low = fusedMultiplyAdd(-1.6666657100e-1f, squared, 1.0f, nativeFma);
    const auto middle = fusedMultiplyAdd(-1.9806615200e-4f,
                                 squared,
                                 8.3330172920e-3f, nativeFma);
    return foldedAngle * fusedMultiplyAdd(
        fourth,
        fusedMultiplyAdd(fourth, 2.5907305826e-6f, middle, nativeFma),
        low, nativeFma);
}

#if defined(_MSC_VER) && defined(_M_X64)
__forceinline __m128 fmCarrierPolynomialFour(__m128 angle) noexcept
{
    const auto squared = _mm_mul_ps(angle, angle);
    const auto fourth = _mm_mul_ps(squared, squared);
    const auto low = _mm_fmadd_ps(
        squared, _mm_set1_ps(-1.6666657100e-1f), _mm_set1_ps(1.0f));
    const auto middle = _mm_fmadd_ps(
        squared, _mm_set1_ps(-1.9806615200e-4f), _mm_set1_ps(8.3330172920e-3f));
    const auto inner = _mm_fmadd_ps(fourth, _mm_set1_ps(2.5907305826e-6f), middle);
    return _mm_mul_ps(angle, _mm_fmadd_ps(fourth, inner, low));
}
#endif

void fastFmCarrierSinEight(float* angles, [[maybe_unused]] bool nativeFmFmaAvailable) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    for (int offset = 0; offset < 8; offset += 4)
        _mm_store_ps(angles + offset, foldFmCarrierAnglesFour(_mm_load_ps(angles + offset)));
#else
    for (int index = 0; index < 8; ++index)
        angles[index] = foldFmCarrierAngle(angles[index]);
#endif

#if defined(__aarch64__)
    for (int offset = 0; offset < 8; offset += 4)
    {
        const auto angle = vld1q_f32(angles + offset);
        const auto squared = vmulq_f32(angle, angle);
        const auto fourth = vmulq_f32(squared, squared);
        const auto low = vfmaq_f32(
            vdupq_n_f32(1.0f), squared, vdupq_n_f32(-1.6666657100e-1f));
        const auto middle = vfmaq_f32(
            vdupq_n_f32(8.3330172920e-3f),
            squared,
            vdupq_n_f32(-1.9806615200e-4f));
        const auto inner = vfmaq_f32(
            middle, fourth, vdupq_n_f32(2.5907305826e-6f));
        const auto polynomial = vfmaq_f32(low, fourth, inner);
        vst1q_f32(angles + offset, vmulq_f32(angle, polynomial));
    }
#elif defined(_MSC_VER) && defined(_M_X64)
    if (nativeFmFmaAvailable)
    {
        for (int offset = 0; offset < 8; offset += 4)
        {
            const auto angle = _mm_load_ps(angles + offset);
            _mm_store_ps(angles + offset, fmCarrierPolynomialFour(angle));
        }
    }
    else
    {
        for (int index = 0; index < 8; ++index)
            angles[index] = fastFmCarrierSin(angles[index]);
    }
#elif defined(__SSE2__)
    for (int offset = 0; offset < 8; offset += 4)
    {
        const auto angle = _mm_load_ps(angles + offset);
        const auto squared = _mm_mul_ps(angle, angle);
        const auto fourth = _mm_mul_ps(squared, squared);
#if defined(__FMA__)
        const auto low = _mm_fmadd_ps(
            squared, _mm_set1_ps(-1.6666657100e-1f), _mm_set1_ps(1.0f));
        const auto middle = _mm_fmadd_ps(
            squared, _mm_set1_ps(-1.9806615200e-4f), _mm_set1_ps(8.3330172920e-3f));
        const auto inner = _mm_fmadd_ps(
            fourth, _mm_set1_ps(2.5907305826e-6f), middle);
        const auto polynomial = _mm_fmadd_ps(fourth, inner, low);
#else
        const auto low = _mm_add_ps(
            _mm_set1_ps(1.0f),
            _mm_mul_ps(squared, _mm_set1_ps(-1.6666657100e-1f)));
        const auto middle = _mm_add_ps(
            _mm_set1_ps(8.3330172920e-3f),
            _mm_mul_ps(squared, _mm_set1_ps(-1.9806615200e-4f)));
        const auto inner = _mm_add_ps(
            middle, _mm_mul_ps(fourth, _mm_set1_ps(2.5907305826e-6f)));
        const auto polynomial = _mm_add_ps(low, _mm_mul_ps(fourth, inner));
#endif
        _mm_store_ps(angles + offset, _mm_mul_ps(angle, polynomial));
    }
#else
    for (int index = 0; index < 8; ++index)
        angles[index] = fastFmCarrierSin(angles[index]);
#endif
}

// The modulator feeds back into itself, so a float sine approximation changes
// future samples. Evaluate the bounded float input in double precision and use
// the CRT whenever rounding to float could depend on the polynomial's error.
#if defined(_MSC_VER) && defined(_M_X64)
__forceinline __m128 fmModulatorSinFour(__m128 input) noexcept
{
    const auto magnitude = _mm_andnot_ps(_mm_set1_ps(-0.0f), input);
    // Both callers admit nearest MXCSR rounding before this fast path.
    // Other modes keep the original CRT without changing the caller's MXCSR.
    if (_mm_movemask_ps(_mm_cmple_ps(magnitude, _mm_set1_ps(16.0f))) != 15)
    {
        alignas(16) std::array<float, 4> values;
        _mm_store_ps(values.data(), input);
        for (auto& value : values)
            value = static_cast<float>(std::sin(static_cast<double>(value)));
        return _mm_load_ps(values.data());
    }
    const auto x = _mm256_cvtps_pd(magnitude);
    const auto halfWaves = _mm256_round_pd(
        _mm256_mul_pd(x, _mm256_set1_pd(0.31830988618379067154)),
        _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    const auto halfWaveBits = _mm256_cvttpd_epi32(halfWaves);
    auto reduced = _mm256_fnmadd_pd(halfWaves,
        _mm256_set1_pd(3.14159265358979311600), x);
    reduced = _mm256_fnmadd_pd(halfWaves,
        _mm256_set1_pd(1.22464679914735320717e-16), reduced);
    const auto squared = _mm256_mul_pd(reduced, reduced);
    // Odd degree-13 polynomial with a conservative certified error bound.
    // Uncertain float rounding still uses the original CRT; see
    // docs/audit/r11-fm-sine-bound-20261017.zh-CN.md and the exhaustive parity test.
    const auto errorBound = _mm256_set1_pd(0x1.6a3ddf2f9ede7p-43);
    auto sine = _mm256_set1_pd(0x1.52e486db16d9fp-33);
    sine = _mm256_fmadd_pd(sine, squared, _mm256_set1_pd(-0x1.ae043c0788bcep-26));
    sine = _mm256_fmadd_pd(sine, squared, _mm256_set1_pd(0x1.71dcf7dfe96f1p-19));
    sine = _mm256_fmadd_pd(sine, squared, _mm256_set1_pd(-0x1.a019fd47c9286p-13));
    sine = _mm256_fmadd_pd(sine, squared, _mm256_set1_pd(0x1.11111109eca55p-7));
    sine = _mm256_fmadd_pd(sine, squared, _mm256_set1_pd(-0x1.5555555543b6ep-3));
    sine = _mm256_fmadd_pd(sine, squared, _mm256_set1_pd(0x1.ffffffffffe3ap-1));
    const auto result = _mm256_mul_pd(sine, reduced);
    const auto absolute = _mm256_andnot_pd(_mm256_set1_pd(-0.0), result);
    // The bound includes enough margin for nearest binary64 endpoint rounding.
    // Identical endpoint float bits admit the same CRT-rounded result; otherwise
    // retain the existing per-lane CRT fallback.
    const auto lowBits = _mm_castps_si128(_mm256_cvtpd_ps(
        _mm256_sub_pd(absolute, errorBound)));
    const auto highBits = _mm_castps_si128(_mm256_cvtpd_ps(
        _mm256_add_pd(absolute, errorBound)));
    const auto equalBits = _mm_cmpeq_epi32(lowBits, highBits);
    const auto uncertain = (~_mm_movemask_ps(_mm_castsi128_ps(equalBits))) & 15;
    const auto sign = _mm_xor_ps(_mm_and_ps(input, _mm_set1_ps(-0.0f)),
        _mm_castsi128_ps(_mm_slli_epi32(_mm_and_si128(halfWaveBits, _mm_set1_epi32(1)), 31)));
    auto output = _mm_xor_ps(_mm256_cvtpd_ps(result), sign);
    // For |x| < 2^-12, sin(double(float(x))) rounds back to x.
    // Keep signed zero and subnormals without range-reduction arithmetic.
    const auto small = _mm_cmplt_ps(magnitude, _mm_set1_ps(0x1p-12f));
    output = _mm_or_ps(_mm_and_ps(small, input), _mm_andnot_ps(small, output));
    // Exact zero keeps its sign; retain the original CRT for every other
    // uncertain input, including subnormals under the callback's DAZ/FTZ mode.
    const auto zero = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(
        _mm_castps_si128(magnitude), _mm_setzero_si128())));
    const auto fallback = uncertain & ~zero;
    if (fallback != 0)
    {
        alignas(16) std::array<float, 4> original, values;
        _mm_store_ps(original.data(), input);
        _mm_store_ps(values.data(), output);
        for (int lane = 0; lane < 4; ++lane)
            if ((fallback & (1 << lane)) != 0)
                values[static_cast<size_t>(lane)] = static_cast<float>(std::sin(
                    static_cast<double>(original[static_cast<size_t>(lane)])));
        return _mm_load_ps(values.data());
    }
    return output;
}
#endif

void fmModulatorSinEight(float* angles, bool nativeFma) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    if (nativeFma && (_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_NEAREST)
    {
        for (int offset = 0; offset < 8; offset += 4)
            _mm_store_ps(angles + offset,
                fmModulatorSinFour(_mm_load_ps(angles + offset)));
        return;
    }
#endif
    for (int lane = 0; lane < 8; ++lane)
        angles[lane] = static_cast<float>(std::sin(static_cast<double>(angles[lane])));
}

float fastLog2Positive(float value, bool nativeFma) noexcept
{
    const auto bits = std::bit_cast<uint32_t>(value);
    const auto exponent = static_cast<int>((bits >> 23) & 0xffu) - 127;
    const auto mantissa = std::bit_cast<float>(
        (bits & 0x007fffffu) | 0x3f800000u);
    const auto t = 2.0f * mantissa - 3.0f;
    auto polynomial = 0.000112815253864001486f;
    polynomial = fusedMultiplyAdd(polynomial, t, -0.000392345952384166863f, nativeFma);
    polynomial = fusedMultiplyAdd(polynomial, t, 0.00117532700878362692f, nativeFma);
    polynomial = fusedMultiplyAdd(polynomial, t, -0.00441215962640813253f, nativeFma);
    polynomial = fusedMultiplyAdd(polynomial, t, 0.0178135030041370011f, nativeFma);
    polynomial = fusedMultiplyAdd(polynomial, t, -0.0801579696141916653f, nativeFma);
    polynomial = fusedMultiplyAdd(polynomial, t, 0.480898270083266632f, nativeFma);
    return static_cast<float>(exponent)
        + fusedMultiplyAdd(polynomial, t, 0.584962759104444618f, nativeFma);
}

float fastExp2Negative(float exponent, bool nativeFma) noexcept
{
    auto integer = static_cast<int>(exponent);
    if (static_cast<float>(integer) > exponent)
        --integer;
    if (integer < -126)
        return 0.0f;
    if (integer > 0)
        integer = 0;

    const auto t = 2.0f * (exponent - static_cast<float>(integer)) - 1.0f;
    auto polynomial = 5.91798143109537947e-05f;
    polynomial = fusedMultiplyAdd(polynomial, t, 0.000855248473053589974f, nativeFma);
    polynomial = fusedMultiplyAdd(polynomial, t, 0.00981173788368757196f, nativeFma);
    polynomial = fusedMultiplyAdd(polynomial, t, 0.0849309755062671451f, nativeFma);
    polynomial = fusedMultiplyAdd(polynomial, t, 0.490129077013731707f, nativeFma);
    polynomial = fusedMultiplyAdd(polynomial, t, 1.41421366908243451f, nativeFma);
    return polynomial
        * std::bit_cast<float>(static_cast<uint32_t>(integer + 127) << 23);
}

float fastWarpPow(float base, float exponent, bool nativeFma) noexcept
{
    if (base <= 0.0f)
        return 0.0f;
    if (base >= 1.0f)
        return 1.0f;
    if (base < std::numeric_limits<float>::min())
        return std::pow(base, exponent);
    return fastExp2Negative(exponent * fastLog2Positive(base, nativeFma), nativeFma);
}

FAD_OSC_LOCAL_INLINE void fastWarpPowEight(float* bases, const float* exponents, bool nativeFma) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    if (nativeFma)
    {
        for (int offset = 0; offset < 8; offset += 4)
        {
            const auto base = _mm_load_ps(bases + offset);
            const auto regular = _mm_and_ps(
                _mm_cmpge_ps(base, _mm_set1_ps(std::numeric_limits<float>::min())),
                _mm_cmplt_ps(base, _mm_set1_ps(1.0f)));
            if (_mm_movemask_ps(regular) != 15)
            {
                for (int lane = offset; lane < offset + 4; ++lane)
                    bases[lane] = fastWarpPow(bases[lane], exponents[lane], true);
                continue;
            }
            const auto bits = _mm_castps_si128(base);
            const auto exponent = _mm_sub_epi32(_mm_srli_epi32(bits, 23), _mm_set1_epi32(127));
            const auto mantissa = _mm_castsi128_ps(_mm_or_si128(
                _mm_and_si128(bits, _mm_set1_epi32(0x007fffff)), _mm_set1_epi32(0x3f800000)));
            const auto t = _mm_sub_ps(_mm_mul_ps(_mm_set1_ps(2.0f), mantissa), _mm_set1_ps(3.0f));
            auto polynomial = _mm_set1_ps(0.000112815253864001486f);
            polynomial = _mm_fmadd_ps(polynomial, t, _mm_set1_ps(-0.000392345952384166863f));
            polynomial = _mm_fmadd_ps(polynomial, t, _mm_set1_ps(0.00117532700878362692f));
            polynomial = _mm_fmadd_ps(polynomial, t, _mm_set1_ps(-0.00441215962640813253f));
            polynomial = _mm_fmadd_ps(polynomial, t, _mm_set1_ps(0.0178135030041370011f));
            polynomial = _mm_fmadd_ps(polynomial, t, _mm_set1_ps(-0.0801579696141916653f));
            polynomial = _mm_fmadd_ps(polynomial, t, _mm_set1_ps(0.480898270083266632f));
            const auto log = _mm_add_ps(_mm_cvtepi32_ps(exponent),
                _mm_fmadd_ps(polynomial, t, _mm_set1_ps(0.584962759104444618f)));
            auto scaled = _mm_mul_ps(_mm_load_ps(exponents + offset), log);
            auto integer = _mm_cvttps_epi32(scaled);
            integer = _mm_add_epi32(integer, _mm_castps_si128(
                _mm_cmpgt_ps(_mm_cvtepi32_ps(integer), scaled)));
            const auto underflow = _mm_cmplt_epi32(integer, _mm_set1_epi32(-126));
            integer = _mm_andnot_si128(_mm_or_si128(underflow,
                _mm_cmpgt_epi32(integer, _mm_setzero_si128())), integer);
            scaled = _mm_andnot_ps(_mm_castsi128_ps(underflow), scaled);
            const auto x = _mm_sub_ps(_mm_mul_ps(_mm_set1_ps(2.0f),
                _mm_sub_ps(scaled, _mm_cvtepi32_ps(integer))), _mm_set1_ps(1.0f));
            polynomial = _mm_set1_ps(5.91798143109537947e-05f);
            polynomial = _mm_fmadd_ps(polynomial, x, _mm_set1_ps(0.000855248473053589974f));
            polynomial = _mm_fmadd_ps(polynomial, x, _mm_set1_ps(0.00981173788368757196f));
            polynomial = _mm_fmadd_ps(polynomial, x, _mm_set1_ps(0.0849309755062671451f));
            polynomial = _mm_fmadd_ps(polynomial, x, _mm_set1_ps(0.490129077013731707f));
            polynomial = _mm_fmadd_ps(polynomial, x, _mm_set1_ps(1.41421366908243451f));
            const auto power = _mm_castsi128_ps(_mm_slli_epi32(
                _mm_add_epi32(integer, _mm_set1_epi32(127)), 23));
            _mm_store_ps(bases + offset, _mm_andnot_ps(_mm_castsi128_ps(underflow),
                _mm_mul_ps(polynomial, power)));
        }
        return;
    }
#endif
    for (int lane = 0; lane < 8; ++lane)
        bases[lane] = fastWarpPow(bases[lane], exponents[lane], nativeFma);
}

FAD_OSC_LOCAL_INLINE void fastTanhEight(float* values) noexcept
{
#if defined(__aarch64__)
    for (int offset = 0; offset < 8; offset += 4)
    {
        const auto x = vld1q_f32(values + offset);
        const auto squared = vmulq_f32(x, x);
        auto numerator = vfmaq_f32(vdupq_n_f32(378.0f), squared, vdupq_n_f32(1.0f));
        numerator = vfmaq_f32(vdupq_n_f32(17325.0f), squared, numerator);
        numerator = vfmaq_f32(vdupq_n_f32(135135.0f), squared, numerator);
        numerator = vmulq_f32(x, numerator);
        auto denominator = vfmaq_f32(
            vdupq_n_f32(3150.0f), squared, vdupq_n_f32(28.0f));
        denominator = vfmaq_f32(vdupq_n_f32(62370.0f), squared, denominator);
        denominator = vfmaq_f32(vdupq_n_f32(135135.0f), squared, denominator);
        vst1q_f32(values + offset, vdivq_f32(numerator, denominator));
    }
#elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
    for (int offset = 0; offset < 8; offset += 4)
    {
        const auto x = _mm_load_ps(values + offset);
        const auto squared = _mm_mul_ps(x, x);
        auto numerator = _mm_add_ps(
            _mm_set1_ps(378.0f), _mm_mul_ps(squared, _mm_set1_ps(1.0f)));
        numerator = _mm_add_ps(_mm_set1_ps(17325.0f), _mm_mul_ps(squared, numerator));
        numerator = _mm_add_ps(_mm_set1_ps(135135.0f), _mm_mul_ps(squared, numerator));
        numerator = _mm_mul_ps(x, numerator);
        auto denominator = _mm_add_ps(
            _mm_set1_ps(3150.0f), _mm_mul_ps(squared, _mm_set1_ps(28.0f)));
        denominator = _mm_add_ps(
            _mm_set1_ps(62370.0f), _mm_mul_ps(squared, denominator));
        denominator = _mm_add_ps(
            _mm_set1_ps(135135.0f), _mm_mul_ps(squared, denominator));
        _mm_store_ps(values + offset, _mm_div_ps(numerator, denominator));
    }
#else
    for (int index = 0; index < 8; ++index)
        values[index] = fad::engine::math::rationalTanh(values[index]);
#endif
}

FAD_OSC_LOCAL_INLINE float wrapSingleIncrement(float phase) noexcept
{
    if (phase >= 1.0f)
        phase -= 1.0f;
    else if (phase < 0.0f)
        phase += 1.0f;

    return phase >= 0.0f && phase < 1.0f ? phase : wrapPhase01(phase);
}

float polyBlep(float phase, float phaseDelta) noexcept
{
    if (phaseDelta <= 0.0f)
        return 0.0f;

    if (phase < phaseDelta)
    {
        const auto t = phase / phaseDelta;
        return t + t - t * t - 1.0f;
    }

    if (phase > 1.0f - phaseDelta)
    {
        const auto t = (phase - 1.0f) / phaseDelta;
        return t * t + t + t + 1.0f;
    }

    return 0.0f;
}

const wavetable::WavetableData& getBuiltinWavetable()
{
    static const auto builtin = wavetable::createBuiltinWavetable();
    return builtin;
}

FAD_OSC_LOCAL_INLINE float samplePreparedLevel(const float* frameA, const float* frameB,
    size_t frameSize, float frameFraction, float phase) noexcept
{
    const auto safePhase = fadmodel1::dsp::utils::sanitizeUnitInterval(phase);
    const auto index = safePhase * static_cast<float>(frameSize);
    const auto baseIndex = static_cast<size_t>(index);
    const auto indexMask = frameSize - 1u;
    const auto sampleA = baseIndex & indexMask;
    const auto sampleB = (sampleA + 1u) & indexMask;
    const auto sampleFraction = index - static_cast<float>(baseIndex);
    const auto valueA = frameA[sampleA]
        + sampleFraction * (frameA[sampleB] - frameA[sampleA]);
    const auto valueB = frameB[sampleA]
        + sampleFraction * (frameB[sampleB] - frameB[sampleA]);
    return valueA + frameFraction * (valueB - valueA);
}

FAD_OSC_LOCAL_INLINE float samplePreparedWavetable(
    const wavetable::WavetableData::PreparedBandLimitedSample& prepared,
    float phase) noexcept
{
    if (prepared.levelAFrameA == nullptr
        || prepared.levelAFrameB == nullptr
        || prepared.levelAFrameSize == 0)
    {
        return 0.0f;
    }


    const auto valueA = samplePreparedLevel(prepared.levelAFrameA,
                                    prepared.levelAFrameB,
                                    prepared.levelAFrameSize,
                                    prepared.frameFraction, phase);
    if (prepared.levelBFrameA == nullptr
        || prepared.levelBFrameB == nullptr
        || prepared.levelBFrameSize == 0
        || prepared.levelFraction < 0.001f)
    {
        return valueA;
    }

    const auto valueB = samplePreparedLevel(prepared.levelBFrameA,
                                    prepared.levelBFrameB,
                                    prepared.levelBFrameSize,
                                    prepared.frameFraction, phase);
    return valueA + prepared.levelFraction * (valueB - valueA);
}

float renderBandLimitedSaw(float phase, float phaseDelta) noexcept
{
    auto sample = 2.0f * phase - 1.0f;
    sample -= polyBlep(phase, phaseDelta);
    return sample;
}

float renderBandLimitedSquare(float phase,
                              float phaseDelta,
                              float pulseWidth) noexcept
{
    const auto width = std::clamp(pulseWidth, 0.01f, 0.99f);
    auto sample = phase < width ? 1.0f : -1.0f;
    sample += polyBlep(phase, phaseDelta);
    sample -= polyBlep(wrapSingleIncrement(phase - width), phaseDelta);
    return sample;
}

float triangleStateAtPhase(float phase, float phaseDelta, float leak) noexcept
{
    // Periodic solution of the leaky square integrator immediately BEFORE this
    // sample. Do not warm it up in the callback: work here is independent of
    // pitch. The (1 + pole) term accounts for the two edge samples of polyBLEP;
    // its fractional-edge residual is O(log(pole)^3), below float audio precision
    // at ordinary sample rates. Preserve the existing steady-state timbre/pole.
    const auto step = static_cast<double>(phaseDelta);
    const auto pole = static_cast<double>(leak);
    const auto previousPhase = wrapPhase01(phase - phaseDelta);
    const auto positive = previousPhase < 0.5f;
    const auto age = (static_cast<double>(previousPhase) - (positive ? 0.0 : 0.5)) / step;
    const auto halfPeriod = 0.5 / step;
    const auto logPole = std::log(pole);
    auto state = (4.0 * step / (1.0 - pole))
        * (1.0 - (1.0 + pole) * std::exp(logPole * age)
            / (1.0 + std::exp(logPole * halfPeriod)));
    // polyBLEP also starts the NEXT edge one sample before the discontinuity.
    const auto untilNextEdge = halfPeriod - age;
    if (untilNextEdge < 1.0)
        state -= 4.0 * step * (1.0 - untilNextEdge) * (1.0 - untilNextEdge);
    return static_cast<float>(positive ? state : -state);
}

float renderTriangleFromSquare(float& integrator,
                               float square,
                               float phaseDelta,
                               float leakCoeff) noexcept
{
    if (! utils::isFinite(integrator))
        integrator = 0.0f;
    if (! utils::isFinite(square) || ! utils::isFinite(phaseDelta))
        return 0.0f;

    const auto safeLeak = fadmodel1::dsp::utils::sanitizeFiniteClamped(leakCoeff,
                                                                       0.995f,
                                                                       0.0f,
                                                                       0.99999f);
    integrator = safeLeak * integrator + square * phaseDelta * 4.0f;
    fad::engine::math::snapSmallState(integrator);
    if (! utils::isFinite(integrator))
        integrator = 0.0f;
    return std::clamp(integrator, -1.0f, 1.0f);
}
#undef FAD_OSC_LOCAL_INLINE
} // namespace

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
void fmDecimationForTest(float* previous, const float* accumulated, int factor, bool batch) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    if (batch) decimateFmEight(previous, accumulated, factor);
    else
#endif
        for (int lane = 0; lane < 8; ++lane)
            previous[lane] += 0.55f * (accumulated[lane] / static_cast<float>(factor) - previous[lane]);
    for (int lane = 0; lane < 8; ++lane)
    {
        fad::engine::math::snapSmallState(previous[lane]);
        if (!utils::isFinite(previous[lane])) previous[lane] = 0.0f;
    }
}

void fmModulatorSinForTest(float* angles, bool useBatch) noexcept
{
    alignas(16) std::array<float, 8> aligned;
    std::copy_n(angles, 8, aligned.begin());
    fmModulatorSinEight(aligned.data(), useBatch);
    std::copy(aligned.begin(), aligned.end(), angles);
}

void warpPowForTest(float* bases, const float* exponents, bool useBatch, bool nativeFma) noexcept
{
    if (useBatch) fastWarpPowEight(bases, exponents, nativeFma);
    else for (int lane = 0; lane < 8; ++lane) bases[lane] = fastWarpPow(bases[lane], exponents[lane], nativeFma);
}

void fmCarrierFoldForTest(float* angles, [[maybe_unused]] bool useBatch) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    if (useBatch)
    {
        _mm_storeu_ps(angles, foldFmCarrierAnglesFour(_mm_loadu_ps(angles)));
        return;
    }
#endif
    for (int index = 0; index < 4; ++index)
        angles[index] = foldFmCarrierAngle(angles[index]);
}
#endif

void OscillatorBlock::prepare(double newSampleRate)
{
#if defined(_MSC_VER) && defined(_M_X64) && defined(PF_AVX_INSTRUCTIONS_AVAILABLE)
    // CPUID alone does not establish OS AVX context support. Query only during
    // preparation; the audio path reads this flag and retains its scalar fallback.
    int featureWords[4]{};
    __cpuid(featureWords, 1);
    nativeFmFmaAvailable = (featureWords[2] & (1 << 12)) != 0
        && ::IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE) != 0;
#endif
    sampleRate = fadmodel1::dsp::utils::sanitizeSampleRate(newSampleRate);
    inverseSampleRate = 1.0f / static_cast<float>(sampleRate);
    fmOversampleFactor = std::clamp(static_cast<int>(std::ceil(minimumFmOversampleRate / sampleRate)), 1, maxOversampleFactor);
    warpOversampleFactor = std::clamp(static_cast<int>(std::ceil(minimumWarpOversampleRate / sampleRate)), 1, maxOversampleFactor);
    warpOversampleMipOffset = std::log2(static_cast<float>(warpOversampleFactor));
    triangleLeakCoeff = static_cast<float>(
        std::exp(-2.0 * std::numbers::pi_v<double> * 20.0 / sampleRate));
    wavetableFadeSamples = std::clamp(fad::engine::math::roundToNearestInt(static_cast<float>(sampleRate) * 0.003f), 16, 256);
    [[maybe_unused]] const auto& builtinWavetable = getBuiltinWavetable();

    reset();
}

void OscillatorBlock::reset() noexcept
{
    phase = 0.0f;
    phaseDelta = 0.0f;
    currentFrequencyHz = 0.0f;
    modulatorPhase = 0.0f;
    modulatorPhaseDelta = 0.0f;
    preparedFmPhaseDelta = std::numeric_limits<float>::quiet_NaN();
    preparedFmRatio = std::numeric_limits<float>::quiet_NaN();
    lastFmModulatorSample = 0.0f;
    triangleIntegrator = 0.0f;
    triangleNeedsAlignment = true;
    fmOversamplePrevOut = 0.0f;
    phaseJitter = 0.0f;
    wavetableFadeRemaining = 0;
    wavetableFadeStartSample = 0.0f;
    lastWavetableSample = 0.0f;
    hasRenderedWavetableSample = false;
    activeMode = parameters::OscillatorMode::analog;
    activeAnalogWave = parameters::AnalogWave::sine;
    modeFadeRemaining = 0;
    modeFadeStartSample = 0.0f;
    lastOutputSample = 0.0f;
    hasRenderedSample = false;
    wavetableMipLevel = std::numeric_limits<float>::quiet_NaN();
    preparedWarp = std::numeric_limits<float>::quiet_NaN();
    invalidateWavetableSamplePlan();
}

void OscillatorBlock::resetRandomState() noexcept
{
    jitterState = initialJitterState;
    phaseJitter = 0.0f;
}

void OscillatorBlock::setPhase(float newPhase01, bool resetPhaseHistory) noexcept
{
    phase = wrapPhase01(utils::isFinite(newPhase01) ? newPhase01 : 0.0f);
    modulatorPhase = phase;
    // Every phase set starts a new onset. Never cross-fade from the previous
    // note's last sample when mode/wave/table changed; that sample belongs to
    // unrelated audio. Native phase memory keeps only its waveform history.
    hasRenderedSample = false;
    lastOutputSample = 0.0f;
    modeFadeRemaining = 0;
    hasRenderedWavetableSample = false;
    lastWavetableSample = 0.0f;
    wavetableFadeRemaining = 0;
    if (resetPhaseHistory)
    {
        triangleNeedsAlignment = true;
        lastFmModulatorSample = 0.0f;
        fmOversamplePrevOut = 0.0f;
    }
}

void OscillatorBlock::setExternalWavetable(const wavetable::WavetableData* table,
                                           uint32_t generation) noexcept
{
    if (table == externalWavetable && generation == externalWavetableGeneration)
        return;

    // A generation change invalidates prepared frame pointers even when an
    // allocator publishes a new WavetableData at the same address.
    invalidateWavetableSamplePlan();

    if (hasRenderedWavetableSample)
    {
        wavetableFadeStartSample = lastWavetableSample;
        wavetableFadeRemaining = wavetableFadeSamples;
    }
    else
    {
        wavetableFadeRemaining = 0;
        wavetableFadeStartSample = 0.0f;
    }

    externalWavetable = (table != nullptr && table->hasRenderableLayout()) ? table : nullptr;
    externalWavetableGeneration = generation;
}

void OscillatorBlock::invalidateWavetableSamplePlan() noexcept
{
    preparedWavetable = nullptr;
    preparedWavetablePosition = std::numeric_limits<float>::quiet_NaN();
    preparedWavetableMipLevel = std::numeric_limits<float>::quiet_NaN();
    preparedWavetableSample = {};
    hasPreparedWavetableSample = false;
}

float OscillatorBlock::renderWavetableSample(const wavetable::WavetableData& table,
                                             float position,
                                             float phaseValue,
                                             float phaseDeltaValue,
                                             float idealMipLevel,
                                             float warp,
                                             parameters::WarpMode warpMode,
                                             int nativeWarpMode) noexcept
{
    // Switching the existing Warp Mode control away from its imported mode
    // takes precedence; the reserved metadata must not make that control inert.
    const bool nativeBend = nativeSyncWarp && isSerumBendMode(nativeWarpMode)
        && warpMode == (nativeWarpMode == 5 ? parameters::WarpMode::bendMinus
                                           : parameters::WarpMode::bendPlus);
    const bool nativeAsym = nativeSyncWarp && nativeWarpMode >= 8 && nativeWarpMode <= 10
        && warpMode == parameters::WarpMode::asym;
    const bool nativeRemap = nativeSyncWarp && nativeWarpMode == 14
        && warpMode == parameters::WarpMode::sync
        && table.phaseRemap.size() == wavetable::WavetableData::phaseRemapSize;
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
    const auto staticFastPathEnabled = staticWavetableFastPathEnabled;
#else
    constexpr auto staticFastPathEnabled = true;
#endif
    if (staticFastPathEnabled
        && !nativeBend
        && !nativeAsym
        && !nativeRemap
        && hasPreparedWavetableSample
        && preparedWavetable == &table
        && std::bit_cast<uint32_t>(preparedWavetablePosition)
            == std::bit_cast<uint32_t>(position)
        && std::bit_cast<uint32_t>(preparedWavetableMipLevel)
            == std::bit_cast<uint32_t>(idealMipLevel)
        && warp >= 0.0f
        && warp < 1.0e-4f)
    {
        const auto sample=samplePreparedWavetable(preparedWavetableSample, phaseValue);
        return nodeCurve && nodeCurve->mix>0 ? sample+nodeCurve->mix*(nodeCurve->sample(phaseValue,phaseDeltaValue)-sample) : sample;
    }

    const auto clampedPosition = std::clamp(fadmodel1::dsp::utils::sanitizeFinite(position), 0.0f, 1.0f);
    const auto clampedWarp = std::clamp(fadmodel1::dsp::utils::sanitizeFinite(warp), 0.0f, 1.0f);
    const auto usePrecomputedMipLevel = utils::isFinite(idealMipLevel);
    const auto oversampled = nativeBend || nativeAsym || nativeRemap || clampedWarp >= 1.0e-4f;
    const auto samplingPhaseDelta = oversampled
        ? phaseDeltaValue / static_cast<float>(warpOversampleFactor)
        : phaseDeltaValue;
    const auto effectiveMipLevel = usePrecomputedMipLevel
        ? idealMipLevel - (oversampled ? warpOversampleMipOffset : 0.0f)
        : wavetable::WavetableData::calculateIdealMipLevel(table.frameSize,
                                                           samplingPhaseDelta);

    const auto positionChanged = preparedWavetablePosition < clampedPosition
        || preparedWavetablePosition > clampedPosition;
    const auto mipLevelChanged = preparedWavetableMipLevel < effectiveMipLevel
        || preparedWavetableMipLevel > effectiveMipLevel;
    if (preparedWavetable != &table || positionChanged || mipLevelChanged)
    {
        preparedWavetable = &table;
        preparedWavetablePosition = clampedPosition;
        preparedWavetableMipLevel = effectiveMipLevel;
        hasPreparedWavetableSample = table.prepareBandLimitedSample(
            clampedPosition,
            effectiveMipLevel,
            preparedWavetableSample);
    }

    const auto samplePrepared = [&](float samplePhase) noexcept
    {
        const auto sample=hasPreparedWavetableSample
            ? samplePreparedWavetable(preparedWavetableSample, samplePhase)
            : 0.0f;
        return nodeCurve && nodeCurve->mix>0 ? sample+nodeCurve->mix*(nodeCurve->sample(samplePhase,samplingPhaseDelta)-sample) : sample;
    };

    if (nativeRemap)
    {
        float sum = 0.0f;
        for (int i = 0; i < warpOversampleFactor; ++i)
        {
            const auto local = wrapSingleIncrement(phaseValue + static_cast<float>(i)*samplingPhaseDelta);
            // Remap 2 mirrors the custom map on the second half-cycle;
            // simply repeating the same map twice changes its spectrum.
            const bool mirrored = local >= .5f;
            const auto mapPhase = mirrored ? 2.0f*(1.0f-local) : 2.0f*local;
            const auto coordinate = mapPhase*static_cast<float>(wavetable::WavetableData::phaseRemapSize-1);
            const auto index = std::min(static_cast<size_t>(coordinate), table.phaseRemap.size()-2);
            const auto fraction = coordinate-static_cast<float>(index);
            const auto remapped = table.phaseRemap[index]+fraction*(table.phaseRemap[index+1]-table.phaseRemap[index]);
            const auto dryPhase = local*2.0f;
            const auto wetPhase = mirrored ? 2.0f-remapped : remapped;
            const auto remappedPhase = dryPhase+clampedWarp*(wetPhase-dryPhase);
            sum += samplePrepared(remappedPhase-std::floor(remappedPhase));
        }
        return sum/static_cast<float>(warpOversampleFactor);
    }

    if (nativeBend)
    {
        float sum = 0.0f;
        for (int i = 0; i < warpOversampleFactor; ++i)
        {
            const auto local = wrapSingleIncrement(phaseValue + static_cast<float>(i) * samplingPhaseDelta);
            sum += samplePrepared(serumBendPhase(local, clampedWarp, nativeWarpMode));
        }
        return sum / static_cast<float>(warpOversampleFactor);
    }

    if (nativeAsym)
    {
        // Native API saw sweeps: + pivots .5 -> .1, - pivots .5 -> .9,
        // bipolar pivots .1 -> .9. Zero bipolar depth is not an Off state.
        const auto pivot = nativeWarpMode == 8 ? .5f - .4f * clampedWarp
            : nativeWarpMode == 9 ? .5f + .4f * clampedWarp : .1f + .8f * clampedWarp;
        const auto leftScale = .5f / pivot;
        const auto rightScale = .5f / (1.0f - pivot);
        float sum = 0.0f;
        for (int i = 0; i < warpOversampleFactor; ++i)
        {
            const auto local = wrapSingleIncrement(phaseValue + static_cast<float>(i) * samplingPhaseDelta);
            const auto mapped = local < pivot ? local * leftScale
                : .5f + (local - pivot) * rightScale;
            sum += samplePrepared(mapped);
        }
        return sum / static_cast<float>(warpOversampleFactor);
    }

    if (! oversampled)
        return samplePrepared(phaseValue);

    if (nativeSyncWarp && warpMode == parameters::WarpMode::sync)
    {
        // Native Sync ratios measured at raw 0/.25/.5/.7878788/1:
        // 1, 1.234375, 2.875, 8.336923, 16. No extra saturation stage.
        const auto ratio = 1.0f + 15.0f * clampedWarp * clampedWarp * clampedWarp;
        float sum = 0.0f;
        for (int i = 0; i < warpOversampleFactor; ++i)
        {
            const auto local = wrapSingleIncrement(phaseValue + static_cast<float>(i) * samplingPhaseDelta);
            sum += samplePrepared(wrapPhase01(local * ratio));
        }
        return sum / static_cast<float>(warpOversampleFactor);
    }

    if (! utils::isFinite(preparedWarp)
        || preparedWarp < clampedWarp
        || preparedWarp > clampedWarp)
    {
        preparedWarp = clampedWarp;
        preparedWarpGain = fad::engine::math::interpolate(clampedWarp, 1.0f, 2.2f);
        preparedWarpMakeup = 1.0f / std::sqrt(preparedWarpGain);
        preparedBendPlusExponent = 1.0f + clampedWarp * 3.0f;
        preparedBendMinusExponent = 1.0f / preparedBendPlusExponent;
    }

    if (warpMode == parameters::WarpMode::bendPlus
        || warpMode == parameters::WarpMode::bendMinus)
    {
        return renderPreparedBendWarpSample(
            phaseValue, phaseDeltaValue, warpMode);
    }

    float accumulated = 0.0f;
    for (int oversampleIndex = 0; oversampleIndex < warpOversampleFactor; ++oversampleIndex)
    {
        const auto localPhase = wrapSingleIncrement(
            phaseValue + static_cast<float>(oversampleIndex) * samplingPhaseDelta);
        const auto warpedPhase = applyWarpToPhase(localPhase, clampedWarp, warpMode);
        accumulated += fad::engine::math::rationalTanh(
            samplePrepared(warpedPhase) * preparedWarpGain) * preparedWarpMakeup;
    }

    return accumulated / static_cast<float>(warpOversampleFactor);
}

float OscillatorBlock::renderPreparedBendWarpSample(
    float phaseValue,
    float phaseDeltaValue,
    parameters::WarpMode warpMode) noexcept
{
    const auto samplingPhaseDelta =
        phaseDeltaValue / static_cast<float>(warpOversampleFactor);
    const auto bendExponent = warpMode == parameters::WarpMode::bendPlus
        ? preparedBendPlusExponent
        : preparedBendMinusExponent;
    float accumulated = 0.0f;
    for (int oversampleIndex = 0; oversampleIndex < warpOversampleFactor; ++oversampleIndex)
    {
        const auto localPhase = wrapSingleIncrement(
            phaseValue + static_cast<float>(oversampleIndex) * samplingPhaseDelta);
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
        const auto warpedPhase = fastWarpPowEnabled
            ? fastWarpPow(localPhase, bendExponent, nativeFmFmaAvailable)
            : std::pow(localPhase, bendExponent);
#else
        const auto warpedPhase = fastWarpPow(localPhase, bendExponent, nativeFmFmaAvailable);
#endif
        const auto preparedSample = hasPreparedWavetableSample
            ? samplePreparedWavetable(preparedWavetableSample, warpedPhase)
            : 0.0f;
        accumulated += fad::engine::math::rationalTanh(
            preparedSample * preparedWarpGain) * preparedWarpMakeup;
    }

    return accumulated / static_cast<float>(warpOversampleFactor);
}

std::array<float, 4> OscillatorBlock::previewCrossSource(
    const parameters::OscillatorSnapshot& snapshot) noexcept
{
    std::array<float, 4> samples{};
    if (!snapshot.enabled || snapshot.mode != parameters::OscillatorMode::wavetable) return samples;
    const auto& table = externalWavetable != nullptr ? *externalWavetable : getBuiltinWavetable();
    // A reciprocal cross route uses the unmodulated source, avoiding an algebraic loop.
    const auto warp = snapshot.crossMode == 0 ? snapshot.wavetableWarp : 0.0f;
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = renderWavetableSample(table, snapshot.wavetablePosition,
            wrapPhase01(phase + phaseDelta * (static_cast<float>(i) * 0.25f)),
            phaseDelta * 0.25f, wavetableMipLevel, warp, snapshot.warpMode,
            snapshot.crossMode == 0 ? snapshot.nativeWarpMode : -1);
    return samples;
}

float OscillatorBlock::renderCrossWavetable(const wavetable::WavetableData& table,
                                            const parameters::OscillatorSnapshot& snapshot) noexcept
{
    const auto amount = std::clamp(fadmodel1::dsp::utils::sanitizeFinite(snapshot.wavetableWarp), 0.0f, 1.0f);
    if (amount == 0.0f)
        return renderWavetableSample(table, snapshot.wavetablePosition, phase,
            phaseDelta, wavetableMipLevel, 0.0f, snapshot.warpMode);
    const auto amountSquared = amount * amount;
    const auto phaseDepth = 8.0f * amountSquared * amountSquared;
    float sum = 0.0f;
    for (size_t i = 0; i < crossSourceSamples.size(); ++i)
    {
        const auto source = fadmodel1::dsp::utils::sanitizeFiniteClamped(crossSourceSamples[i], 0.0f, -8.0f, 8.0f);
        auto samplePhase = phase + phaseDelta * (static_cast<float>(i) * 0.25f);
        // Native FM uses a unipolar source offset, including the depth-dependent
        // carrier phase shift. Omitting +1 changes PWM/saw timbre even at fixed pitch.
        if (snapshot.crossMode == 1) samplePhase += phaseDepth * (1.0f + source);
        const auto carrier = renderWavetableSample(table, snapshot.wavetablePosition,
            wrapPhase01(samplePhase), phaseDelta * 0.25f,
            wavetableMipLevel - 2.0f, 0.0f, snapshot.warpMode);
        const auto gain = snapshot.crossMode == 2 ? 1.0f + 2.0f * amount * source
            : snapshot.crossMode == 3 ? 1.0f - amount + 2.0f * amount * source : 1.0f;
        sum += carrier * gain;
    }
    return sum * 0.25f;
}

float OscillatorBlock::renderStaticWavetableSample(
    const parameters::OscillatorSnapshot& snapshot) noexcept
{
    if (activeMode != parameters::OscillatorMode::wavetable
        || snapshot.mode != parameters::OscillatorMode::wavetable)
    {
        return renderSample(snapshot);
    }

    if (! utils::isFinite(phase))
        phase = 0.0f;
    else if (phase < 0.0f || phase >= 1.0f)
        phase = wrapPhase01(phase);

    if (! utils::isFinite(phaseDelta))
    {
        phaseDelta = 0.0f;
        currentFrequencyHz = -1.0f;
    }
    if (! utils::isFinite(phaseJitter))
        phaseJitter = 0.0f;

    const auto* table = externalWavetable != nullptr
        ? externalWavetable
        : &getBuiltinWavetable();
    const auto limitedPhaseDelta = std::max(0.0f, phaseDelta);
    const auto bendWarpMode = snapshot.warpMode == parameters::WarpMode::bendPlus
        || snapshot.warpMode == parameters::WarpMode::bendMinus;
    const auto preparedMipLevel = wavetableMipLevel - warpOversampleMipOffset;
    const auto usePreparedBendWarp = !(nodeCurve && nodeCurve->mix>0) && bendWarpMode
        && !(nativeSyncWarp && isSerumBendMode(snapshot.nativeWarpMode))
        && snapshot.wavetableWarp >= 1.0e-4f
        && hasPreparedWavetableSample
        && preparedWavetable == table
        && std::bit_cast<uint32_t>(preparedWavetablePosition)
            == std::bit_cast<uint32_t>(snapshot.wavetablePosition)
        && utils::isFinite(wavetableMipLevel)
        && std::bit_cast<uint32_t>(preparedWavetableMipLevel)
            == std::bit_cast<uint32_t>(preparedMipLevel)
        && std::bit_cast<uint32_t>(preparedWarp)
            == std::bit_cast<uint32_t>(snapshot.wavetableWarp);

    auto sample = snapshot.crossMode > 0
        ? renderCrossWavetable(*table, snapshot)
        : usePreparedBendWarp
        ? renderPreparedBendWarpSample(
              phase, limitedPhaseDelta, snapshot.warpMode)
        : renderWavetableSample(
              *table,
              snapshot.wavetablePosition,
              phase,
              limitedPhaseDelta,
              wavetableMipLevel,
              snapshot.wavetableWarp,
              snapshot.warpMode,
              snapshot.nativeWarpMode);


    if (wavetableFadeRemaining > 0)
    {
        const auto fadeOut = static_cast<float>(wavetableFadeRemaining)
            / static_cast<float>(wavetableFadeSamples);
        sample = wavetableFadeStartSample * fadeOut + sample * (1.0f - fadeOut);
        --wavetableFadeRemaining;
    }
    lastWavetableSample = sample;
    hasRenderedWavetableSample = true;

    auto output = utils::isFinite(sample) ? sample : 0.0f;
    if (modeFadeRemaining > 0)
    {
        const auto fadeOut = static_cast<float>(modeFadeRemaining)
            / static_cast<float>(wavetableFadeSamples);
        output = modeFadeStartSample * fadeOut + output * (1.0f - fadeOut);
        --modeFadeRemaining;
        output = utils::isFinite(output) ? output : 0.0f;
    }

    lastOutputSample = output;
    hasRenderedSample = true;
    phase = phase + phaseDelta + phaseJitter;
    if (phase >= 1.0f)
        phase -= 1.0f;
    else if (phase < 0.0f)
        phase += 1.0f;
    return output;
}

void OscillatorBlock::StaticUnisonRenderSpan::advancePhaseJitter() noexcept
{
    if (!fmActive)
    {
        for (int lane = 0; lane < count; ++lane)
            (lane == 0 ? primary : extras[lane - 1]).advancePhaseJitter();
        return;
    }
#if defined(_MSC_VER) && defined(_M_X64)
    for (int offset = 0; offset < 8; offset += 4)
    {
        auto state = _mm_load_si128(reinterpret_cast<const __m128i*>(fmState.jitterStates.data() + offset));
        state = _mm_xor_si128(state, _mm_slli_epi32(state, 13));
        state = _mm_xor_si128(state, _mm_srli_epi32(state, 17));
        state = _mm_xor_si128(state, _mm_slli_epi32(state, 5));
        _mm_store_si128(reinterpret_cast<__m128i*>(fmState.jitterStates.data() + offset), state);
        _mm_store_ps(fmState.jitter.data() + offset, _mm_mul_ps(_mm_mul_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(state), _mm_set1_ps(1.0f / 2147483648.0f)),
            _mm_load_ps(fmState.phaseDeltas.data() + offset)), _mm_set1_ps(0.00005f)));
    }
#else
    for (int lane = 0; lane < count; ++lane)
    {
        auto& state = fmState.jitterStates[static_cast<size_t>(lane)];
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        fmState.jitter[static_cast<size_t>(lane)] = static_cast<float>(static_cast<int32_t>(state))
            * (1.0f / 2147483648.0f) * fmState.phaseDeltas[static_cast<size_t>(lane)] * 0.00005f;
    }
#endif
}

void OscillatorBlock::StaticUnisonRenderSpan::flush() noexcept
{
    if (fmActive)
        commitFmUnison(primary, extras, fmState);
    fmActive = false;
    admitted = false;
}

bool OscillatorBlock::StaticUnisonRenderSpan::render(float* outputSamples) noexcept
{
    if (outputSamples == nullptr) return false;
    if (snapshot.mode == parameters::OscillatorMode::fm2Op)
    {
        if (!fmActive && !prepareStaticFmUnison(primary, extras, count, snapshot, fmState))
            return false;
        fmActive = true;
        renderPreparedFmUnison(fmState, outputSamples);
        return true;
    }
    if (snapshot.mode != parameters::OscillatorMode::wavetable) return false;
    const bool rendered = renderStaticWavetableUnisonImpl(primary, extras, count, snapshot, outputSamples, !admitted);
    admitted = rendered && (admitted || (utils::isFinite(snapshot.wavetablePosition)
        && utils::isFinite(snapshot.wavetableWarp)));
    return rendered;
}

bool OscillatorBlock::renderStaticWavetableUnison(
    OscillatorBlock& primary,
    OscillatorBlock* extraOscillators,
    int voiceCount,
    const parameters::OscillatorSnapshot& snapshot,
    float* outputSamples) noexcept
{
    return renderStaticWavetableUnisonImpl(primary, extraOscillators, voiceCount, snapshot, outputSamples, true);
}

bool OscillatorBlock::renderStaticWavetableUnisonImpl(
    OscillatorBlock& primary,
    OscillatorBlock* extraOscillators,
    int voiceCount,
    const parameters::OscillatorSnapshot& snapshot,
    float* outputSamples, bool checkAdmission) noexcept
{
    if (extraOscillators == nullptr
        || outputSamples == nullptr
        || voiceCount < 2
        || voiceCount > parameters::maxUnisonVoiceCount
        || snapshot.mode != parameters::OscillatorMode::wavetable
        || snapshot.crossMode != 0
        || (primary.nativeSyncWarp && isSerumBendMode(snapshot.nativeWarpMode))
        || (snapshot.warpMode != parameters::WarpMode::bendPlus
            && snapshot.warpMode != parameters::WarpMode::bendMinus)
        || snapshot.wavetableWarp < 1.0e-4f)
    {
        return false;
    }

    if (checkAdmission)
    {
        if(primary.nodeCurve && primary.nodeCurve->mix>0)return false;
        const auto canBatch = [&](const OscillatorBlock& oscillator) noexcept
        {
            const auto* table = oscillator.externalWavetable != nullptr
                ? oscillator.externalWavetable
                : &getBuiltinWavetable();
            const auto preparedMipLevel = oscillator.wavetableMipLevel
                - oscillator.warpOversampleMipOffset;
            return oscillator.warpOversampleFactor >= 1
                && oscillator.warpOversampleFactor == primary.warpOversampleFactor
                && oscillator.activeMode == parameters::OscillatorMode::wavetable
                && oscillator.modeFadeRemaining == 0
                && oscillator.wavetableFadeRemaining == 0
                && utils::isFinite(oscillator.phase)
                && oscillator.phase >= 0.0f
                && oscillator.phase < 1.0f
                && utils::isFinite(oscillator.phaseDelta)
                && utils::isFinite(oscillator.phaseJitter)
                && oscillator.hasPreparedWavetableSample
                && oscillator.preparedWavetable == table
                && std::bit_cast<uint32_t>(oscillator.preparedWavetablePosition)
                    == std::bit_cast<uint32_t>(snapshot.wavetablePosition)
                && utils::isFinite(oscillator.wavetableMipLevel)
                && std::bit_cast<uint32_t>(oscillator.preparedWavetableMipLevel)
                    == std::bit_cast<uint32_t>(preparedMipLevel)
                && std::bit_cast<uint32_t>(oscillator.preparedWarp)
                    == std::bit_cast<uint32_t>(snapshot.wavetableWarp);
        };

        if (! canBatch(primary))
            return false;
        for (int voiceIndex = 1; voiceIndex < voiceCount; ++voiceIndex)
            if (! canBatch(extraOscillators[voiceIndex - 1]))
                return false;

    }

    alignas(16) std::array<float, 8> shapedSamples{};
    alignas(16) std::array<float, 8> phases{};
    alignas(16) std::array<float, 8> exponents{};
    std::array<float, 8> accumulated{};
    exponents.fill(1.0f);
    for (int voiceIndex = 0; voiceIndex < voiceCount; ++voiceIndex)
    {
        auto& oscillator = voiceIndex == 0
            ? primary
            : extraOscillators[voiceIndex - 1];
        exponents[static_cast<size_t>(voiceIndex)] = snapshot.warpMode == parameters::WarpMode::bendPlus
            ? oscillator.preparedBendPlusExponent
            : oscillator.preparedBendMinusExponent;
    }
    for (int oversample = 0; oversample < primary.warpOversampleFactor; ++oversample)
    {
        phases.fill(0.5f);
        for (int voiceIndex = 0; voiceIndex < voiceCount; ++voiceIndex)
        {
            auto& oscillator = voiceIndex == 0 ? primary : extraOscillators[voiceIndex - 1];
            const auto step = std::max(0.0f, oscillator.phaseDelta)
                / static_cast<float>(oscillator.warpOversampleFactor);
            phases[static_cast<size_t>(voiceIndex)] = wrapSingleIncrement(
                oscillator.phase + static_cast<float>(oversample) * step);
        }
        fastWarpPowEight(phases.data(), exponents.data(), primary.nativeFmFmaAvailable);
        for (int voiceIndex = 0; voiceIndex < voiceCount; ++voiceIndex)
        {
            auto& oscillator = voiceIndex == 0 ? primary : extraOscillators[voiceIndex - 1];
            shapedSamples[static_cast<size_t>(voiceIndex)] = samplePreparedWavetable(
                oscillator.preparedWavetableSample, phases[static_cast<size_t>(voiceIndex)])
                * oscillator.preparedWarpGain;
        }
        fastTanhEight(shapedSamples.data());
        for (int voiceIndex = 0; voiceIndex < voiceCount; ++voiceIndex)
        {
            auto& oscillator = voiceIndex == 0 ? primary : extraOscillators[voiceIndex - 1];
            accumulated[static_cast<size_t>(voiceIndex)] += shapedSamples[static_cast<size_t>(voiceIndex)]
                * oscillator.preparedWarpMakeup;
        }
    }
    for (int voiceIndex = 0; voiceIndex < voiceCount; ++voiceIndex)
    {
        auto& oscillator = voiceIndex == 0
            ? primary
            : extraOscillators[voiceIndex - 1];
        auto output = accumulated[static_cast<size_t>(voiceIndex)]
            / static_cast<float>(oscillator.warpOversampleFactor);
        output = utils::isFinite(output) ? output : 0.0f;
        oscillator.lastWavetableSample = output;
        oscillator.hasRenderedWavetableSample = true;
        oscillator.lastOutputSample = output;
        oscillator.hasRenderedSample = true;
        oscillator.phase = oscillator.phase
            + oscillator.phaseDelta
            + oscillator.phaseJitter;
        if (oscillator.phase >= 1.0f)
            oscillator.phase -= 1.0f;
        else if (oscillator.phase < 0.0f)
            oscillator.phase += 1.0f;
        outputSamples[voiceIndex] = output;
    }
    return true;
}

bool OscillatorBlock::renderStaticFmUnison(
    OscillatorBlock& primary, OscillatorBlock* extras, int count,
    const parameters::OscillatorSnapshot& snapshot, float* outputSamples) noexcept
{
    if (outputSamples == nullptr) return false;
    FmUnisonState state;
    if (!prepareStaticFmUnison(primary, extras, count, snapshot, state)) return false;
    renderPreparedFmUnison(state, outputSamples);
    commitFmUnison(primary, extras, state);
    return true;
}

bool OscillatorBlock::prepareStaticFmUnison(
    OscillatorBlock& primary, OscillatorBlock* extraOscillators, int voiceCount,
    const parameters::OscillatorSnapshot& snapshot, FmUnisonState& state) noexcept
{
    if (extraOscillators == nullptr || voiceCount < 2
        || voiceCount > parameters::maxUnisonVoiceCount
        || snapshot.mode != parameters::OscillatorMode::fm2Op
        || !utils::isFinite(snapshot.fmRatio) || !utils::isFinite(snapshot.fmIndex)
        || !utils::isFinite(snapshot.fmFeedback) || !utils::isFinite(snapshot.fmLevel))
        return false;

    const auto ratio = std::clamp(snapshot.fmRatio, 1.f/128.f, 128.f);
    const auto fmIndex = std::clamp(snapshot.fmIndex, 0.0f, 100.0f);
    const auto fmFeedback = std::clamp(snapshot.fmFeedback, 0.0f, 1.0f);
    const auto fmLevel = std::clamp(snapshot.fmLevel, 0.0f, 1.0f);
    if (fmIndex <= 0.0f && fmFeedback <= 0.0f)
        return false;

    {
        const auto canBatch = [&](const OscillatorBlock& oscillator) noexcept
        {
    #if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
            if (! oscillator.fastFmCarrierEnabled)
                return false;
    #endif
            return oscillator.activeMode == parameters::OscillatorMode::fm2Op
                && oscillator.modeFadeRemaining == 0
                && oscillator.fmOversampleFactor > 0
                && utils::isFinite(oscillator.phase)
                && oscillator.phase >= 0.0f
                && oscillator.phase < 1.0f
                && utils::isFinite(oscillator.phaseDelta)
                && oscillator.phaseDelta >= 0.0f
                && utils::isFinite(oscillator.phaseJitter)
                && oscillator.phaseJitter >= -oscillator.phaseDelta
                && utils::isFinite(oscillator.modulatorPhase)
                && oscillator.modulatorPhase >= 0.0f
                && oscillator.modulatorPhase < 1.0f
                && utils::isFinite(oscillator.modulatorPhaseDelta)
                && oscillator.modulatorPhaseDelta >= 0.0f
                && utils::isFinite(oscillator.lastFmModulatorSample)
                && utils::isFinite(oscillator.fmOversamplePrevOut)
                && std::bit_cast<uint32_t>(oscillator.preparedFmPhaseDelta)
                    == std::bit_cast<uint32_t>(std::max(0.0f, oscillator.phaseDelta))
                && std::bit_cast<uint32_t>(oscillator.preparedFmRatio)
                    == std::bit_cast<uint32_t>(ratio);
        };

        if (! canBatch(primary))
            return false;
        for (int voiceIndex = 1; voiceIndex < voiceCount; ++voiceIndex)
        {
            const auto& oscillator = extraOscillators[voiceIndex - 1];
            if (oscillator.fmOversampleFactor != primary.fmOversampleFactor
                || ! canBatch(oscillator))
            {
                return false;
            }
        }

    }

    // Zero unused SIMD lanes as well as live state before the first render.
    state = {};
    state.factor = primary.fmOversampleFactor;
    state.count = voiceCount;
    state.index = fmIndex; state.feedback = fmFeedback; state.level = fmLevel;
    state.nativeFma = primary.nativeFmFmaAvailable;
    for (int voiceIndex = 0; voiceIndex < voiceCount; ++voiceIndex)
    {
        const auto& oscillator = voiceIndex == 0 ? primary : extraOscillators[voiceIndex - 1];
        const auto lane = static_cast<size_t>(voiceIndex);
        state.phases[lane] = oscillator.phase;
        state.modulatorPhases[lane] = oscillator.modulatorPhase;
        state.modulators[lane] = oscillator.lastFmModulatorSample;
        state.phaseDeltas[lane] = oscillator.phaseDelta;
        state.jitter[lane] = oscillator.phaseJitter;
        state.jitterStates[lane] = oscillator.jitterState;
        state.modulatorSubDeltas[lane] = oscillator.modulatorPhaseDelta;
        state.previousOutputs[lane] = oscillator.fmOversamplePrevOut;
    }
    return true;
}

#if defined(_MSC_VER) && defined(_M_X64)
__declspec(noinline)
#endif
void OscillatorBlock::renderPreparedFmUnison(FmUnisonState& state, float* outputSamples) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64)
    if (state.nativeFma && (_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_NEAREST)
    {
        alignas(16) std::array<float, 8> accumulated;
        const auto factor = _mm_set1_ps(static_cast<float>(state.factor));
        const auto twoPi = _mm_set1_ps((static_cast<float>(2) * std::numbers::pi_v<float>));
        const auto index = _mm_set1_ps(state.index);
        const auto feedbackAmount = _mm_set1_ps(state.feedback);
        const auto wrap = [](__m128 input) noexcept
        {
            const auto high = _mm_cmpge_ps(input, _mm_set1_ps(1.0f));
            return _mm_or_ps(_mm_and_ps(high, _mm_sub_ps(input, _mm_set1_ps(1.0f))),
                             _mm_andnot_ps(high, input));
        };
        // Interleave two independent groups while preserving each lane's
        // oversample order, arithmetic and feedback. No reduction order changes.
        auto phaseLow = _mm_load_ps(state.phases.data() + 0);
        auto modulatorPhaseLow = _mm_load_ps(state.modulatorPhases.data() + 0);
        auto modulatorLow = _mm_load_ps(state.modulators.data() + 0);
        const auto carrierDeltaLow = _mm_div_ps(_mm_add_ps(
            _mm_load_ps(state.phaseDeltas.data() + 0),
            _mm_load_ps(state.jitter.data() + 0)), factor);
        const auto modulatorDeltaLow = _mm_load_ps(state.modulatorSubDeltas.data() + 0);
        auto sumLow = _mm_setzero_ps();
        auto phaseHigh = _mm_load_ps(state.phases.data() + 4);
        auto modulatorPhaseHigh = _mm_load_ps(state.modulatorPhases.data() + 4);
        auto modulatorHigh = _mm_load_ps(state.modulators.data() + 4);
        const auto carrierDeltaHigh = _mm_div_ps(_mm_add_ps(
            _mm_load_ps(state.phaseDeltas.data() + 4),
            _mm_load_ps(state.jitter.data() + 4)), factor);
        const auto modulatorDeltaHigh = _mm_load_ps(state.modulatorSubDeltas.data() + 4);
        auto sumHigh = _mm_setzero_ps();
        for (int step = 0; step < state.factor; ++step)
        {
            const auto feedbackLow = _mm_mul_ps(_mm_mul_ps(twoPi, modulatorLow), feedbackAmount);
            const auto feedbackHigh = _mm_mul_ps(_mm_mul_ps(twoPi, modulatorHigh), feedbackAmount);
            modulatorLow = fmModulatorSinFour(
                _mm_add_ps(_mm_mul_ps(twoPi, modulatorPhaseLow), feedbackLow));
            modulatorHigh = fmModulatorSinFour(
                _mm_add_ps(_mm_mul_ps(twoPi, modulatorPhaseHigh), feedbackHigh));
            const auto carrierLow = _mm_add_ps(_mm_mul_ps(twoPi, phaseLow), _mm_mul_ps(modulatorLow, index));
            const auto carrierHigh = _mm_add_ps(_mm_mul_ps(twoPi, phaseHigh), _mm_mul_ps(modulatorHigh, index));
            const auto sampleLow = fmCarrierPolynomialFour(foldFmCarrierAnglesFour(carrierLow));
            const auto sampleHigh = fmCarrierPolynomialFour(foldFmCarrierAnglesFour(carrierHigh));
            phaseLow = wrap(_mm_add_ps(phaseLow, carrierDeltaLow));
            modulatorPhaseLow = wrap(_mm_add_ps(modulatorPhaseLow, modulatorDeltaLow));
            sumLow = _mm_add_ps(sumLow, sampleLow);
            phaseHigh = wrap(_mm_add_ps(phaseHigh, carrierDeltaHigh));
            modulatorPhaseHigh = wrap(_mm_add_ps(modulatorPhaseHigh, modulatorDeltaHigh));
            sumHigh = _mm_add_ps(sumHigh, sampleHigh);
        }
        _mm_store_ps(state.phases.data() + 0, phaseLow);
        _mm_store_ps(state.modulatorPhases.data() + 0, modulatorPhaseLow);
        _mm_store_ps(state.modulators.data() + 0, modulatorLow);
        _mm_store_ps(accumulated.data() + 0, sumLow);
        _mm_store_ps(state.phases.data() + 4, phaseHigh);
        _mm_store_ps(state.modulatorPhases.data() + 4, modulatorPhaseHigh);
        _mm_store_ps(state.modulators.data() + 4, modulatorHigh);
        _mm_store_ps(accumulated.data() + 4, sumHigh);
        decimateFmEight(state.previousOutputs.data(), accumulated.data(), state.factor);
        for (int lane = 0; lane < state.count; ++lane)
        {
            auto& previous = state.previousOutputs[static_cast<size_t>(lane)];
            fad::engine::math::snapSmallState(previous);
            if (!utils::isFinite(previous)) previous = 0.0f;
            outputSamples[lane] = previous * state.level;
        }
        return;
    }
#endif
    alignas(16) std::array<float, 8> accumulated{}, carriers{}, carrierSubDeltas{};
    auto& phases = state.phases;
    auto& modulatorPhases = state.modulatorPhases;
    auto& modulators = state.modulators;
    const auto& modulatorSubDeltas = state.modulatorSubDeltas;
    const auto fmFeedback = state.feedback;
    const auto fmIndex = state.index;
#if defined(_MSC_VER) && defined(_M_X64)
    for (int offset = 0; offset < 8; offset += 4)
        _mm_store_ps(carrierSubDeltas.data() + offset, _mm_div_ps(_mm_add_ps(
            _mm_load_ps(state.phaseDeltas.data() + offset), _mm_load_ps(state.jitter.data() + offset)),
            _mm_set1_ps(static_cast<float>(state.factor))));
#else
    for (int lane = 0; lane < state.count; ++lane)
        carrierSubDeltas[static_cast<size_t>(lane)] =
            (state.phaseDeltas[static_cast<size_t>(lane)] + state.jitter[static_cast<size_t>(lane)])
            / static_cast<float>(state.factor);
#endif
    for (int oversampleIndex = 0; oversampleIndex < state.factor; ++oversampleIndex)
    {
#if defined(_MSC_VER) && defined(_M_X64)
        for (int offset = 0; offset < 8; offset += 4)
        {
            const auto twoPi = _mm_set1_ps((static_cast<float>(2) * std::numbers::pi_v<float>));
            const auto feedback = _mm_mul_ps(_mm_mul_ps(twoPi, _mm_load_ps(modulators.data() + offset)),
                _mm_set1_ps(fmFeedback));
            _mm_store_ps(modulators.data() + offset,
                _mm_add_ps(_mm_mul_ps(twoPi, _mm_load_ps(modulatorPhases.data() + offset)), feedback));
        }
#else
        for (int lane = 0; lane < 8; ++lane)
        {
            const auto feedback = (static_cast<float>(2) * std::numbers::pi_v<float>) * modulators[lane] * fmFeedback;
            modulators[lane] = (static_cast<float>(2) * std::numbers::pi_v<float>) * modulatorPhases[lane] + feedback;
        }
#endif
        fmModulatorSinEight(modulators.data(), state.nativeFma);
#if defined(_MSC_VER) && defined(_M_X64)
        for (int offset = 0; offset < 8; offset += 4)
            _mm_store_ps(carriers.data() + offset, _mm_add_ps(
                _mm_mul_ps(_mm_set1_ps((static_cast<float>(2) * std::numbers::pi_v<float>)), _mm_load_ps(phases.data() + offset)),
                _mm_mul_ps(_mm_load_ps(modulators.data() + offset), _mm_set1_ps(fmIndex))));
#else
        for (int lane = 0; lane < 8; ++lane)
            carriers[lane] = (static_cast<float>(2) * std::numbers::pi_v<float>) * phases[lane] + modulators[lane] * fmIndex;
#endif
        fastFmCarrierSinEight(carriers.data(), state.nativeFma);
#if defined(_MSC_VER) && defined(_M_X64)
        const auto wrap = [](__m128 input) noexcept
        {
            // Admission and the jitter generator guarantee non-negative steps.
            // Preserve selection (including signed zero) rather than subtracting
            // a zero mask on lanes that did not cross the upper boundary.
            const auto high = _mm_cmpge_ps(input, _mm_set1_ps(1.0f));
            return _mm_or_ps(_mm_and_ps(high, _mm_sub_ps(input, _mm_set1_ps(1.0f))),
                             _mm_andnot_ps(high, input));
        };
        for (int offset = 0; offset < 8; offset += 4)
        {
            _mm_store_ps(phases.data() + offset, wrap(_mm_add_ps(
                _mm_load_ps(phases.data() + offset), _mm_load_ps(carrierSubDeltas.data() + offset))));
            _mm_store_ps(modulatorPhases.data() + offset, wrap(_mm_add_ps(
                _mm_load_ps(modulatorPhases.data() + offset), _mm_load_ps(modulatorSubDeltas.data() + offset))));
            _mm_store_ps(accumulated.data() + offset, _mm_add_ps(
                _mm_load_ps(accumulated.data() + offset), _mm_load_ps(carriers.data() + offset)));
        }
#else
        for (int lane = 0; lane < 8; ++lane)
        {
            phases[lane] = wrapSingleIncrement(phases[lane] + carrierSubDeltas[lane]);
            modulatorPhases[lane] = wrapSingleIncrement(modulatorPhases[lane] + modulatorSubDeltas[lane]);
            accumulated[lane] += carriers[lane];
        }
#endif
    }

#if defined(_MSC_VER) && defined(_M_X64)
    decimateFmEight(state.previousOutputs.data(), accumulated.data(), state.factor);
#endif
    for (int lane = 0; lane < state.count; ++lane)
    {
        auto& previous = state.previousOutputs[static_cast<size_t>(lane)];
#if !defined(_MSC_VER) || !defined(_M_X64)
        const auto decimated = accumulated[static_cast<size_t>(lane)] / static_cast<float>(state.factor);
        previous += 0.55f * (decimated - previous);
#endif
        fad::engine::math::snapSmallState(previous);
        if (!utils::isFinite(previous)) previous = 0.0f;
        outputSamples[lane] = previous * state.level;
    }
}

void OscillatorBlock::commitFmUnison(OscillatorBlock& primary, OscillatorBlock* extras,
                                    const FmUnisonState& state) noexcept
{
    for (int voiceIndex = 0; voiceIndex < state.count; ++voiceIndex)
    {
        auto& oscillator = voiceIndex == 0 ? primary : extras[voiceIndex - 1];
        const auto lane = static_cast<size_t>(voiceIndex);
        oscillator.phase = state.phases[lane];
        oscillator.modulatorPhase = state.modulatorPhases[lane];
        oscillator.lastFmModulatorSample = state.modulators[lane];
        oscillator.fmOversamplePrevOut = state.previousOutputs[lane];
        oscillator.phaseJitter = state.jitter[lane];
        oscillator.jitterState = state.jitterStates[lane];
        oscillator.lastOutputSample = state.previousOutputs[lane] * state.level;
        oscillator.hasRenderedSample = true;
    }
}

float OscillatorBlock::renderSample(const parameters::OscillatorSnapshot& snapshot) noexcept
{
    if (! utils::isFinite(phase))
        phase = 0.0f;
    else if (phase < 0.0f || phase >= 1.0f)
        phase = wrapPhase01(phase);

    if (! utils::isFinite(phaseDelta))
    {
        phaseDelta = 0.0f;
        currentFrequencyHz = -1.0f;
    }
    if (! utils::isFinite(phaseJitter))
        phaseJitter = 0.0f;

    if (snapshot.mode != activeMode)
    {
        activeMode = snapshot.mode;
        if (activeMode == parameters::OscillatorMode::analog)
            activeAnalogWave = snapshot.analogWave;
        if (hasRenderedSample)
        {
            triangleIntegrator = 0.0f;
            triangleNeedsAlignment = true;
            modulatorPhase = phase;
            modulatorPhaseDelta = 0.0f;
            preparedFmPhaseDelta = std::numeric_limits<float>::quiet_NaN();
            preparedFmRatio = std::numeric_limits<float>::quiet_NaN();
            lastFmModulatorSample = 0.0f;
            fmOversamplePrevOut = 0.0f;
            modeFadeStartSample = lastOutputSample;
            modeFadeRemaining = wavetableFadeSamples;
        }

        if (activeMode != parameters::OscillatorMode::wavetable)
        {
            wavetableFadeRemaining = 0;
            wavetableFadeStartSample = 0.0f;
            lastWavetableSample = 0.0f;
            hasRenderedWavetableSample = false;
        }
    }
    else if (snapshot.mode == parameters::OscillatorMode::analog
             && snapshot.analogWave != activeAnalogWave)
    {
        activeAnalogWave = snapshot.analogWave;
        triangleIntegrator = 0.0f;
        triangleNeedsAlignment = true;
        if (hasRenderedSample)
        {
            modeFadeStartSample = lastOutputSample;
            modeFadeRemaining = wavetableFadeSamples;
        }
    }

    const auto limitedPhaseDelta = std::max(0.0f, phaseDelta);
    if (triangleNeedsAlignment && limitedPhaseDelta > 0.0f
        && snapshot.mode == parameters::OscillatorMode::analog
        && (snapshot.analogWave == parameters::AnalogWave::saw
            || snapshot.analogWave == parameters::AnalogWave::triangle))
    {
        triangleIntegrator = triangleStateAtPhase(phase, limitedPhaseDelta, triangleLeakCoeff);
        triangleNeedsAlignment = false;
    }
    float sample = 0.0f;
    const auto finishSample = [this](float value, bool advancePhase) noexcept
    {
        auto output = utils::isFinite(value) ? value : 0.0f;
        if (modeFadeRemaining > 0)
        {
            const auto fadeOut = static_cast<float>(modeFadeRemaining)
                / static_cast<float>(wavetableFadeSamples);
            output = modeFadeStartSample * fadeOut + output * (1.0f - fadeOut);
            --modeFadeRemaining;
            output = utils::isFinite(output) ? output : 0.0f;
        }

        lastOutputSample = output;
        hasRenderedSample = true;
        if (advancePhase)
        {
            phase = phase + phaseDelta + phaseJitter;
            if (phase >= 1.0f)
                phase -= 1.0f;
            else if (phase < 0.0f)
                phase += 1.0f;
        }
        return output;
    };

    switch (snapshot.mode)
    {
        case parameters::OscillatorMode::wavetable:
            return renderStaticWavetableSample(snapshot);

        case parameters::OscillatorMode::fm2Op:
        {
            if (! utils::isFinite(modulatorPhase) || ! utils::isFinite(modulatorPhaseDelta))
            {
                modulatorPhase = phase;
                modulatorPhaseDelta = 0.0f;
                preparedFmPhaseDelta = std::numeric_limits<float>::quiet_NaN();
                preparedFmRatio = std::numeric_limits<float>::quiet_NaN();
            }
            if (! utils::isFinite(lastFmModulatorSample))
                lastFmModulatorSample = 0.0f;
            if (! utils::isFinite(fmOversamplePrevOut))
                fmOversamplePrevOut = 0.0f;

            const auto ratio = std::clamp(snapshot.fmRatio, 1.f/128.f, 128.f);
            const auto fmIndex = std::clamp(snapshot.fmIndex, 0.0f, 100.0f);
            const auto fmFeedback = std::clamp(snapshot.fmFeedback, 0.0f, 1.0f);
            const auto fmLevel = std::clamp(snapshot.fmLevel, 0.0f, 1.0f);
            const auto carrierSubDelta =
                (limitedPhaseDelta + phaseJitter) / static_cast<float>(fmOversampleFactor);
            if (! utils::isFinite(preparedFmPhaseDelta)
                || ! utils::isFinite(preparedFmRatio)
                || preparedFmPhaseDelta < limitedPhaseDelta
                || preparedFmPhaseDelta > limitedPhaseDelta
                || preparedFmRatio < ratio
                || preparedFmRatio > ratio)
            {
                preparedFmPhaseDelta = limitedPhaseDelta;
                preparedFmRatio = ratio;
                modulatorPhaseDelta = std::clamp((limitedPhaseDelta / static_cast<float>(fmOversampleFactor)) * ratio, 0.0f, 0.49f);
            }

            float accumulated = 0.0f;
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
            const auto useNeutralFastPath = neutralFmFastPathEnabled
                && fmIndex <= 0.0f && fmFeedback <= 0.0f;
#else
            const auto useNeutralFastPath = fmIndex <= 0.0f && fmFeedback <= 0.0f;
#endif
            if (useNeutralFastPath)
            {
                for (int oversampleIndex = 0; oversampleIndex < fmOversampleFactor; ++oversampleIndex)
                {
                    if (oversampleIndex == fmOversampleFactor - 1)
                    {
                        lastFmModulatorSample = static_cast<float>(std::sin(static_cast<double>(
                            (static_cast<float>(2) * std::numbers::pi_v<float>) * modulatorPhase)));
                    }

                    accumulated += std::sin((static_cast<float>(2) * std::numbers::pi_v<float>) * phase);
                    phase = wrapSingleIncrement(phase + carrierSubDelta);
                    modulatorPhase = wrapSingleIncrement(modulatorPhase + modulatorPhaseDelta);
                }
            }
            else
            {
                for (int oversampleIndex = 0; oversampleIndex < fmOversampleFactor; ++oversampleIndex)
                {
                    const auto feedbackPhase =
                        (static_cast<float>(2) * std::numbers::pi_v<float>)
                        * lastFmModulatorSample * fmFeedback;
                    // A one-ULP sinf difference is amplified by recursive FM feedback.
                    const auto modulator = static_cast<float>(std::sin(static_cast<double>(
                        (static_cast<float>(2) * std::numbers::pi_v<float>) * modulatorPhase
                        + feedbackPhase)));
                    const auto carrierAngle =
                        (static_cast<float>(2) * std::numbers::pi_v<float>) * phase + modulator * fmIndex;
#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
                    const auto carrier = fastFmCarrierEnabled
                        ? fastFmCarrierSin(carrierAngle, nativeFmFmaAvailable)
                        : std::sin(carrierAngle);
#else
                    const auto carrier = fastFmCarrierSin(carrierAngle, nativeFmFmaAvailable);
#endif
                    lastFmModulatorSample = modulator;

                    phase = wrapSingleIncrement(phase + carrierSubDelta);
                    modulatorPhase = wrapSingleIncrement(modulatorPhase + modulatorPhaseDelta);
                    accumulated += carrier;
                }
            }

            const float decimated = accumulated / static_cast<float>(fmOversampleFactor);
            fmOversamplePrevOut = fmOversamplePrevOut + 0.55f * (decimated - fmOversamplePrevOut);
            fad::engine::math::snapSmallState(fmOversamplePrevOut);
            if (! utils::isFinite(fmOversamplePrevOut))
                fmOversamplePrevOut = 0.0f;
            return finishSample(fmOversamplePrevOut * fmLevel, false);
        }

        case parameters::OscillatorMode::analog:
        default:
            switch (snapshot.analogWave)
            {
                case parameters::AnalogWave::saw:
                {
                    const auto saw = renderBandLimitedSaw(phase, limitedPhaseDelta);
                    const auto square = renderBandLimitedSquare(phase, limitedPhaseDelta, 0.5f);
                    const auto triangle = renderTriangleFromSquare(
                        triangleIntegrator, square, limitedPhaseDelta, triangleLeakCoeff);
                    sample = fad::engine::math::interpolate(std::clamp(snapshot.analogShape, 0.0f, 1.0f), triangle, saw);
                    break;
                }

                case parameters::AnalogWave::square:
                {
                    const auto pulseWidth = std::clamp(snapshot.analogPulseWidth, 0.01f, 0.99f);
                    sample = renderBandLimitedSquare(
                        phase,
                        limitedPhaseDelta,
                        fad::engine::math::interpolate(std::clamp(snapshot.analogShape, 0.0f, 1.0f), 0.5f, pulseWidth));
                    break;
                }

                case parameters::AnalogWave::triangle:
                {
                    const auto bandLimitedSquare =
                        renderBandLimitedSquare(phase, limitedPhaseDelta, 0.5f);
                    const auto triangle = renderTriangleFromSquare(
                        triangleIntegrator,
                        bandLimitedSquare,
                        limitedPhaseDelta,
                        triangleLeakCoeff);
                    sample = fad::engine::math::interpolate(std::clamp(snapshot.analogShape, 0.0f, 1.0f),
                                        std::sin((static_cast<float>(2) * std::numbers::pi_v<float>) * phase),
                                        triangle);
                    break;
                }

                case parameters::AnalogWave::sine:
                default:
                    sample = std::sin((static_cast<float>(2) * std::numbers::pi_v<float>) * phase);
                    break;
            }
            break;
    }

    return finishSample(sample, true);
}
} // namespace fadmodel1::dsp::oscillators
