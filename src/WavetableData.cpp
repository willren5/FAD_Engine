#include <fad/engine/assets/WavetableData.h>
#include <fad/engine/assets/PreparationFft.h>
#include <numbers>
#include <fad/engine/math/DspSafetyUtils.h>

#include <cmath>

namespace fadmodel1::dsp::wavetable
{
namespace
{
// Blend between two frames at given phase
float sampleBlended(const float* frameA, const float* frameB,
                    size_t frameSize, float frameFrac, float phase) noexcept
{
    if (frameSize == 0)
        return 0.0f;

    const float safePhase = fadmodel1::dsp::utils::sanitizeUnitInterval(phase);
    const float index = safePhase * static_cast<float>(frameSize);
    const auto baseIndex = static_cast<size_t>(index);
    const auto indexMask = frameSize - 1u;
    const auto sampleA = baseIndex & indexMask;
    const auto sampleB = (sampleA + 1u) & indexMask;
    const float sampleFrac = index - static_cast<float>(baseIndex);
    const auto sampleFrame = [sampleA, sampleB, sampleFrac](const float* frame) noexcept
    {
        return frame != nullptr
            ? frame[sampleA] + sampleFrac * (frame[sampleB] - frame[sampleA])
            : 0.0f;
    };

    const float a = sampleFrame(frameA);
    const float b = sampleFrame(frameB);
    return a + frameFrac * (b - a);
}
}

bool WavetableData::isSupportedFrameSize(size_t size) noexcept
{
    return size >= 256
        && size <= 4096
        && (size & (size - 1u)) == 0u;
}

bool WavetableData::hasRenderableLayout() const noexcept
{
    if (frameCount == 0
        || ! isSupportedFrameSize(frameSize)
        || frames.size() < frameCount)
        return false;

    for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        if (frames[frameIndex].size() != frameSize)
            return false;

    return true;
}

bool WavetableData::isValid() const noexcept
{
    if (!validHistory()) return false;
    if (!validPhaseRemap(phaseRemap) || (!phaseRemap.empty() && sampleLength != 0)) return false;
    if (!nativeCurves.isValid(frameCount)
        || (!nativeCurves.empty() && (sampleLength != 0 || curveEndpoints.size() != frameCount))) return false;
    if (discreteFrames && (sampleLength != 0 || !curveEndpoints.empty() || !curveLoopStarts.empty() || !nativeCurves.empty()))
        return false;
    if ((sampleLength == 0) != (sampleRate == 0.0)
        || (sampleChannels != 1 && sampleChannels != 2)
        || (sampleLength == 0 && sampleChannels != 1)
        || frameCount > (sampleLength > 0 ? maxSampleFrames : maxFrames)
        || sampleLength > frameCount * frameSize / static_cast<size_t>(sampleChannels)
        || ! std::isfinite(sampleRate)
        || (sampleLength > 0 && (sampleRate < 8000.0 || sampleRate > 384000.0)))
        return false;
    if (! curveLoopStarts.empty()
        && (curveLoopStarts.size() != frameCount || sampleLength != 0
            || std::any_of(curveLoopStarts.begin(), curveLoopStarts.end(),
                [](float value) { return !std::isfinite(value) || (value != -1.0f && (value < 0.0f || value >= 1.0f)); })))
        return false;
    if (! curveEndpoints.empty()
        && (curveEndpoints.size() != frameCount || sampleLength != 0
            || std::any_of(curveEndpoints.begin(), curveEndpoints.end(),
                           [](float value) { return ! std::isfinite(value); })))
        return false;
    if (! hasRenderableLayout())
        return false;

    for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
    {
        const auto& frame = frames[frameIndex];
        for (const auto sample : frame)
            if (! std::isfinite(sample) || (!historyTimes.empty() && historyNodes.empty()
                && (sample < (historyOwner<2?-1.f:0.f) || sample>1.f)))
                return false;
    }

    return true;
}

bool WavetableData::validHistory() const noexcept
{
    if(historyTimes.empty())return historyOwner==-1 && historyNodes.empty();
    if(historyOwner<0 || historyOwner>=int(parameters::curveOwnerCount) || sampleLength || discreteFrames
        || historyTimes.size()!=frameCount || frameCount<2 || frameCount>maxFrames
        || historyTimes.front()!=0 || historyTimes.back()>30000.f || !nativeCurves.empty())return false;
    float previous=-1;
    for(auto at:historyTimes){if(!std::isfinite(at) || at<=previous)return false;previous=at;}
    if(historyNodes.empty())return historyOwner<10 && curveEndpoints.size()==frameCount;
    if(historyNodes.size()!=frameCount)return false;
    for(const auto& row:historyNodes)
    {
        for(auto v:row)if(!std::isfinite(v))return false;
        if(row[0]!=0 && row[0]!=1)return false;
        for(size_t n=0;n<16;++n){const auto f=1+n*4;
            if((row[f]!=0 && row[f]!=1) || row[f+1]<0 || row[f+1]>1 || row[f+2]<0 || row[f+2]>1 || std::abs(row[f+3])>8)return false;}
        for(size_t m=0;m<8;++m){const auto f=65+m*3;
            if(row[f]<0 || row[f]>47 || row[f]!=std::floor(row[f]) || row[f+1]<0 || row[f+1]>8 || row[f+1]!=std::floor(row[f+1]) || std::abs(row[f+2])>1)return false;}
    }
    return true;
}

float WavetableData::historyFramePosition(float position) const noexcept
{
    if(historyTimes.size()<2)return 0;
    const auto at=utils::sanitizeUnitInterval(position)*historyTimes.back();
    auto next=std::upper_bound(historyTimes.begin(),historyTimes.end(),at);
    if(next==historyTimes.end())return 1;
    const auto b=size_t(next-historyTimes.begin()),a=b?b-1:0;
    const auto fraction=b>a?(at-historyTimes[a])/(historyTimes[b]-historyTimes[a]):0.f;
    return (float(a)+fraction)/float(historyTimes.size()-1);
}

float WavetableData::sampleHistory(float position,double phase) const noexcept
{
    return sampleHistoryFrame(historyFramePosition(position),phase);
}
float WavetableData::sampleHistoryFrame(float framePosition,double phase) const noexcept
{
    const auto p=utils::sanitizeUnitInterval(framePosition)*float(frameCount-1);
    const auto a=std::min(size_t(p),frameCount-1),b=std::min(a+1,frameCount-1);
    const auto x=utils::sanitizeUnitInterval(float(phase))*float(frameSize);
    const auto i=std::min(size_t(x),frameSize),j=std::min(i+1,frameSize);
    const auto sample=[&](size_t f){const auto y=[&](size_t k){return k==frameSize?curveEndpoints[f]:frames[f][k];};return y(i)+(y(j)-y(i))*(x-float(i));};
    return sample(a)+(sample(b)-sample(a))*(p-float(a));
}

float WavetableData::historyLoopStart(float position) const noexcept
{
    const auto frame=std::min(size_t(historyFramePosition(position)*float(frameCount-1)),frameCount-1);
    return curveLoopStart(frame);
}

parameters::CurveNodes WavetableData::historyCurve(float position) const noexcept
{
    parameters::CurveNodes result;
    if(historyNodes.empty())return result;
    const auto p=historyFramePosition(position)*float(frameCount-1);
    const auto a=std::min(size_t(p),frameCount-1),b=std::min(a+1,frameCount-1);const auto u=p-float(a);
    const auto& left=historyNodes[a];const auto& right=historyNodes[b];
    result.enabled=left[0]>.5f;
    for(size_t n=0;n<16;++n){auto& node=result.nodes[n];const auto f=1+n*4;
        node.enabled=left[f]>.5f;node.x=left[f+1]+(right[f+1]-left[f+1])*u;
        node.y=left[f+2]+(right[f+2]-left[f+2])*u;node.c=left[f+3]+(right[f+3]-left[f+3])*u;}
    for(size_t m=0;m<8;++m){const auto f=65+m*3;result.mods[m]={int(left[f]),int(left[f+1]),left[f+2]+(right[f+2]-left[f+2])*u};}
    return result;
}

float WavetableData::sampleAtPosition(float position, float phase) const noexcept
{
    if (frameCount == 0 || frameSize == 0 || frames.size() < frameCount)
        return 0.0f;

    const float safePosition = fadmodel1::dsp::utils::sanitizeUnitInterval(position);
    const float safePhase = fadmodel1::dsp::utils::sanitizeUnitInterval(phase);
    const float scaledPos = safePosition * static_cast<float>(discreteFrames ? frameCount : frameCount - 1);
    const auto frameA = std::min(static_cast<size_t>(scaledPos), frameCount - 1);
    const auto frameB = discreteFrames ? frameA : std::min(frameA + 1, frameCount - 1);
    const float frameFrac = discreteFrames ? 0.0f : scaledPos - static_cast<float>(frameA);
    if (frames[frameA].size() < frameSize || frames[frameB].size() < frameSize)
        return 0.0f;

    const float index = safePhase * static_cast<float>(frameSize);
    const auto baseIndex = static_cast<size_t>(index);
    const auto sampleA = baseIndex % frameSize;
    const auto sampleB = (sampleA + 1) % frameSize;
    const float sampleFrac = index - static_cast<float>(baseIndex);

    const float valA = fad::engine::math::interpolate(sampleFrac,
                                   frames[frameA][sampleA],
                                   frames[frameA][sampleB]);
    const float valB = fad::engine::math::interpolate(sampleFrac,
                                   frames[frameB][sampleA],
                                   frames[frameB][sampleB]);

    return fad::engine::math::interpolate(frameFrac, valA, valB);
}

float WavetableData::sampleAtPositionBandLimited(float position, float phase,
                                                  float phaseDelta) const noexcept
{
    return sampleAtPositionBandLimitedWithLevel(
        position, phase, calculateIdealMipLevel(frameSize, phaseDelta));
}

float WavetableData::calculateIdealMipLevel(size_t sourceFrameSize, float phaseDelta) noexcept
{
    const auto safePhaseDelta = std::isfinite(phaseDelta) && phaseDelta > 0.0f
        ? phaseDelta
        : 0.0f;
    if (sourceFrameSize == 0 || safePhaseDelta <= 0.0f)
        return 0.0f;

    const auto levelInput = static_cast<float>(sourceFrameSize) * safePhaseDelta;
    // The lower of the two crossfaded levels must already fit below Nyquist.
    // Without this offset the lower level retains out-of-band harmonics.
    return levelInput > 0.0f && std::isfinite(levelInput) ? std::log2(levelInput) + 1.0f : 0.0f;
}

bool WavetableData::prepareBandLimitedSample(
    float position,
    float idealMipLevel,
    PreparedBandLimitedSample& prepared) const noexcept
{
    prepared = {};
    if (frameCount == 0
        || frameSize == 0
        || (frameSize & (frameSize - 1u)) != 0u
        || frames.size() < frameCount)
        return false;

    const float safePosition = fadmodel1::dsp::utils::sanitizeUnitInterval(position);
    const float scaledPosition = safePosition * static_cast<float>(discreteFrames ? frameCount : frameCount - 1);
    const auto frameA = std::min(static_cast<size_t>(scaledPosition), frameCount - 1);
    const auto frameB = discreteFrames ? frameA : std::min(frameA + 1, frameCount - 1);
    prepared.frameFraction = discreteFrames ? 0.0f : scaledPosition - static_cast<float>(frameA);

    const auto resolveLevel = [&](int logicalLevel,
                                  const float*& resolvedFrameA,
                                  const float*& resolvedFrameB,
                                  size_t& resolvedFrameSize) noexcept
    {
        if (logicalLevel <= 0)
        {
            if (frames[frameA].size() < frameSize || frames[frameB].size() < frameSize)
                return false;

            resolvedFrameA = frames[frameA].data();
            resolvedFrameB = frames[frameB].data();
            resolvedFrameSize = frameSize;
            return true;
        }

        const auto storageIndex = static_cast<size_t>(logicalLevel - 1);
        if (storageIndex >= mipLevels.size() || storageIndex >= mipFrameSizes.size())
            return false;

        const auto resolvedSize = mipFrameSizes[storageIndex];
        if (resolvedSize == 0 || (resolvedSize & (resolvedSize - 1u)) != 0u)
            return false;

        const auto requiredSamples = frameCount * resolvedSize;
        const auto& mipData = mipLevels[storageIndex];
        if (mipData.size() < requiredSamples)
            return false;

        resolvedFrameA = mipData.data() + frameA * resolvedSize;
        resolvedFrameB = mipData.data() + frameB * resolvedSize;
        resolvedFrameSize = resolvedSize;
        return true;
    };

    const auto resolveBaseLevel = [&]() noexcept
    {
        prepared.levelBFrameA = nullptr;
        prepared.levelBFrameB = nullptr;
        prepared.levelBFrameSize = 0;
        prepared.levelFraction = 0.0f;
        return resolveLevel(0,
                            prepared.levelAFrameA,
                            prepared.levelAFrameB,
                            prepared.levelAFrameSize);
    };

    if (mipLevelCount <= 1 || mipLevels.empty() || mipFrameSizes.size() != mipLevels.size())
        return resolveBaseLevel();

    const auto safeIdealLevel = std::isfinite(idealMipLevel) ? idealMipLevel : 0.0f;
    const float clampedLevel = std::clamp(safeIdealLevel, 0.0f, static_cast<float>(mipLevelCount - 1));
    const auto levelA = static_cast<int>(clampedLevel);
    const auto levelB = std::min(levelA + 1, mipLevelCount - 1);
    prepared.levelFraction = clampedLevel - static_cast<float>(levelA);

    if (! resolveLevel(levelA,
                       prepared.levelAFrameA,
                       prepared.levelAFrameB,
                       prepared.levelAFrameSize))
        return resolveBaseLevel();

    if (levelA == levelB || prepared.levelFraction < 0.001f)
    {
        prepared.levelFraction = 0.0f;
        return true;
    }

    if (! resolveLevel(levelB,
                       prepared.levelBFrameA,
                       prepared.levelBFrameB,
                       prepared.levelBFrameSize))
        return resolveBaseLevel();

    return true;
}

float WavetableData::samplePreparedBandLimited(
    const PreparedBandLimitedSample& prepared,
    float phase) noexcept
{
    if (prepared.levelAFrameA == nullptr
        || prepared.levelAFrameB == nullptr
        || prepared.levelAFrameSize == 0)
        return 0.0f;

    const auto valueA = sampleBlended(prepared.levelAFrameA,
                                      prepared.levelAFrameB,
                                      prepared.levelAFrameSize,
                                      prepared.frameFraction,
                                      phase);
    if (prepared.levelBFrameA == nullptr
        || prepared.levelBFrameB == nullptr
        || prepared.levelBFrameSize == 0
        || prepared.levelFraction < 0.001f)
        return valueA;

    const auto valueB = sampleBlended(prepared.levelBFrameA,
                                      prepared.levelBFrameB,
                                      prepared.levelBFrameSize,
                                      prepared.frameFraction,
                                      phase);
    return valueA + prepared.levelFraction * (valueB - valueA);
}

float WavetableData::sampleAtPositionBandLimitedWithLevel(float position,
                                                          float phase,
                                                          float idealMipLevel) const noexcept
{
    if (frameCount == 0
        || frameSize == 0
        || (frameSize & (frameSize - 1u)) != 0u
        || frames.size() < frameCount)
        return 0.0f;

    // If no reduced mipmap is available, fall back to the base frames.
    if (mipLevelCount <= 1 || mipLevels.empty() || mipFrameSizes.size() != mipLevels.size())
        return sampleAtPosition(position, phase);

    const auto safeIdealLevel = std::isfinite(idealMipLevel) ? idealMipLevel : 0.0f;
    const float clampedLevel = std::clamp(safeIdealLevel, 0.0f, static_cast<float>(mipLevelCount - 1));

    const auto levelA = static_cast<int>(clampedLevel);
    const auto levelB = std::min(levelA + 1, mipLevelCount - 1);
    const float levelFrac = clampedLevel - static_cast<float>(levelA);

    // Resolve frame position
    const float safePosition = fadmodel1::dsp::utils::sanitizeUnitInterval(position);
    const float scaledPos = safePosition * static_cast<float>(discreteFrames ? frameCount : frameCount - 1);
    const auto fA = std::min(static_cast<size_t>(scaledPos), frameCount - 1);
    const auto fB = discreteFrames ? fA : std::min(fA + 1, frameCount - 1);
    const float frameFrac = discreteFrames ? 0.0f : scaledPos - static_cast<float>(fA);
    const auto sampleLogicalLevel = [&](int logicalLevel, float& result) noexcept
    {
        if (logicalLevel <= 0)
        {
            if (frames[fA].size() < frameSize || frames[fB].size() < frameSize)
                return false;

            result = sampleBlended(frames[fA].data(),
                                   frames[fB].data(),
                                   frameSize,
                                   frameFrac,
                                   phase);
            return true;
        }

        const auto storageIndex = static_cast<size_t>(logicalLevel - 1);
        if (storageIndex >= mipLevels.size() || storageIndex >= mipFrameSizes.size())
            return false;

        const auto levelFrameSize = mipFrameSizes[storageIndex];
        if (levelFrameSize == 0 || (levelFrameSize & (levelFrameSize - 1u)) != 0u)
            return false;

        const auto requiredMipSamples = frameCount * levelFrameSize;
        const auto& mipData = mipLevels[storageIndex];
        if (mipData.size() < requiredMipSamples)
            return false;

        result = sampleBlended(mipData.data() + fA * levelFrameSize,
                               mipData.data() + fB * levelFrameSize,
                               levelFrameSize,
                               frameFrac,
                               phase);
        return true;
    };

    float valA = 0.0f;
    if (! sampleLogicalLevel(levelA, valA))
        return sampleAtPosition(position, phase);

    if (levelA == levelB || levelFrac < 0.001f)
        return valA;

    float valB = 0.0f;
    if (! sampleLogicalLevel(levelB, valB))
        return sampleAtPosition(position, phase);

    return valA + levelFrac * (valB - valA);
}

void WavetableData::buildMipmap()
{
    mipLevels.clear(); mipFrameSizes.clear(); mipLevelCount = 0;
    if (!isValid() || frameSize < 4) return;
    mipLevelCount = std::min(maxMipLevels, static_cast<int>(std::log2(static_cast<float>(frameSize))));
    if (mipLevelCount <= 1) return;
    mipLevels.resize(static_cast<size_t>(mipLevelCount - 1));
    mipFrameSizes.resize(static_cast<size_t>(mipLevelCount - 1));
    fad::engine::assets::PreparationFft fft(frameSize);
    std::vector<std::complex<double>> spectrum(frameSize), scratch(frameSize);
    for (int level = 1; level < mipLevelCount; ++level)
    {
        const auto index = static_cast<size_t>(level - 1);
        const auto count = std::max(size_t{256}, frameSize >> static_cast<size_t>(level));
        mipFrameSizes[index] = count;
        mipLevels[index].resize(frameCount * count, 0.0f);
    }
    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        for (size_t i = 0; i < frameSize; ++i) spectrum[i] = {frames[frame][i], 0.0};
        fft.transform(spectrum, false);
        for (int level = 1; level < mipLevelCount; ++level)
        {
            scratch = spectrum;
            const auto maximumHarmonic = (frameSize / 2) >> static_cast<size_t>(level);
            for (size_t bin = maximumHarmonic + 1; bin < frameSize - maximumHarmonic; ++bin)
                scratch[bin] = {};
            fft.transform(scratch, true);
            const auto index = static_cast<size_t>(level - 1), count = mipFrameSizes[index];
            auto* destination = mipLevels[index].data() + frame * count;
            for (size_t i = 0; i < count; ++i)
                destination[i] = static_cast<float>(scratch[i * (frameSize / count)].real());
        }
    }
}

