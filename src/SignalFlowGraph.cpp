#include <fad/engine/diagnostics/SignalFlowGraph.h>

#include <cmath>

#include <fad/engine/fx/FxCatalog.h>

namespace fadmodel1::dsp::diagnostics
{
namespace
{
struct DynamicFxSlotEntry
{
    int order = 0;
    int slotIndex = 0;
    ModuleId semanticId = ModuleId::masterOutput;
};

constexpr float activationThreshold = 1.0e-4f;
constexpr int driveAmountParamIndex = 0;
constexpr int driveMixParamIndex = 2;
constexpr int chorusDepthParamIndex = 1;
constexpr int chorusMixParamIndex = 4;
constexpr int delayMixParamIndex = 4;
constexpr int reverbWetParamIndex = 2;

ModuleId semanticModuleIdForFxType(fx::FxType type) noexcept
{
    switch (type)
    {
        case fx::FxType::drive:  return ModuleId::driveFx;
        case fx::FxType::chorus: return ModuleId::chorusFx;
        case fx::FxType::delay:  return ModuleId::delayFx;
        case fx::FxType::reverb: return ModuleId::reverbFx;
        case fx::FxType::eq3Band: return ModuleId::eq3BandFx;
        case fx::FxType::compressor: return ModuleId::compressorFx;
        case fx::FxType::phaser: return ModuleId::phaserFx;
        case fx::FxType::flanger: return ModuleId::flangerFx;
        case fx::FxType::distortion: return ModuleId::distortionFx;
        case fx::FxType::bitcrusher: return ModuleId::bitcrusherFx;
        case fx::FxType::filterFx: return ModuleId::filterFx;
        case fx::FxType::stereo: return ModuleId::stereoFx;
        case fx::FxType::limiter: return ModuleId::limiterFx;
        case fx::FxType::none:
        default:
            return ModuleId::masterOutput;
    }
}

bool genericFxSlotActive(fx::FxType type, const parameters::GenericFxSlotSnapshot& slot) noexcept
{
    switch (type)
    {
        case fx::FxType::eq3Band:
            return std::abs(slot.params[1]) > activationThreshold
                || std::abs(slot.params[3]) > activationThreshold
                || std::abs(slot.params[6]) > activationThreshold;
        case fx::FxType::compressor:
            return slot.params[5] > activationThreshold;
        case fx::FxType::phaser:
        case fx::FxType::flanger:
            return slot.params[1] > activationThreshold
                && slot.params[5] > activationThreshold;
        case fx::FxType::distortion:
            return slot.params[0] > activationThreshold
                && slot.params[2] > activationThreshold;
        case fx::FxType::bitcrusher:
            return slot.params[2] > activationThreshold;
        case fx::FxType::filterFx:
            return slot.params[1] > activationThreshold
                || slot.params[3] > activationThreshold;
        case fx::FxType::stereo:
            return slot.params[4] > activationThreshold;
        case fx::FxType::limiter:
            return slot.params[4] > activationThreshold;
        case fx::FxType::drive:
        case fx::FxType::chorus:
        case fx::FxType::delay:
        case fx::FxType::reverb:
        case fx::FxType::none:
        default:
            return false;
    }
}

bool isLfoUsedAsSource(const parameters::ModulationSnapshot& mod, parameters::ModSource src) noexcept
{
    for (const auto& slot : mod.slots)
    {
        if (slot.source == src
            && slot.destination != parameters::ModDestination::none
            && std::abs(slot.amount) > 0.0001f)
            return true;
    }
    return false;
}

bool anyModSlotActive(const parameters::ModulationSnapshot& mod) noexcept
{
    for (const auto& slot : mod.slots)
    {
        if (slot.source != parameters::ModSource::none
            && slot.destination != parameters::ModDestination::none
            && std::abs(slot.amount) > 0.0001f)
            return true;
    }
    return false;
}
} // namespace

SignalFlowSnapshot buildSignalFlowGraph(
    const parameters::ParameterSnapshot& snapshot,
    double sampleRate,
    int blockSize) noexcept
{
    SignalFlowSnapshot flow;

    const bool osc1Active = snapshot.oscillators[0].enabled && snapshot.oscillators[0].level > 0.0001f;
    const bool osc2Active = snapshot.oscillators[1].enabled && snapshot.oscillators[1].level > 0.0001f;
    const bool subActive = snapshot.subLevel > 0.0001f;
    const bool noiseActive = snapshot.noiseLevel > 0.0001f;
    const bool anySourceActive = osc1Active || osc2Active || subActive || noiseActive;
    const bool filterWet = snapshot.filter.enabled && snapshot.filter.mix > 0.0f;
    const std::array<bool, 4> sourceActive{{osc1Active, osc2Active, subActive, noiseActive}};
    bool filterActive = false;
    for (size_t i = 0; i < sourceActive.size(); ++i)
        filterActive = filterActive || (filterWet && snapshot.filter.inputs[i] && sourceActive[i]);
    const bool ampEnvActive = anySourceActive;
    const bool filterEnvActive = anySourceActive;
    const bool lfo1Active = isLfoUsedAsSource(snapshot.modulation, parameters::ModSource::lfo1);
    const bool lfo2Active = isLfoUsedAsSource(snapshot.modulation, parameters::ModSource::lfo2);
    const bool lfo3Active = isLfoUsedAsSource(snapshot.modulation, parameters::ModSource::lfo3);
    const bool lfo4Active = isLfoUsedAsSource(snapshot.modulation, parameters::ModSource::lfo4);
    const bool modMatrixActive = anyModSlotActive(snapshot.modulation);

    bool driveActive = false;
    bool chorusActive = false;
    bool delayActive = false;
    bool reverbActive = false;
    std::array<DynamicFxSlotEntry, parameters::ids::maxFxSlots> dynamicFxOrder{};
    size_t dynamicFxOrderCount = 0;

    const auto pushDynamicFxSlot = [&](DynamicFxSlotEntry entry) noexcept
    {
        if (dynamicFxOrderCount < dynamicFxOrder.size())
            dynamicFxOrder[dynamicFxOrderCount++] = entry;
    };

    const auto clampedSlotCount = std::clamp(snapshot.dynamicFx.slotCount, 0, static_cast<int>(snapshot.dynamicFx.slots.size()));
    const auto dynamicFxProcessingRanks =
        parameters::buildDynamicFxProcessingRanks(snapshot.dynamicFx, clampedSlotCount);
    bool dynamicFxPopulated = snapshot.dynamicFx.hasExplicitTopology;
    if (snapshot.dynamicFx.hasExplicitTopology)
    {
        for (int slotIndex = 0; slotIndex < clampedSlotCount; ++slotIndex)
        {
            const auto& slot = snapshot.dynamicFx.slots[static_cast<size_t>(slotIndex)];
            const auto type = fx::FxCatalog::sanitizeTypeValue(slot.type);
            if (type == fx::FxType::none)
                continue;

            if (! slot.enabled)
                continue;

            switch (type)
            {
                case fx::FxType::drive:
                {
                    const bool slotActive = slot.params[driveMixParamIndex] > activationThreshold
                        || slot.params[driveAmountParamIndex] > activationThreshold;
                    driveActive = driveActive || slotActive;
                    if (slotActive)
                        pushDynamicFxSlot({dynamicFxProcessingRanks[static_cast<size_t>(slotIndex)],
                                           slotIndex,
                                           semanticModuleIdForFxType(type)});
                    break;
                }
                case fx::FxType::chorus:
                {
                    const bool slotActive = slot.params[chorusMixParamIndex] > activationThreshold
                        && slot.params[chorusDepthParamIndex] > activationThreshold;
                    chorusActive = chorusActive || slotActive;
                    if (slotActive)
                        pushDynamicFxSlot({dynamicFxProcessingRanks[static_cast<size_t>(slotIndex)],
                                           slotIndex,
                                           semanticModuleIdForFxType(type)});
                    break;
                }
                case fx::FxType::delay:
                {
                    const bool slotActive = slot.params[delayMixParamIndex] > activationThreshold;
                    delayActive = delayActive || slotActive;
                    if (slotActive)
                        pushDynamicFxSlot({dynamicFxProcessingRanks[static_cast<size_t>(slotIndex)],
                                           slotIndex,
                                           semanticModuleIdForFxType(type)});
                    break;
                }
                case fx::FxType::reverb:
                {
                    const bool slotActive = slot.params[reverbWetParamIndex] > activationThreshold;
                    reverbActive = reverbActive || slotActive;
                    if (slotActive)
                        pushDynamicFxSlot({dynamicFxProcessingRanks[static_cast<size_t>(slotIndex)],
                                           slotIndex,
                                           semanticModuleIdForFxType(type)});
                    break;
                }
                case fx::FxType::eq3Band:
                case fx::FxType::compressor:
                case fx::FxType::phaser:
                case fx::FxType::flanger:
                case fx::FxType::distortion:
                case fx::FxType::bitcrusher:
                case fx::FxType::filterFx:
                case fx::FxType::stereo:
                case fx::FxType::limiter:
                    if (genericFxSlotActive(type, slot))
                        pushDynamicFxSlot({dynamicFxProcessingRanks[static_cast<size_t>(slotIndex)],
                                           slotIndex,
                                           semanticModuleIdForFxType(type)});
                    break;
                case fx::FxType::none:
                default:
                    break;
            }
        }
    }

    if (! dynamicFxPopulated)
    {
        const auto& fx = snapshot.fx;
        driveActive = fx.drive.enabled
            && (fx.drive.amount > 0.0001f || fx.drive.mix > 0.0001f);
        chorusActive = fx.chorus.enabled
            && fx.chorus.mix > 0.0001f && fx.chorus.depth > 0.0001f;
        delayActive = fx.delay.enabled && fx.delay.mix > 0.0001f;
        reverbActive = fx.reverb.enabled && fx.reverb.wet > 0.0001f;
    }

    auto& nodes = flow.nodes;
    nodes[static_cast<int>(ModuleId::osc1)]           = {ModuleId::osc1, osc1Active, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::osc2)]           = {ModuleId::osc2, osc2Active, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::subOsc)]         = {ModuleId::subOsc, subActive, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::noise)]          = {ModuleId::noise, noiseActive, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::filter1)]        = {ModuleId::filter1, filterActive, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::ampEnvelope)]    = {ModuleId::ampEnvelope, ampEnvActive, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::filterEnvelope)] = {ModuleId::filterEnvelope, filterEnvActive, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::lfo1)]           = {ModuleId::lfo1, lfo1Active, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::lfo2)]           = {ModuleId::lfo2, lfo2Active, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::lfo3)]           = {ModuleId::lfo3, lfo3Active, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::lfo4)]           = {ModuleId::lfo4, lfo4Active, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::modMatrix)]      = {ModuleId::modMatrix, modMatrixActive, 0, 0, false, 0};
    const bool useLegacyFxNodes = ! dynamicFxPopulated;
    nodes[static_cast<int>(ModuleId::driveFx)]        = {ModuleId::driveFx, useLegacyFxNodes && driveActive, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::chorusFx)]       = {ModuleId::chorusFx, useLegacyFxNodes && chorusActive, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::delayFx)]        = {ModuleId::delayFx, useLegacyFxNodes && delayActive, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::reverbFx)]       = {ModuleId::reverbFx, useLegacyFxNodes && reverbActive, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::eq3BandFx)]      = {ModuleId::eq3BandFx, false, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::compressorFx)]   = {ModuleId::compressorFx, false, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::phaserFx)]       = {ModuleId::phaserFx, false, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::flangerFx)]      = {ModuleId::flangerFx, false, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::distortionFx)]   = {ModuleId::distortionFx, false, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::bitcrusherFx)]   = {ModuleId::bitcrusherFx, false, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::filterFx)]       = {ModuleId::filterFx, false, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::stereoFx)]       = {ModuleId::stereoFx, false, 0, 0, false, 0};
    nodes[static_cast<int>(ModuleId::limiterFx)]      = {ModuleId::limiterFx, false, 0, 0, false, 0};
    for (int slotIndex = 0; slotIndex < parameters::ids::maxFxSlots; ++slotIndex)
    {
        if (const auto slotId = fxSlotModuleIdForIndex(slotIndex))
        {
            nodes[static_cast<size_t>(static_cast<int>(*slotId))]
                = {*slotId, false, false, 0, 0, false, 0, ModuleId::masterOutput, slotIndex};
        }
    }
    nodes[static_cast<int>(ModuleId::masterOutput)]   = {ModuleId::masterOutput, true, 0, 0, false, 0};

    auto addEdge = [&](ModuleId from, ModuleId to)
    {
        if (flow.edgeCount >= SignalFlowSnapshot::maxEdges)
            return;
        const bool edgeActive = nodes[static_cast<size_t>(static_cast<int>(from))].active
                             && nodes[static_cast<size_t>(static_cast<int>(to))].active;
        flow.edges[flow.edgeCount++] = {from, to, edgeActive};
    };

    // Source → Filter
    const std::array<ModuleId, 4> sourceNodes{{ModuleId::osc1, ModuleId::osc2, ModuleId::subOsc, ModuleId::noise}};
    for (size_t i = 0; i < sourceNodes.size(); ++i)
        if (filterWet && snapshot.filter.inputs[i]) addEdge(sourceNodes[i], ModuleId::filter1);
    auto postFilterNode = dynamicFxPopulated ? ModuleId::masterOutput : ModuleId::driveFx;

    // FX chain
    if (dynamicFxPopulated)
    {
        if (dynamicFxOrderCount > 0)
        {
            for (size_t i = 1; i < dynamicFxOrderCount; ++i)
            {
                const auto candidate = dynamicFxOrder[i];
                size_t j = i;
                while (j > 0
                       && (dynamicFxOrder[j - 1].order > candidate.order
                           || (dynamicFxOrder[j - 1].order == candidate.order
                               && dynamicFxOrder[j - 1].slotIndex > candidate.slotIndex)))
                {
                    dynamicFxOrder[j] = dynamicFxOrder[j - 1];
                    --j;
                }
                dynamicFxOrder[j] = candidate;
            }

            for (size_t i = 0; i < dynamicFxOrderCount; ++i)
            {
                const auto& slot = dynamicFxOrder[i];
                if (const auto slotNodeId = fxSlotModuleIdForIndex(slot.slotIndex))
                {
                    auto& slotNode = nodes[static_cast<size_t>(static_cast<int>(*slotNodeId))];
                    slotNode.active = true;
                    slotNode.semanticId = slot.semanticId;
                    slotNode.slotIndex = slot.slotIndex;
                }
            }

            const auto routing = parameters::buildDynamicFxRoutingPlan(snapshot.dynamicFx, clampedSlotCount);
            std::array<ModuleId, parameters::ids::maxFxSlots> incoming{};
            incoming[0] = ModuleId::filter1;
            size_t incomingCount = 1;
            for (int s = 0; s < routing.count; ++s)
            {
                const auto& stage = routing.stages[static_cast<size_t>(s)];
                std::array<ModuleId, parameters::ids::maxFxSlots> outgoing{};
                std::array<bool, static_cast<size_t>(ModuleId::count)> used{};
                size_t outgoingCount = 0;
                const auto append = [&](ModuleId id) {
                    if (!used[static_cast<size_t>(id)] && outgoingCount < outgoing.size())
                    { used[static_cast<size_t>(id)] = true; outgoing[outgoingCount++] = id; }
                };
                for (int l = 0; l < stage.count; ++l)
                {
                    const auto& lane = stage.lanes[static_cast<size_t>(l)];
                    std::optional<ModuleId> previous;
                    for (int i = 0; i < lane.count; ++i)
                    {
                        const auto id = fxSlotModuleIdForIndex(lane.slots[static_cast<size_t>(i)]);
                        if (!id || !nodes[static_cast<size_t>(*id)].active) continue;
                        if (previous) addEdge(*previous, *id);
                        else for (size_t p = 0; p < incomingCount; ++p) addEdge(incoming[p], *id);
                        previous = id;
                    }
                    if (previous) append(*previous);
                    else for (size_t p = 0; p < incomingCount; ++p) append(incoming[p]);
                }
                incoming = outgoing;incomingCount = outgoingCount;
            }
            for (size_t i = 0; i < incomingCount; ++i) addEdge(incoming[i], ModuleId::masterOutput);
        }
        else
        {
            addEdge(ModuleId::filter1, ModuleId::masterOutput);
        }
    }
    else
    {
        addEdge(ModuleId::filter1, ModuleId::driveFx);
        addEdge(ModuleId::driveFx, ModuleId::chorusFx);
        addEdge(ModuleId::chorusFx, ModuleId::delayFx);
        addEdge(ModuleId::delayFx, ModuleId::reverbFx);
        addEdge(ModuleId::reverbFx, ModuleId::masterOutput);
    }

    std::array<bool, static_cast<size_t>(ModuleId::count)> dynamicInputs{};
    if (dynamicFxPopulated)
        for (size_t i = 0; i < flow.edgeCount; ++i)
            if (flow.edges[i].from == ModuleId::filter1)
                dynamicInputs[static_cast<size_t>(flow.edges[i].to)] = true;
    for (size_t i = 0; i < sourceNodes.size(); ++i)
        if (! filterWet || ! snapshot.filter.inputs[i] || snapshot.filter.mix < 1.0f)
        {
            if (dynamicFxPopulated)
                for (size_t n = 0; n < dynamicInputs.size(); ++n)
                    if (dynamicInputs[n]) addEdge(sourceNodes[i], static_cast<ModuleId>(n));
            if (!dynamicFxPopulated) addEdge(sourceNodes[i], postFilterNode);
        }

    // Modulation routing
    addEdge(ModuleId::lfo1, ModuleId::modMatrix);
    addEdge(ModuleId::lfo2, ModuleId::modMatrix);
    addEdge(ModuleId::lfo3, ModuleId::modMatrix);
    addEdge(ModuleId::lfo4, ModuleId::modMatrix);
    addEdge(ModuleId::ampEnvelope, ModuleId::modMatrix);
    addEdge(ModuleId::filterEnvelope, ModuleId::modMatrix);
    addEdge(ModuleId::modMatrix, ModuleId::osc1);
    addEdge(ModuleId::modMatrix, ModuleId::osc2);
    addEdge(ModuleId::modMatrix, ModuleId::filter1);

    // Budget
    if (sampleRate > 0.0 && blockSize > 0)
        flow.blockBudgetMicroseconds = static_cast<float>(
            static_cast<double>(blockSize) / sampleRate * 1'000'000.0);

    return flow;
}
} // namespace fadmodel1::dsp::diagnostics
