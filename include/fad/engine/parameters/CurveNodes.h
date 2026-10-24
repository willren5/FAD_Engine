#pragma once
#include "ParameterTypes.h"
#include <array>

namespace fadmodel1::dsp::wavetable { struct WavetableData; }
namespace fadmodel1::parameters
{
inline constexpr size_t legacyCurveOwnerCount = 11;
inline constexpr size_t eqCurveOwnerBegin = legacyCurveOwnerCount;
inline constexpr size_t curveOwnerCount = eqCurveOwnerBegin + ids::maxFxSlots;
inline constexpr size_t curveNodeCount = 16, curveModCount = 8;
inline constexpr size_t curveFieldCount = 1 + curveNodeCount * 4 + curveModCount * 3;
struct CurveNode { float x=0, y=.5f, c=0; bool enabled=false; bool operator==(const CurveNode&) const = default; };
struct CurveNodeMod { int target=0, source=0; float amount=0; };
struct CurveNodes
{
    bool enabled=false;
    std::array<CurveNode,curveNodeCount> nodes{};
    std::array<CurveNodeMod,curveModCount> mods{};
    int historySlot=0;
    float historyPosition=1.f;
    int historySource=0;
    float historyDepth=0.f;
    // Block-local view, pinned by the existing WavetableBank ownership guard.
    const dsp::wavetable::WavetableData* history=nullptr;
};
inline bool compatibleCurveHistoryOwner(int assetOwner, int owner) noexcept
{
    // Removing a physical FX slot compacts its parameters. EQ histories retain
    // their immutable assets; only their referencing slot changes.
    return assetOwner==owner || (assetOwner>=int(eqCurveOwnerBegin) && assetOwner<int(curveOwnerCount)
        && owner>=int(eqCurveOwnerBegin) && owner<int(curveOwnerCount));
}
}
