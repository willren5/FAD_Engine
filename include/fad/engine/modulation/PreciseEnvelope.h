#pragma once
#include "LinearEnvelope.h"
#include <cmath>

namespace fadmodel1::dsp::modulation
{
// Legacy times keep JUCE's exact float path. Long stages use double accumulation
// so a small increment cannot stall near the peak at high sample rates.
class PreciseEnvelope final
{
public:
    void setSampleRate(double rate) noexcept { sampleRate=rate; legacy.setSampleRate(rate); }
    void setParameters(const fad::engine::EnvelopeParameters& next) noexcept {
        const bool needsPrecision=next.attack>32 || next.decay>32 || next.release>32;
        if(stage==Stage::release && (precise || needsPrecision)
            && (!precise || next.release!=parameters.release))
            releaseStep=next.release>0?value/(next.release*sampleRate):value;
        parameters=next;
        if(needsPrecision) precise=true;
        legacy.setParameters(next);
    }
    const fad::engine::EnvelopeParameters& getParameters() const noexcept { return parameters; }
    void reset() noexcept { legacy.reset(); value=0; stage=Stage::idle; precise=parameters.attack>32 || parameters.decay>32 || parameters.release>32; }
    void noteOn() noexcept {
        if(!precise) { legacy.noteOn(); stage=parameters.attack>0?Stage::attack:Stage::decay; return; }
        stage=parameters.attack>0?Stage::attack:Stage::decay;
        if(parameters.attack<=0)value=1;
    }
    void noteOff() noexcept {
        if(!precise) {legacy.noteOff();stage=legacy.isActive()?Stage::release:Stage::idle;return;}
        if(stage==Stage::idle)return;
        if(parameters.release<=0){value=0;stage=Stage::idle;return;}
        releaseStep=value/(parameters.release*sampleRate);stage=Stage::release;
    }
    bool isActive() const noexcept { return precise?stage!=Stage::idle:legacy.isActive(); }
    float getNextSample() noexcept {
        if(!precise) {
            auto result=legacy.getNextSample();value=result;
            if(!legacy.isActive())stage=Stage::idle;
            else if(stage==Stage::attack && result>=1)stage=Stage::decay;
            else if(stage==Stage::decay && result<=parameters.sustain)stage=Stage::sustain;
            return result;
        }
        return getNextPreciseSample();
    }
private:
    // Keep the long-stage arithmetic out of the common JUCE ADSR sample path.
    float getNextPreciseSample() noexcept {
        switch(stage) {
            case Stage::idle:value=0;break;
            case Stage::attack:
                value+=parameters.attack>0?1.0/(parameters.attack*sampleRate):1;
                if(value>=1){value=1;stage=Stage::decay;}break;
            case Stage::decay:
                value-=parameters.decay>0?(1.0-parameters.sustain)/(parameters.decay*sampleRate):1;
                if(value<=parameters.sustain || parameters.decay<=0){value=parameters.sustain;stage=Stage::sustain;}break;
            case Stage::sustain:value=parameters.sustain;break;
            case Stage::release:
                value-=releaseStep;
                if(value<=0){value=0;stage=Stage::idle;}break;
        }
        return static_cast<float>(value);
    }
    enum class Stage {idle,attack,decay,sustain,release};
    fad::engine::LinearEnvelope legacy;
    fad::engine::EnvelopeParameters parameters;
    double sampleRate=44100, value=0, releaseStep=0;
    Stage stage=Stage::idle;
    bool precise=false;
};
}
