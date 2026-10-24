#pragma once

#include "ParameterTypes.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>

namespace fadmodel1::parameters
{
inline constexpr int fxRoutingCapacity = ids::maxFxSlots;

inline int resolvedFxRoutingStage(int slot, int stage) noexcept
{
    return stage >= 0 && stage < fxRoutingCapacity ? stage : slot;
}

struct FxRoutingLane
{
    int id = 0;
    int count = 0;
    std::array<int, fxRoutingCapacity> slots{};
};

struct FxRoutingStage
{
    int id = 0;
    int count = 0;
    std::array<FxRoutingLane, fxRoutingCapacity> lanes{};
};

// Stages and lanes take their order from their first configured slot. Every
// slot belongs to exactly one lane; this bounded graph cannot form feedback.
struct FxRoutingPlan
{
    int count = 0;
    int slotCount = 0;
    bool parallel = false;
    std::array<FxRoutingStage, fxRoutingCapacity> stages{};
    std::array<int, fxRoutingCapacity> flattenedOrder{};
};

inline FxRoutingPlan buildFxRoutingPlan(
    const std::array<int, fxRoutingCapacity>& order, int slotCount,
    const std::array<int, fxRoutingCapacity>& stages,
    const std::array<int, fxRoutingCapacity>& lanes,
    uint32_t configuredMask = (uint32_t{1} << fxRoutingCapacity) - 1) noexcept
{
    FxRoutingPlan result;
    std::array<bool, fxRoutingCapacity> visited{};
    slotCount = std::clamp(slotCount, 0, fxRoutingCapacity);
    for (int position = 0; position < slotCount; ++position)
    {
        const auto slot = order[static_cast<size_t>(position)];
        if (slot < 0 || slot >= slotCount || visited[static_cast<size_t>(slot)]
            || (configuredMask & (uint32_t{1} << slot)) == 0) continue;
        visited[static_cast<size_t>(slot)] = true;
        const auto stageId = resolvedFxRoutingStage(slot, stages[static_cast<size_t>(slot)]);
        const auto laneId = std::clamp(lanes[static_cast<size_t>(slot)], 0, fxRoutingCapacity - 1);
        int stageIndex = 0;
        while (stageIndex < result.count && result.stages[static_cast<size_t>(stageIndex)].id != stageId)
            ++stageIndex;
        if (stageIndex == result.count)
            result.stages[static_cast<size_t>(result.count++)].id = stageId;
        auto& stage = result.stages[static_cast<size_t>(stageIndex)];
        int laneIndex = 0;
        while (laneIndex < stage.count && stage.lanes[static_cast<size_t>(laneIndex)].id != laneId)
            ++laneIndex;
        if (laneIndex == stage.count)
            stage.lanes[static_cast<size_t>(stage.count++)].id = laneId;
        auto& lane = stage.lanes[static_cast<size_t>(laneIndex)];
        lane.slots[static_cast<size_t>(lane.count++)] = slot;
    }
    for (int s = 0; s < result.count; ++s)
    {
        const auto& stage = result.stages[static_cast<size_t>(s)];
        result.parallel = result.parallel || stage.count > 1;
        for (int l = 0; l < stage.count; ++l)
        {
            const auto& lane = stage.lanes[static_cast<size_t>(l)];
            for (int i = 0; i < lane.count; ++i)
                result.flattenedOrder[static_cast<size_t>(result.slotCount++)] = lane.slots[static_cast<size_t>(i)];
        }
    }
    return result;
}

}
