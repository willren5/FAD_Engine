#pragma once
#include <fad/engine/modulation/CurveNodesRuntime.h>
#include <bit>

namespace fadmodel1::dsp::filters
{
class CurveNodeFilter final
{
public:
    void reset() noexcept
    {
        for(auto& band:bands)band=Band{};
        active=false;valid=false;
    }
    void prepare(const parameters::CurveNodes& source,double sampleRate) noexcept
    {
        if(!source.enabled && !active)return;
        const double fs=std::isfinite(sampleRate)?std::max(8000.,sampleRate):48000.;
        auto curve=source;
        for(auto& p:curve.nodes)
        {
            p.x=modulation::curveSafe(p.x,0,0,1);
            p.y=modulation::curveSafe(p.y,.5f,0,1);
            p.c=modulation::curveSafe(p.c,0,-8,8);
        }
        bool changed=!valid || !same(fs,preparedRate) || curve.enabled!=previous.enabled;
        for(size_t i=0;i<bands.size();++i)
        {
            const auto& a=curve.nodes[i];const auto& b=previous.nodes[i];
            changed=changed || a.enabled!=b.enabled || !same(a.x,b.x) || !same(a.y,b.y) || !same(a.c,b.c);
        }
        if(!changed)return;
        valid=true;previous=curve;preparedRate=fs;
        alpha=1.-std::exp(-1./(fs*.005));
        std::array<Coefficients,parameters::curveNodeCount> targets{};
        for(size_t i=0;i<bands.size();++i)
            targets[i]=coefficients(curve.nodes[i],fs,1.,curve.enabled);
        // Limit the sampled actual response, so separated boosts retain their
        // gain. Sum only positive bands: a cut cannot hide excess amplification.
        double maximum=1.;
        const auto inspect=[&](double hz) noexcept
        {
            const auto w=6.283185307179586*std::min(fs*.49,hz)/fs;
            const auto c=std::cos(w),s=std::sin(w),c2=c*c-s*s,s2=2*c*s;
            double product=1.;
            for(size_t i=0;i<bands.size();++i)
            {
                const auto& p=curve.nodes[i];if(!curve.enabled || !p.enabled || p.y<=.5f)continue;
                const auto& a=targets[i];
                const auto nr=a[0]+a[1]*c+a[2]*c2,ni=-a[1]*s-a[2]*s2;
                const auto dr=1.+a[3]*c+a[4]*c2,di=-a[3]*s-a[4]*s2;
                product*=std::max(1.e-24,(nr*nr+ni*ni)/std::max(1.e-24,dr*dr+di*di));
            }
            maximum=std::max(maximum,product);
        };
        if(curve.enabled)
        {
            for(int i=0;i<32;++i)inspect(20.*std::pow(1000.,double(i)/31));
            for(const auto& p:curve.nodes)if(p.enabled && p.y>.5f)inspect(20.*std::pow(1000.,p.x));
        }
        const auto peakDb=10.*std::log10(maximum);
        const auto scale=peakDb>18.?18./peakDb:1.;
        for(size_t i=0;i<bands.size();++i)
        {
            auto& b=bands[i];const auto& p=curve.nodes[i];
            b.enabled=curve.enabled && p.enabled && std::abs(p.y-.5f)>1.e-7f;
            b.target=scale<1.?coefficients(p,fs,scale,curve.enabled):targets[i];
            active=active || b.enabled;
        }
    }
    // Settled target response for an exclusively owned UI/test renderer.
    double getStaticMagnitudeForFrequency(double hz) const noexcept
    {
        if(!valid)return 1.;
        const auto w=6.283185307179586*std::clamp(hz,0.,preparedRate*.49)/preparedRate;
        const auto c=std::cos(w),s=std::sin(w),c2=std::cos(2*w),s2=std::sin(2*w);
        double power=1.;
        for(const auto& band:bands)
        {
            const auto& a=band.target;
            const auto nr=a[0]+a[1]*c+a[2]*c2,ni=-a[1]*s-a[2]*s2;
            const auto dr=1.+a[3]*c+a[4]*c2,di=-a[3]*s-a[4]*s2;
            power*=std::max(1.e-24,(nr*nr+ni*ni)/std::max(1.e-24,dr*dr+di*di));
        }
        return std::sqrt(power);
    }
    float process(float input) noexcept
    {
        if(!active)return input;
        bool settled=true;double sample=input;
        for(auto& b:bands)
        {
            bool identity=!b.enabled;
            for(size_t j=0;j<5;++j)
            {
                b.current[j]+=alpha*(b.target[j]-b.current[j]);
                const auto difference=std::abs(b.current[j]-b.target[j]);
                if(difference>1.e-12)settled=false;
                identity=identity && difference<1.e-12;
            }
            if(identity){b.z1=b.z2=0;continue;}
            const auto& c=b.current;const auto output=c[0]*sample+b.z1;
            b.z1=c[1]*sample-c[3]*output+b.z2;b.z2=c[2]*sample-c[4]*output;
            if(!std::isfinite(output) || !std::isfinite(b.z1) || !std::isfinite(b.z2)){b.z1=b.z2=0;sample=0;}
            else{sample=output;if(std::abs(b.z1)<1.e-20)b.z1=0;if(std::abs(b.z2)<1.e-20)b.z2=0;}
        }
        if(settled && std::none_of(bands.begin(),bands.end(),[](const Band& b){return b.enabled;}))active=false;
        return std::isfinite(sample)?float(sample):0.f;
    }
private:
    template<class T> static bool same(T a,T b) noexcept
    {
        if constexpr(sizeof(T)==4)return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);
        else return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);
    }
    using Coefficients=std::array<double,5>;
    static Coefficients coefficients(const parameters::CurveNode& p,double fs,double scale,bool enabled) noexcept
    {
        if(!enabled || !p.enabled || std::abs(p.y-.5f)<1.e-7f)return {1,0,0,0,0};
        const double gain=48.*p.y-24.,db=gain>0?gain*scale:gain;
        const auto A=std::pow(10.,db/40.),w=6.283185307179586*std::min(fs*.45,20.*std::pow(1000.,p.x))/fs;
        const auto q=std::clamp(std::exp(double(p.c)*.35), .25, 8.),cosw=std::cos(w),a=std::sin(w)/(2*q),den=1+a/A;
        return {(1+a*A)/den,-2*cosw/den,(1-a*A)/den,-2*cosw/den,(1-a/A)/den};
    }
    struct Band
    {
        Coefficients current{1,0,0,0,0},target{1,0,0,0,0};
        double z1=0,z2=0;bool enabled=false;
    };
    std::array<Band,parameters::curveNodeCount> bands{};
    parameters::CurveNodes previous{};double alpha=.004,preparedRate=0;bool active=false,valid=false;
};
}
