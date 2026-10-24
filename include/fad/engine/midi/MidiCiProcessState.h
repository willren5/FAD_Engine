#pragma once
#include <array>
#include <cstdint>

namespace fadmodel1::midici
{
// Audio-owned values, copied only for a pending Process Inquiry. Raw MIDI 2.0
// values survive the report without the DSP float conversion losing precision.
struct ChannelState
{
    uint32_t pitch = 0x80000000u, pressure = 0;
};
using ProcessState = std::array<std::array<ChannelState, 16>, 16>;
}
