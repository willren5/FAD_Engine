#include <fad/engine/fx/FxFactory.h>
#include <fad/engine/fx/DriveFx.h>
#include <fad/engine/fx/BitcrusherFx.h>
#include <fad/engine/fx/CompressorFx.h>
#include <fad/engine/fx/Eq3BandFx.h>
#include <fad/engine/fx/LimiterFx.h>
#include <fad/engine/fx/PhaserFx.h>
#include <fad/engine/fx/FlangerFx.h>
#include <fad/engine/fx/DelayFx.h>
#include <fad/engine/fx/ReverbFx.h>
#include <fad/engine/fx/FilterFx.h>
#include <fad/engine/fx/StereoFx.h>
#include <fad/engine/fx/ChorusFx.h>
#include <fad/engine/fx/DistortionFx.h>
namespace fadmodel1::dsp::fx
{
std::unique_ptr<BaseFxProcessorCore> createCoreFxProcessor(FxType type)
{
    switch(type)
    {
        case FxType::drive: return std::make_unique<DriveFxCore>();
        case FxType::bitcrusher: return std::make_unique<BitcrusherFxCore>();
        case FxType::compressor: return std::make_unique<CompressorFxCore>();
        case FxType::eq3Band: return std::make_unique<Eq3BandFxCore>();
        case FxType::limiter: return std::make_unique<LimiterFxCore>();
        case FxType::phaser: return std::make_unique<PhaserFxCore>();
        case FxType::flanger: return std::make_unique<FlangerFxCore>();
        case FxType::delay: return std::make_unique<DelayFxCore>();
        case FxType::reverb: return std::make_unique<ReverbFxCore>();
        case FxType::filterFx: return std::make_unique<FilterFxCore>();
        case FxType::stereo: return std::make_unique<StereoFxCore>();
        case FxType::chorus: return std::make_unique<ChorusFxCore>();
        case FxType::distortion: return std::make_unique<DistortionFxCore>();
        case FxType::none: default: return nullptr;
    }
}
}
