#pragma once

#include <algorithm>
#include <array>
#include <cmath>
namespace fadmodel1::parameters::serum
{
inline constexpr std::array<float, 101> measuredHz{
        8.0f, 9.0f, 10.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 17.0f, 18.0f, 19.0f, 21.0f, 23.0f, 25.0f, 27.0f, 29.0f, 31.0f, 34.0f, 37.0f, 40.0f, 43.0f, 47.0f, 50.0f, 54.0f, 59.0f, 64.0f, 69.0f, 75.0f, 81.0f, 88.0f, 95.0f, 103.0f, 111.0f, 120.0f, 130.0f, 141.0f, 152.0f, 165.0f, 178.0f, 193.0f, 209.0f, 226.0f, 245.0f, 264.0f, 286.0f, 310.0f, 335.0f, 363.0f, 393.0f, 425.0f, 460.0f, 498.0f, 539.0f, 583.0f, 631.0f, 683.0f, 739.0f, 800.0f, 866.0f, 937.0f, 1014.0f, 1097.0f, 1187.0f, 1284.0f, 1391.0f, 1504.0f, 1629.0f, 1763.0f, 1907.0f, 2065.0f, 2234.0f, 2418.0f, 2616.0f, 2832.0f, 3063.0f, 3316.0f, 3591.0f, 3884.0f, 4205.0f, 4548.0f, 4924.0f, 5327.0f, 5767.0f, 6238.0f, 6754.0f, 7305.0f, 7909.0f, 8563.0f, 9263.0f, 10028.0f, 10847.0f, 11744.0f, 12703.0f, 13753.0f, 14877.0f, 16107.0f, 17438.0f, 18863.0f, 20422.0f, 22050.0f};
inline float cutoffToHz(float raw) noexcept
{
    const auto position=std::clamp(std::isfinite(raw)?raw:0.0f, 0.0f, 1.0f)*100.0f;
    const auto index=std::min(99,static_cast<int>(position));
    const auto fraction=position-static_cast<float>(index);
    return measuredHz[static_cast<size_t>(index)]*std::pow(measuredHz[static_cast<size_t>(index+1)]/measuredHz[static_cast<size_t>(index)],fraction);
}
inline float cutoffFromHz(float hz) noexcept
{
    hz=std::clamp(std::isfinite(hz)?hz:measuredHz.front(), measuredHz.front(), measuredHz.back());
    const auto upper=std::upper_bound(measuredHz.begin(),measuredHz.end(),hz);
    if(upper==measuredHz.end())return 1.0f;
    const auto index=static_cast<size_t>(upper-measuredHz.begin()-1);
    return (static_cast<float>(index)+std::log(hz/measuredHz[index])/std::log(measuredHz[index+1]/measuredHz[index]))*.01f;
}
}
