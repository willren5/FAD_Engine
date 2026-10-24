#pragma once
#include <fad/engine/midi/ShortMessage.h>
#include <fad/engine/midi/MidiSource.h>
#include <fad/engine/midi/UmpChannelVoice.h>

namespace fadmodel1::dsp::midi
{
struct RenderMidiEvent
{
    ShortMessage message;
    int samplePosition = 0;
    Source source = Source::host;
    uint32_t stableOrder = 0;
    UmpChannelVoice ump;
};
using MidiEventSpan = std::span<const RenderMidiEvent>;

inline ShortMessage umpProxyShortMessage(const UmpChannelVoice& packet) noexcept
{
    const auto channel = packet.channel(), index = packet.byte2();
    switch (packet.status())
    {
        case 8: return ShortMessage::noteOff(channel, index);
        case 9: return ShortMessage::noteOn(channel, index, uint8_t{1});
        case 10: return ShortMessage::aftertouchChange(channel, index, static_cast<int>(packet.data() >> 25));
        case 11: return ShortMessage::controllerEvent(channel, index, static_cast<int>(packet.data() >> 25));
        case 12: return ShortMessage::programChange(channel, static_cast<int>((packet.data() >> 24) & 127));
        case 13: return ShortMessage::channelPressureChange(channel, static_cast<int>(packet.data() >> 25));
        case 14: return ShortMessage::pitchWheel(channel, static_cast<int>(packet.data() >> 18));
        default: return ShortMessage(0xfe);
    }
}
}
