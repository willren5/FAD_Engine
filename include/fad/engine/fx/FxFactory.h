#pragma once
#include <fad/engine/fx/BaseFxProcessor.h>
#include <fad/engine/fx/FxCatalog.h>
#include <memory>
namespace fadmodel1::dsp::fx
{
std::unique_ptr<BaseFxProcessorCore> createCoreFxProcessor(FxType);
}
