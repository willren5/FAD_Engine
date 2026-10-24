#pragma once

#include "ModulationTrace.h"
#include "SignalFlowGraph.h"

namespace fadmodel1::dsp::diagnostics
{
struct DiagnosticsSnapshot
{
    ModulationTraceSnapshot modulationTrace{};
    SignalFlowSnapshot signalFlow{};
    bool enabled = false;
};
} // namespace fadmodel1::dsp::diagnostics
