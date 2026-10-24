#pragma once
#include <fad/engine/parameters/CurveNodes.h>
#include <fad/engine/assets/WavetableData.h>
#include <complex>
#include <algorithm>
#include <bit>

namespace fadmodel1::dsp::modulation
{
inline float curveSafe(float v,float fallback,float lo,float hi) noexcept
{ return std::isfinite(v)?std::clamp(v, lo, hi):fallback; }
inline parameters::CurveNodes modulatedCurve(parameters::CurveNodes curve,const std::array<float,8>& lfos) noexcept
{
    if(curve.historySource>0 && curve.historySource<=8)
        curve.historyPosition=curveSafe(curve.historyPosition+curveSafe(lfos[size_t(curve.historySource-1)],0,0,1)*curve.historyDepth,curve.historyPosition,0,1);
    if(curve.history && !curve.history->historyNodes.empty())
    {
        auto resolved=curve.history->historyCurve(curve.historyPosition);
        resolved.historySlot=curve.historySlot;resolved.historyPosition=curve.historyPosition;
        curve=resolved;
    }
    if(!curve.enabled)return curve;
    for(const auto& route:curve.mods)if(route.source>0 && route.source<=8 && route.target>=0 && route.target<48)
    {
        auto& point=curve.nodes[size_t(route.target/3)];const auto delta=curveSafe(lfos[size_t(route.source-1)],0,0,1)*curveSafe(route.amount,0,-1,1);
        if(route.target%3==0)point.x+=delta;
        else if(route.target%3==1)point.y+=delta;
        else point.c+=16*delta;
    }
    for(auto& p:curve.nodes){p.x=curveSafe(p.x,0,0,1);p.y=curveSafe(p.y,.5f,0,1);p.c=curveSafe(p.c,0,-8,8);}
    return curve;
}
// Fixed capacity, stable identities in parameter state; sorting never renumbers slots.
class CurveNodesRuntime
{
public:
    void prepare(const parameters::CurveNodes& source,bool periodic=false) noexcept
    {
        if(source.history && !source.history->historyNodes.empty())
        {prepare(source.history->historyCurve(source.historyPosition),periodic);return;}
        history=source.history && source.history->historyNodes.empty()?source.history:nullptr;
        historyPosition=source.historyPosition;
        historyFramePosition=history?history->historyFramePosition(historyPosition):0.f;
        loopStart=history?history->historyLoopStart(historyPosition):-1.f;
        count=0;
        if(source.enabled)for(auto p:source.nodes)if(p.enabled)
        {p.x=curveSafe(p.x,0,0,1);p.y=curveSafe(p.y,.5f,0,1);p.c=curveSafe(p.c,0,-8,8);nodes[count++]=p;}
        // Insertion sort is bounded and allocation-free, including equal positions.
        for(size_t i=1;i<count;++i){auto p=nodes[i];size_t j=i;while(j && nodes[j-1].x>p.x){nodes[j]=nodes[j-1];--j;}nodes[j]=p;}
        if(count==0){nodes[0]={0,.5f,0,true};nodes[1]={1,.5f,0,true};count=2;return;}
        if(nodes[0].x>0){for(size_t j=count;j>0;--j)nodes[j]=nodes[j-1];nodes[0]={0,nodes[1].y,0,true};++count;}
        if(nodes[count-1].x<1){nodes[count]={1,periodic?nodes[0].y:nodes[count-1].y,0,true};++count;}
        if(count==1){nodes[count]={1,nodes[0].y,0,true};++count;}
    }
    float sample(double x) const noexcept
    {
        if(history)return history->sampleHistoryFrame(historyFramePosition,x);
        x=std::isfinite(x)?std::clamp(x, 0., 1.):0.;
        if(x>=1){size_t endpoint=0;while(endpoint+1<count && nodes[endpoint].x<1)++endpoint;return nodes[endpoint].y;}
        size_t left=0;while(left+1<count && nodes[left+1].x<=x)++left;
        if(left+1>=count)return nodes[left].y;
        const auto& a=nodes[left];const auto& b=nodes[left+1];const double width=b.x-a.x;
        const double u=width>1.e-9?std::clamp((x-a.x)/width, 0., 1.):1.;
        const double c=a.c,shape=std::abs(c)<1.e-5?u:std::expm1(c*u)/std::expm1(c);
        return curveSafe(float(a.y+(b.y-a.y)*shape),.5f,0,1);
    }
    std::array<parameters::CurveNode,parameters::curveNodeCount+2> nodes{};
    size_t count=2;
    const wavetable::WavetableData* history=nullptr;
    float historyPosition=1.f,historyFramePosition=0.f,loopStart=-1.f;
};
// An analytic Fourier representation avoids generating wavetables or allocating
// on the render thread. Only harmonics below Nyquist are sampled. DC is omitted.
class BandLimitedNodeCurve final
{
public:
    static constexpr size_t harmonics=32;
    void reset() noexcept { current.fill({});target.fill({});mix=targetMix=0;valid=false;history=nullptr; }
    void prepare(const parameters::CurveNodes& curve,double sampleRate) noexcept
    {
        alpha=float(1.-std::exp(-1./(std::max(8000.,sampleRate)*.005)));
        targetMix=curve.enabled?1.f:0.f;
        const auto* next=curve.history && curve.history->historyNodes.empty()?curve.history:nullptr;
        if(next){targetHistoryPosition=next->historyFramePosition(curve.historyPosition);if(history!=next)historyPosition=targetHistoryPosition;history=next;return;}
        if(history){history=nullptr;valid=false;}
        bool changed=!valid;
        for(size_t i=0;i<curve.nodes.size();++i)
        {const auto& a=curve.nodes[i];const auto& b=previous.nodes[i];changed=changed||a.enabled!=b.enabled||std::bit_cast<uint32_t>(a.x)!=std::bit_cast<uint32_t>(b.x)||std::bit_cast<uint32_t>(a.y)!=std::bit_cast<uint32_t>(b.y)||std::bit_cast<uint32_t>(a.c)!=std::bit_cast<uint32_t>(b.c);}
        if(!curve.enabled || !changed)return;
        previous=curve;valid=true;target.fill({});CurveNodesRuntime runtime;runtime.prepare(curve,true);
        constexpr double tau=6.2831853071795864769;
        const std::complex<double> imaginary{0,1};
        for(size_t segment=0;segment+1<runtime.count;++segment)
        {
            const auto& a=runtime.nodes[segment];const auto& b=runtime.nodes[segment+1];const double width=b.x-a.x;
            if(width<1.e-8)continue;
            const double ya=2*a.y-1,yb=2*b.y-1,c=a.c;
            const auto ra=std::polar(1.,-tau*a.x),rb=std::polar(1.,-tau*b.x);
            std::complex<double> za{1,0},zb{1,0};
            for(size_t n=1;n<=harmonics;++n)
            {
                za*=ra;zb*=rb;const double w=tau*double(n);const auto iw=imaginary*w;
                std::complex<double> integral;
                if(std::abs(c)<1.e-5)
                    integral=ya*(za-zb)/iw+(yb-ya)/width*(-width*zb/iw+(za-zb)/(iw*iw));
                else
                {const double ec=std::exp(c),beta=(yb-ya)/(ec-1),base=ya-beta;
                 integral=base*(za-zb)/iw+beta*(ec*zb-za)/(c/width-iw);}
                target[n-1]+=std::complex<float>{float(2*integral.real()),float(-2*integral.imag())};
            }
        }
    }
    void advance() noexcept
    {mix+=alpha*(targetMix-mix);if(mix<1.e-6f && targetMix<.5f)mix=0;if(history){historyPosition+=alpha*(targetHistoryPosition-historyPosition);return;}for(size_t i=0;i<harmonics;++i)current[i]+=alpha*(target[i]-current[i]);}
    float sample(float phase,float delta) const noexcept
    {
        if(history)return history->sampleAtPositionBandLimited(historyPosition,phase,delta);
        if(!std::isfinite(phase))return 0;
        const double angle=6.2831853071795864769*(phase-std::floor(phase));
        const double ca=std::cos(angle),sa=std::sin(angle);double cn=1,sn=0,value=0;
        const auto limit=size_t(std::clamp(int(.49/std::max(1.e-6f,std::abs(delta))), 0, 32));
        for(size_t i=0;i<limit;++i){const double next=cn*ca-sn*sa;sn=sn*ca+cn*sa;cn=next;value+=current[i].real()*cn+current[i].imag()*sn;}
        return std::isfinite(value)?float(value):0.f;
    }
    float mix=0;
    const wavetable::WavetableData* history=nullptr;
private:
    float historyPosition=1.f,targetHistoryPosition=1.f;
    std::array<std::complex<float>,harmonics> current{},target{};
    parameters::CurveNodes previous{};float alpha=.004f,targetMix=0;bool valid=false;
};
}
