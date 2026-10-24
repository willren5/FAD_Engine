#pragma once
#include "parameters/ParameterSnapshot.h"
#include "synth/OfflineSeed.h"
#include "synth/PerformanceCurve.h"

namespace fad::engine
{
// Source-level API. Runtime snapshot is not a preset/session serialization format.
using Snapshot = fadmodel1::parameters::ParameterSnapshot;
using CurveNodes = fadmodel1::parameters::CurveNodes;
}
