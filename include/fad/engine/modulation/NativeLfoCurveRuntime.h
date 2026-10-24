#pragma once
#include <fad/engine/assets/NativeLfoCurveData.h>
#include <fad/engine/math/DspSafetyUtils.h>
#include "NativeLfoCurveCalibration.h"
#include <array>
#include <cstdint>

namespace fadmodel1::dsp::modulation
{
// Fixed-capacity per-reader cache. Static macro positions prepare once; dynamic
// positions prepare only when the bus values change. Sampling is logarithmic.
class NativeLfoCurveRuntime final
{
public:
    void invalidate() noexcept { identity = nullptr; count = 0; }
    void prepare(const NativeLfoCurveData::Curve& curve,
                 const std::array<float, NativeLfoCurveData::busCount>& buses,
                 uint32_t generation) noexcept
    {
        if (identity == &curve && lastGeneration == generation && lastBuses == buses) return;
        identity = &curve; lastGeneration = generation; lastBuses = buses;
        count = curve.points.size();
        if (count < 2 || count > nodes.size()) { count = 0; return; }
        float previous = 0.0f;
        endpoint = count - 1;
        bool foundEndpoint = false;
        for (size_t i = 0; i < count; ++i)
        {
            const auto& source = curve.points[i];
            const auto bus = [&](int index) noexcept {
                return index >= 0 && index < static_cast<int>(buses.size())
                    ? utils::sanitizeFiniteClamped(buses[static_cast<size_t>(index)], 0.0f, -32.0f, 32.0f) : 0.0f;
            };
            auto& point = nodes[i];
            point.x = std::max(previous, utils::sanitizeUnitInterval(source.x + bus(source.xBus)));
            if (i == 0) point.x = 0.0f;
            if (i + 1 == count) point.x = 1.0f;
            point.y = utils::sanitizeUnitInterval(source.y + bus(source.yBus));
            point.coefficient = utils::sanitizeFiniteClamped(source.coefficient, 0.0f, -100.0f, 100.0f);
            const auto curveOffset = bus(source.curveBus);
            if (source.curveBus >= 0 && curveOffset != 0.0f && i + 1 < count)
            {
                // Native isolated positive/negative/zero-offset probes are
                // bit-identical to a static curve at base + bus/2. Convert in
                // position space, not exponential-coefficient space.
                const auto position = static_cast<double>(utils::sanitizeUnitInterval(
                    source.curvePosition + .5f * curveOffset)) * 40.0;
                const auto index = std::clamp(static_cast<int>(position), 0, 39);
                const auto fraction = position - index;
                point.coefficient = static_cast<float>(serumLfoCurveCoefficients[static_cast<size_t>(index)] * (1.0 - fraction)
                    + serumLfoCurveCoefficients[static_cast<size_t>(index + 1)] * fraction);
            }
            previous = point.x;
            if (!foundEndpoint && point.x >= 1.0f) { endpoint = i; foundEndpoint = true; }
        }
        loopStart = curve.loopStartIndex >= 0 && static_cast<size_t>(curve.loopStartIndex) < endpoint
            ? nodes[static_cast<size_t>(curve.loopStartIndex)].x : -1.0f;
    }

    float getLoopStart() const noexcept { return loopStart; }
    float sample(double phase) const noexcept
    {
        if (count < 2 || !std::isfinite(phase)) return 0.0f;
        if (phase >= 1.0) return nodes[endpoint].y;
        phase = std::clamp(phase, 0.0, 1.0);
        size_t left = 0, right = endpoint;
        while (left + 1 < right)
        {
            const auto middle = left + (right - left) / 2;
            if (nodes[middle].x <= phase) left = middle;
            else right = middle;
        }
        const auto& a = nodes[left]; const auto& b = nodes[right];
        const auto width = static_cast<double>(b.x) - a.x;
        const auto t = width > 0 ? std::clamp((phase - a.x) / width, 0.0, 1.0) : 1.0;
        const auto k = static_cast<double>(a.coefficient);
        const auto shaped = std::abs(k) < 1.e-5 ? t : std::expm1(k * t) / std::expm1(k);
        return utils::sanitizeUnitInterval(static_cast<float>(a.y + (b.y - a.y) * shaped));
    }

private:
    struct Point { float x = 0, y = 0, coefficient = 0; };
    std::array<Point, NativeLfoCurveData::maxPoints> nodes{};
    std::array<float, NativeLfoCurveData::busCount> lastBuses{};
    const NativeLfoCurveData::Curve* identity = nullptr;
    uint32_t lastGeneration = 0;
    size_t count = 0, endpoint = 0;
    float loopStart = -1.0f;
};
}
