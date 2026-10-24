#pragma once
#include <algorithm>
#include <cmath>
namespace fadmodel1::dsp::fx
{
// Private cached controls 26..28 supplement the unchanged 0..25 host lanes.
inline float wideFxDecode(int index,float normalized) noexcept
{
    const auto n=std::clamp(normalized,0.f,1.f);
    const auto scale=index==26?.0001:.1, maximum=index==26?50.0:60000.0;
    if(n<=.02f) return -1.f+n/.02f;
    if(n>=1.f) return static_cast<float>(maximum);
    return static_cast<float>(scale*std::expm1(std::log1p(maximum/scale)*(n-.02f)/.98f));
}
inline float wideFxEncode(int index,float plain) noexcept
{
    const auto scale=index==26?.0001:.1, maximum=index==26?50.0:60000.0;
    const auto value=std::clamp(static_cast<double>(std::isfinite(plain)?plain:-1.f),-1.0,maximum);
    if(value<=0) return static_cast<float>((value+1)*.02f);
    if(value>=maximum) return 1;
    return .02f+static_cast<float>(.98f*std::log1p(value/scale)/std::log1p(maximum/scale));
}
}