WavetableData createBuiltinWavetable()
{
    WavetableData data;
    data.frameSize = 2048;
    data.frameCount = 4;
    data.frames.resize(4);

    constexpr int maxHarmonics = 512; // Much more than the 32 we had before

    for (size_t frameIndex = 0; frameIndex < 4; ++frameIndex)
    {
        data.frames[frameIndex].resize(2048);
        for (size_t sampleIndex = 0; sampleIndex < 2048; ++sampleIndex)
        {
            const auto phase = (2.0f * std::numbers::pi_v<float>)
                * static_cast<float>(sampleIndex) / 2048.0f;

            switch (frameIndex)
            {
                case 0: // sine
                    data.frames[frameIndex][sampleIndex] = std::sin(phase);
                    break;
                case 1: // triangle (additive, full harmonics)
                {
                    float val = 0.0f;
                    for (int h = 1; h <= maxHarmonics; h += 2)
                    {
                        const float sign = ((h % 4) == 1) ? 1.0f : -1.0f;
                        val += sign * std::sin(phase * static_cast<float>(h))
                               / static_cast<float>(h * h);
                    }
                    data.frames[frameIndex][sampleIndex] = val;
                    break;
                }
                case 2: // saw (additive, full harmonics)
                {
                    float val = 0.0f;
                    for (int h = 1; h <= maxHarmonics; ++h)
                        val += std::sin(phase * static_cast<float>(h)) / static_cast<float>(h);
                    data.frames[frameIndex][sampleIndex] = val;
                    break;
                }
                case 3: // square (additive, full harmonics)
                {
                    float val = 0.0f;
                    for (int h = 1; h <= maxHarmonics; h += 2)
                        val += std::sin(phase * static_cast<float>(h)) / static_cast<float>(h);
                    data.frames[frameIndex][sampleIndex] = val;
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Normalize each frame
    for (auto& frame : data.frames)
    {
        float maxMag = 0.0f;
        for (const auto s : frame)
            maxMag = std::max(maxMag, std::abs(s));
        if (maxMag > 0.0f)
            for (auto& s : frame)
                s /= maxMag;
    }

    // Build band-limited mipmap
    data.buildMipmap();

    return data;
}
} // namespace fadmodel1::dsp::wavetable
