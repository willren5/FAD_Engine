#pragma once
#include <cstdint>
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
 #include <xmmintrin.h>
#elif defined(__aarch64__) || defined(__arm64__)
 #include <arm_acle.h>
#else
 #error "FAD Engine denormal control requires a verified x86 SSE or AArch64 target"
#endif

namespace fad::engine::rt
{
// Thread-local floating-point control only. Preserve the caller's rounding,
// exception and unrelated control bits; nesting restores the incoming state.
// AArch64 FZ and x86 FTZ/DAZ are hardware-defined, not a DSP gain/clamp policy.
class ScopedNoDenormals final
{
public:
    ScopedNoDenormals() noexcept : saved_(readControl()) { writeControl(saved_ | flushMask); }
    ~ScopedNoDenormals() noexcept { writeControl(saved_); }
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

    static uint64_t readControl() noexcept
    {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        return _mm_getcsr();
#else
        return __arm_rsr64("fpcr");
#endif
    }
    static void writeControl(uint64_t control) noexcept
    {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        _mm_setcsr(static_cast<unsigned>(control));
#else
        __arm_wsr64("fpcr", control);
#endif
    }
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
    static constexpr uint64_t flushMask = (uint64_t{1} << 15) | (uint64_t{1} << 6);
#else
    static constexpr uint64_t flushMask = uint64_t{1} << 24;
#endif
private:
    uint64_t saved_;
};
}
