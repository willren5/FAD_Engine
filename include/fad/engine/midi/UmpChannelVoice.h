#pragma once
#include <array>
#include <cstdint>
#include <cmath>

namespace fadmodel1::dsp::midi
{
// M2-104-UM 1.1.2, section 7.4. Words stay in host byte order. Do not
// translate MIDI 2.0 expression to 7/14-bit MIDI before the render boundary.
struct UmpChannelVoice
{
    std::array<uint32_t, 2> words{};
    bool present() const noexcept { return (words[0] >> 28) == 4; }
    unsigned group() const noexcept { return (words[0] >> 24) & 15; }
    unsigned status() const noexcept { return (words[0] >> 20) & 15; }
    int channel() const noexcept { return static_cast<int>((words[0] >> 16) & 15) + 1; }
    int byte2() const noexcept { return static_cast<int>((words[0] >> 8) & 127); }
    int byte3() const noexcept { return static_cast<int>(words[0] & 255); }
    uint32_t data() const noexcept { return words[1]; }
    float unit() const noexcept { return static_cast<float>(static_cast<double>(data()) / 4294967295.0); }
    float velocity() const noexcept { return static_cast<float>(data() >> 16) / 65535.0f; }
    float bipolar() const noexcept
    { return static_cast<float>((static_cast<double>(data()) - 2147483648.0) / 2147483648.0); }
    bool valid() const noexcept { return present() && status() != 7; }
    bool noteOn() const noexcept { return present() && status() == 9; }
    static constexpr bool registeredPerNoteDefined(int index) noexcept
    { return index==1 || index==2 || index==3 || index==7 || index==8 || index==10 || index==11
        || (index>=70 && index<=79) || (index>=91 && index<=95); }
};
}
