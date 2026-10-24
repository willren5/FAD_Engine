#pragma once
#include <array>
#include <vector>
#include <cmath>
#include <cstddef>

namespace fadmodel1::dsp::modulation
{
struct NativeLfoCurveData
{
    static constexpr size_t maxCurves = 8, maxPoints = 480, busCount = 16, maxAssignments = 128;
    struct Point
    {
        float x = 0.0f, y = 0.0f, coefficient = 0.0f;
        int xBus = -1, yBus = -1;
        int curveBus = -1;
        float curvePosition = .5f;
    };
    struct Curve
    {
        std::vector<Point> points;
        int loopStartIndex = -1;
    };
    std::array<float, busCount> busBases{};
    std::vector<Curve> curves;

    bool empty() const noexcept { return curves.empty(); }

    // Non-RT validation: publication must validate every point before readers
    // receive this object. Runtime readers still enforce fixed capacity bounds.
    bool isValid(size_t frameCount) const noexcept
    {
        if (empty()) return true;
        if (curves.size() != frameCount || curves.size() > maxCurves) return false;
        for (const auto value : busBases)
            if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
        size_t assignments = 0;
        for (const auto& curve : curves)
        {
            const auto count = curve.points.size();
            if (count == 0) { if (curve.loopStartIndex != -1) return false; continue; }
            if (count < 2 || count > maxPoints || curve.points.front().x != 0.0f
                || curve.points.back().x != 1.0f) return false;
            if (curve.loopStartIndex < -1 || curve.loopStartIndex >= static_cast<int>(count)) return false;
            if (curve.loopStartIndex >= 0 && curve.points[static_cast<size_t>(curve.loopStartIndex)].x >= 1.0f) return false;
            float previous = 0.0f;
            for (const auto& point : curve.points)
            {
                if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.coefficient)
                    || point.x < previous || point.x > 1.0f || point.y < 0.0f || point.y > 1.0f
                    || std::abs(point.coefficient) > 100.0f
                    || point.xBus < -1 || point.xBus >= static_cast<int>(busCount)
                    || point.yBus < -1 || point.yBus >= static_cast<int>(busCount)
                    || point.curveBus < -1 || point.curveBus >= static_cast<int>(busCount)
                    || !std::isfinite(point.curvePosition) || point.curvePosition < 0.0f
                    || point.curvePosition > 1.0f) return false;
                previous = point.x;
                assignments += static_cast<size_t>(point.xBus >= 0) + static_cast<size_t>(point.yBus >= 0)
                    + static_cast<size_t>(point.curveBus >= 0);
            }
        }
        return assignments <= maxAssignments;
    }

};
}
