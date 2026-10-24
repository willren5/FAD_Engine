#pragma once

#include <array>

#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fadmodel1::dsp::diagnostics
{
struct TraceSourceValues
{
    float lfo1 = 0.0f;
    float lfo2 = 0.0f;
    float lfo3 = 0.0f;
    float lfo4 = 0.0f;
    float filterEnvelope = 0.0f;
    float ampEnvelope = 0.0f;
    float modEnvelope = 0.0f;
    float velocity = 0.0f;
    float modWheel = 0.0f;
    float aftertouch = 0.0f;
    std::array<float, 8> macros{};
};

struct ModSlotTrace
{
    parameters::ModSource source = parameters::ModSource::none;
    parameters::ModDestination destination = parameters::ModDestination::none;
    float configuredAmount = 0.0f;
    float resolvedSourceValue = 0.0f;
    float rawModulation = 0.0f;
    float scaledContribution = 0.0f;
    bool active = false;
};

struct DestinationTrace
{
    parameters::ModDestination destination = parameters::ModDestination::none;
    // Sum of per-slot scaled contributions before the destination clamp.
    float totalScaledContribution = 0.0f;
    int activeSlotCount = 0;
    std::array<int, 8> activeSlotIndices{};
};

struct ModulationTraceSnapshot
{
    std::array<ModSlotTrace, parameters::modMatrixSlotCount> slots{};
    std::array<DestinationTrace, 17> destinations{}; // globalPitch=16 → need 17 slots
    TraceSourceValues sourceValues{};
};
} // namespace fadmodel1::dsp::diagnostics
