#pragma once
#include "ModMatrix.h"
#include "NativeLfoCurveRuntime.h"
#include <fad/engine/assets/WavetableData.h>

namespace fadmodel1::dsp::modulation
{
// Resolve all buses from one source snapshot before applying any point changes.
// Bounded and non-recursive: route ordering cannot alter the result.
template <typename Render>
void applyNativeLfoBuses(const parameters::ModulationSnapshot& snapshot, SourceValues& values,
    const std::array<const wavetable::WavetableData*, parameters::lfoCount>& banks,
    std::array<NativeLfoCurveRuntime, parameters::lfoCount>& runtimes,
    uint32_t generation, Render&& render) noexcept
{
    bool any = false;
    for (const auto* bank : banks) any = any || (bank != nullptr && !bank->nativeCurves.empty());
    if (!any) return;
    std::array<float, NativeLfoCurveData::busCount> offsets{};
    for (const auto& route : snapshot.slots)
    {
        if (route.nativeTarget < 299 || route.nativeTarget > 314 || !route.hasSource()) continue;
        const auto contribution = ModMatrix::resolveRouteSourceValue(route, values) * route.amount;
        if (!std::isfinite(contribution)) continue;
        auto& value = offsets[static_cast<size_t>(route.nativeTarget - 299)];
        value = std::clamp(value + contribution, -32.0f, 32.0f);
    }
    const std::array<float*, 4> legacy{&values.lfo1, &values.lfo2, &values.lfo3, &values.lfo4};
    for (size_t i = 0; i < banks.size(); ++i)
    {
        const auto* bank = banks[i];
        if (bank == nullptr || bank->nativeCurves.curves.size() <= i
            || bank->nativeCurves.curves[i].points.empty()) continue;
        auto buses = bank->nativeCurves.busBases;
        for (size_t bus = 0; bus < buses.size(); ++bus) buses[bus] += offsets[bus];
        runtimes[i].prepare(bank->nativeCurves.curves[i], buses, generation);
        const auto raw = utils::sanitizeUnitInterval(render(i, runtimes[i]));
        values.nativeLfos[i] = raw;
        values.fadLfos[i] = raw;
        values.unipolarLfoMask |= uint32_t{1} << i;
        if (i < legacy.size()) *legacy[i] = raw * values.lfoDepth[i];
    }
}
}
