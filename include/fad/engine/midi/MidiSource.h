#pragma once
#include <cstddef>
#include <cstdint>

namespace fadmodel1::dsp::midi
{
// Transient performance ownership; never serialized as a parameter or preset ID.
enum class Source : uint8_t { host = 0, touch, audition, standalone0, standalone1, standalone2,
    standalone3, standalone4, standalone5, standalone6, standalone7,
    instrumentPitch = 255 }; // Control-only sentinel; never a note owner or persisted source.
inline constexpr size_t standaloneSourceCount = 3 + 8 * 16;
inline constexpr size_t sourceCount = standaloneSourceCount + 15;
static_assert(sourceCount < static_cast<size_t>(Source::instrumentPitch));
inline constexpr Source hostSource(unsigned group = 0) noexcept
{ return group == 0 ? Source::host : static_cast<Source>(standaloneSourceCount + group - 1); }
inline constexpr Source standaloneSource(unsigned port, unsigned group = 0) noexcept
{ return static_cast<Source>(group == 0 ? 3 + port : 11 + port * 15 + group - 1); }

}
