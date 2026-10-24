#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace fadmodel1::dsp::midi
{
// MIDI 1.0 channel-voice bytes only. SysEx, transport and CI stay with the host
// adapter. The render owner has no heap-backed message or parser state.
class ShortMessage
{
public:
    ShortMessage() noexcept = default;
    explicit ShortMessage(int status) noexcept : bytes_{static_cast<uint8_t>(status), 0, 0} {}
    ShortMessage(int status, int first, double timestamp = 0) noexcept
        : bytes_{static_cast<uint8_t>(status), static_cast<uint8_t>(first), 0}, timestamp_(timestamp) {}
    ShortMessage(int status, int first, int second, double timestamp = 0) noexcept
        : bytes_{static_cast<uint8_t>(status), static_cast<uint8_t>(first), static_cast<uint8_t>(second)}, timestamp_(timestamp) {}

    static std::optional<ShortMessage> decode(std::span<const uint8_t> bytes, double timestamp = 0) noexcept
    {
        if (bytes.size() < 2 || bytes.size() > 3 || bytes[0] < 0x80 || bytes[0] >= 0xf0)
            return std::nullopt;
        const auto kind = bytes[0] >> 4;
        if (bytes.size() != static_cast<size_t>(kind == 12 || kind == 13 ? 2 : 3)
            || bytes[1] >= 128 || (bytes.size() == 3 && bytes[2] >= 128)) return std::nullopt;
        return ShortMessage(bytes[0], bytes[1], bytes.size() == 3 ? bytes[2] : 0, timestamp);
    }
    int getChannel() const noexcept { return bytes_[0] < 0xf0 ? (bytes_[0] & 15) + 1 : 0; }
    double getTimeStamp() const noexcept { return timestamp_; }
    void setTimeStamp(double value) noexcept { timestamp_ = value; }
    int getNoteNumber() const noexcept { return bytes_[1]; }
    float getFloatVelocity() const noexcept { return static_cast<float>(bytes_[2]) * (1.0f / 127.0f); }
    int getAfterTouchValue() const noexcept { return bytes_[2]; }
    int getChannelPressureValue() const noexcept { return bytes_[1]; }
    int getControllerNumber() const noexcept { return bytes_[1]; }
    int getControllerValue() const noexcept { return bytes_[2]; }
    int getPitchWheelValue() const noexcept { return bytes_[1] + (static_cast<int>(bytes_[2]) << 7); }
    bool isNoteOn(bool includeZeroVelocity = false) const noexcept
    { return kind() == 9 && (includeZeroVelocity || bytes_[2] != 0); }
    bool isNoteOff(bool includeZeroVelocityNoteOn = true) const noexcept
    { return kind() == 8 || (includeZeroVelocityNoteOn && kind() == 9 && bytes_[2] == 0); }
    bool isNoteOnOrOff() const noexcept { return kind() == 8 || kind() == 9; }
    bool isAftertouch() const noexcept { return kind() == 10; }
    bool isController() const noexcept { return kind() == 11; }
    bool isProgramChange() const noexcept { return kind() == 12; }
    bool isChannelPressure() const noexcept { return kind() == 13; }
    bool isPitchWheel() const noexcept { return kind() == 14; }
    bool isAllNotesOff() const noexcept { return isController() && bytes_[1] == 123; }
    bool isAllSoundOff() const noexcept { return isController() && bytes_[1] == 120; }
    std::span<const uint8_t> bytes() const noexcept
    { return {bytes_.data(), bytes_[0] >= 0xf0 ? size_t{1} : (kind() == 12 || kind() == 13 ? size_t{2} : size_t{3})}; }
    static ShortMessage controllerEvent(int channel, int controller, int value) noexcept
    { return {0xb0 | ((channel - 1) & 15), controller, value}; }
    static ShortMessage allNotesOff(int channel) noexcept { return controllerEvent(channel, 123, 0); }
    static ShortMessage allSoundOff(int channel) noexcept { return controllerEvent(channel, 120, 0); }
    static ShortMessage noteOn(int channel, int note, uint8_t velocity) noexcept
    { return {0x90 | ((channel - 1) & 15), note, velocity}; }
    static ShortMessage noteOff(int channel, int note) noexcept
    { return {0x80 | ((channel - 1) & 15), note, 0}; }
    static ShortMessage aftertouchChange(int channel, int note, int value) noexcept
    { return {0xa0 | ((channel - 1) & 15), note, value}; }
    static ShortMessage programChange(int channel, int program) noexcept
    { return {0xc0 | ((channel - 1) & 15), program}; }
    static ShortMessage channelPressureChange(int channel, int value) noexcept
    { return {0xd0 | ((channel - 1) & 15), value}; }
    static ShortMessage pitchWheel(int channel, int value) noexcept
    { return {0xe0 | ((channel - 1) & 15), value & 127, (value >> 7) & 127}; }
    static double getMidiNoteInHertz(int note) noexcept
    { return 440.0 * std::pow(2.0, static_cast<double>(note - 69) / 12.0); }
private:
    int kind() const noexcept { return bytes_[0] >> 4; }
    std::array<uint8_t, 3> bytes_{0xfe, 0, 0};
    double timestamp_ = 0;
};

// MIDI 2.0 bit expansion preserves the centre exactly. Above centre, repeat
// the remaining (Bits-1) bit word until all low destination bits are filled.
// This loop derives both 7->32 and 14->32 forms from that rule.
template<unsigned Bits> constexpr uint32_t expandMidi1(uint32_t value) noexcept
{
    static_assert(Bits == 7 || Bits == 14);
    const uint32_t centre = uint32_t{1} << (Bits - 1);
    uint32_t expanded = value << (32 - Bits);
    if (value > centre)
    {
        const uint32_t word = value & (centre - 1);
        for (int shift = 32 - int(Bits) - int(Bits - 1); shift > -int(Bits - 1); shift -= int(Bits - 1))
            expanded |= shift >= 0 ? word << shift : word >> -shift;
    }
    return expanded;
}
}
