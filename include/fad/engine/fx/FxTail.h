#pragma once
#include <fad/engine/fx/FxCatalog.h>
#include <fad/engine/fx/NativeHallReverb.h>
#include <fad/engine/math/Numeric.h>
namespace fadmodel1::dsp::fx
{
// Slot mode is additive: existing presets have normalized mode 0 (Legacy).
// The UI offers only explicit Original (.5) and Hall (1); old Decay automation
// keeps selecting the same algorithm until an explicit mode is saved.
inline bool reverbUsesHall(float modeNormalized, float decayNormalized) noexcept
{
    const auto mode = fad::engine::math::roundToNearestInt(fadmodel1::dsp::utils::sanitizeUnitInterval(modeNormalized) * 2.0f);
    return mode == 2 || (mode == 0 && decayNormalized >= 0.5f);
}

// Extended Plate delay is additive; a missing reserved slot value (0) keeps
// the legacy range. A saved zero-ms extended delay remains explicitly active.
inline bool reverbHasExtendedPreDelay(float normalized) noexcept
{
    return fadmodel1::dsp::utils::sanitizeUnitInterval(normalized) >= 1.0f / 251.0f;
}

inline float reverbPreDelayMilliseconds(float legacyNormalized, float extendedNormalized) noexcept
{
    if (reverbHasExtendedPreDelay(extendedNormalized))
        return fadmodel1::dsp::utils::sanitizeUnitInterval(extendedNormalized) * 251.0f - 1.0f;
    return fadmodel1::dsp::utils::sanitizeUnitInterval(legacyNormalized) * 100.0f;
}

// Shared by the host query and retired FX graph budget. Values are slot
// normalized values, so the same mode and delay rules govern both paths.
inline double reverbTailSeconds(float sizeNormalized, float nativeDecayNormalized,
                               float hallDecayNormalized, float modeNormalized,
                               float legacyPreDelayNormalized, float extendedPreDelayNormalized,
                               float spinDepthNormalized = 0.0f) noexcept
{
    const auto size = fadmodel1::dsp::utils::sanitizeUnitInterval(sizeNormalized);
    const auto hall = reverbUsesHall(modeNormalized, hallDecayNormalized);
    const auto preDelay = static_cast<double>(reverbPreDelayMilliseconds(
        legacyPreDelayNormalized, hall ? 0.0f : extendedPreDelayNormalized)) * 0.001;
    if (hall) {
        const auto rawDecay = FxCatalog::denormalizeParamValue(FxType::reverb, 8, hallDecayNormalized);
        return preDelay + NativeHallReverb::tailSeconds(size, rawDecay >= 0.0f ? rawDecay : 0.35f, spinDepthNormalized);
    }
    const auto feedback = fad::engine::math::interpolate(static_cast<double>(size), 0.62, 0.92);
    const auto originalTail = 0.18 * std::ceil(std::log(0.001) / std::log(feedback));
    const auto nativeDecay = FxCatalog::denormalizeParamValue(FxType::reverb, 7, nativeDecayNormalized);
    return preDelay + std::max(originalTail, static_cast<double>(nativeDecay) * 2.0);
}

}
