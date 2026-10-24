#pragma once

#include <array>
#include <optional>

#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fadmodel1::dsp::diagnostics
{
enum class ModuleId : int
{
    osc1,
    osc2,
    subOsc,
    noise,
    filter1,
    ampEnvelope,
    filterEnvelope,
    lfo1,
    lfo2,
    lfo3,
    lfo4,
    modMatrix,
    driveFx,
    chorusFx,
    delayFx,
    reverbFx,
    eq3BandFx,
    compressorFx,
    phaserFx,
    flangerFx,
    distortionFx,
    bitcrusherFx,
    filterFx,
    stereoFx,
    limiterFx,
    fxSlot1,
    fxSlot2,
    fxSlot3,
    fxSlot4,
    fxSlot5,
    fxSlot6,
    fxSlot7,
    fxSlot8,
    masterOutput,
    fxSlot9,
    fxSlot10,
    count
};

static constexpr int maxFxSlotModules = parameters::ids::maxFxSlots;
inline constexpr std::array<ModuleId, maxFxSlotModules> fxSlotModuleIds{
    ModuleId::fxSlot1, ModuleId::fxSlot2, ModuleId::fxSlot3, ModuleId::fxSlot4,
    ModuleId::fxSlot5, ModuleId::fxSlot6, ModuleId::fxSlot7, ModuleId::fxSlot8,
    ModuleId::fxSlot9, ModuleId::fxSlot10};
// The old output module retains its diagnostic identity.
static_assert(static_cast<int>(ModuleId::masterOutput) == 33);

inline std::optional<ModuleId> fxSlotModuleIdForIndex(int slotIndex) noexcept
{
    if (slotIndex < 0 || slotIndex >= maxFxSlotModules) return std::nullopt;
    return fxSlotModuleIds[static_cast<size_t>(slotIndex)];
}

inline bool isFxSlotModule(ModuleId id) noexcept
{
    for (const auto slotId : fxSlotModuleIds) if (slotId == id) return true;
    return false;
}

struct ModuleNode
{
    ModuleId id = ModuleId::masterOutput;
    bool active = false;
    bool processingObserved = false;
    float cpuMicroseconds = 0.0f;
    float cpuPercent = 0.0f;
    bool clipping = false;
    float peakLevel = 0.0f;
    ModuleId semanticId = ModuleId::masterOutput;
    int slotIndex = -1;
};

struct SignalEdge
{
    ModuleId from = ModuleId::masterOutput;
    ModuleId to = ModuleId::masterOutput;
    bool active = false;
};

struct SignalFlowSnapshot
{
    std::array<ModuleNode, static_cast<int>(ModuleId::count)> nodes{};
    // Ten parallel lanes can fan each dry source into a distinct first FX.
    static constexpr size_t maxEdges = 96;
    std::array<SignalEdge, maxEdges> edges{};
    size_t edgeCount = 0;
    bool cpuTimingAvailable = false;
    float totalBlockMicroseconds = 0.0f;
    float blockBudgetMicroseconds = 0.0f;
};

SignalFlowSnapshot buildSignalFlowGraph(
    const parameters::ParameterSnapshot& snapshot,
    double sampleRate,
    int blockSize) noexcept;
} // namespace fadmodel1::dsp::diagnostics
