#pragma once
#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fadmodel1::dsp::oscillators
{
// The existing Voice law, shared with off-thread waveform previews. This is
// presentation parity, not a change to the saved oscillator parameter meaning.
inline void applyOscillatorTimbreMapping(const parameters::OscillatorSnapshot& source,
    parameters::OscillatorSnapshot& target, float wavetablePositionOffset = 0.0f,
    float fmIndexOffset = 0.0f) noexcept
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const auto mappedTimbre = std::clamp(source.timbre, 0.0f, 1.0f);
    switch (source.mode)
    {
        case parameters::OscillatorMode::analog:
            target.analogShape =
                std::clamp(0.5f * source.analogShape + 0.5f * mappedTimbre, 0.0f, 1.0f);
            target.analogPulseWidth = std::clamp(0.5f * source.analogPulseWidth + 0.5f * fad::engine::math::interpolate(mappedTimbre, 0.12f, 0.88f), 0.05f, 0.95f);
            break;

        case parameters::OscillatorMode::wavetable:
            target.wavetablePosition = std::clamp(0.5f * source.wavetablePosition + 0.5f * mappedTimbre
                    + wavetablePositionOffset, 0.0f, 1.0f);
            if (const auto warpModulation = fmIndexOffset;
                warpModulation != 0.0f)
            {
                target.wavetableWarp = std::clamp(source.wavetableWarp + warpModulation * 0.2f, 0.0f, 1.0f);
            }
            break;

        case parameters::OscillatorMode::fm2Op:
            target.fmIndex = std::clamp((source.wideFmIndex ? source.fmIndex : 0.35f * source.fmIndex + 0.65f * (mappedTimbre * 10.0f))
                    + fmIndexOffset, 0.0f, source.wideFmIndex ? 100.0f : 10.0f);
            break;
    }
}
}
