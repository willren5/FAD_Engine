#pragma once
#include <array>
#include <cmath>
#include <algorithm>
namespace fadmodel1::dsp::fx {
// Native stepped-input measurements at five Drive settings collapse onto
// f(abs(input) / 2 + Drive). Values cover z=0..1.6; interpolate without I/O.
inline float serumZeroSquare(float input, float drive) noexcept
{
    if (!std::isfinite(input) || !std::isfinite(drive) || input == 0.0f) return 0.0f;
    static constexpr std::array<float, 81> curve{
        0.000000000f, 0.019795848f, 0.039591701f, 0.059387548f, 0.079183396f, 0.098979252f,
        0.118775100f, 0.138570947f, 0.158366798f, 0.178162630f, 0.197958504f, 0.217754357f,
        0.237550198f, 0.257345896f, 0.277141186f, 0.296936965f, 0.316732762f, 0.336528048f,
        0.356322479f, 0.376116772f, 0.395909801f, 0.415699870f, 0.435487832f, 0.455271058f,
        0.475046587f, 0.494812227f, 0.514562953f, 0.534293440f, 0.553993077f, 0.573653191f,
        0.593258839f, 0.612788952f, 0.632219942f, 0.651522083f, 0.670658261f, 0.689579741f,
        0.708230946f, 0.726551494f, 0.744468122f, 0.761897280f, 0.778758353f, 0.794970600f,
        0.810445158f, 0.825111245f, 0.838913584f, 0.851815629f, 0.863779818f, 0.874805802f,
        0.884919179f, 0.894142530f, 0.902513147f, 0.910096551f, 0.916943769f, 0.923112968f,
        0.928665841f, 0.933664287f, 0.938159589f, 0.942203385f, 0.945843750f, 0.949126737f,
        0.952087037f, 0.954758037f, 0.957174780f, 0.959363290f, 0.961345661f, 0.963145919f,
        0.964783764f, 0.966273670f, 0.967631384f, 0.968872000f, 0.970007011f, 0.971044518f,
        0.971996049f, 0.972870098f, 0.973672750f, 0.974411161f, 0.975091861f, 0.975719781f,
        0.976298924f, 0.976834672f, 0.977330733f
    };
    const auto magnitude = std::abs(input);
    // Independent low-level steps show that the square offset is disabled
    // below 0.0002 at the effect input. Omitting this native dead zone turns
    // decaying filter residue into a sustained full-level square wave.
    if (magnitude < 2.0e-7f) return input;
    const float z = .5f * magnitude
        + (magnitude >= .0002f ? std::clamp(drive, 0.0f, 1.0f) : 0.0f);
    float output;
    if (z >= 1.6f) {
        // Smooth bounded continuation outside the measured input range.
        const auto room = 1.0f - curve.back();
        const auto slope = (curve.back() - curve[curve.size()-2]) / .02f;
        output = 1.0f - room / (1.0f + (z - 1.6f) * slope / room);
    } else {
        const auto position = z * 50.0f;
        const auto index = static_cast<size_t>(position);
        output = curve[index] + (curve[index+1] - curve[index]) * (position - static_cast<float>(index));
    }
    return std::copysign(output, input);
}
}
