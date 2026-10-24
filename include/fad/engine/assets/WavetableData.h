#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include "AssetAttachment.h"

#include <vector>
#include "NativeLfoCurveData.h"
#include "../parameters/CurveNodes.h"

namespace fadmodel1::dsp::wavetable
{
struct WavetableData
{
    static constexpr size_t defaultFrameSize = 2048;
    static constexpr size_t maxFrames = 256;
    static constexpr size_t maxSampleFrames = 1024; // bounded PCM budget, independent of cyclic tables
    static constexpr int maxMipLevels = 12; // 4096-sample source through the one-harmonic level

    std::vector<std::vector<float>> frames;
    size_t frameSize = defaultFrameSize;
    size_t frameCount = 0;
    // Immutable application data is retired with this asset. The core never reads it.
    std::shared_ptr<const fad::engine::AssetAttachment> applicationData;
    // Explicit no-morph assets use floor(position*frameCount), capped at the
    // final frame. Absent metadata preserves legacy continuous interpolation.
    bool discreteFrames = false;
    // Drawing replay shares the bank's atomic publication and session codec.
    // Empty times means an ordinary pre-existing asset.
    static constexpr size_t historyNodeFields = 89;
    int historyOwner = -1;
    std::vector<float> historyTimes;
    std::vector<std::array<float,historyNodeFields>> historyNodes;
    bool validHistory() const noexcept;
    float historyFramePosition(float position) const noexcept;
    float sampleHistory(float position, double phase) const noexcept;
    float sampleHistoryFrame(float framePosition, double phase) const noexcept;
    float historyLoopStart(float position) const noexcept;
    parameters::CurveNodes historyCurve(float position) const noexcept;
    // Optional imported phase map, sampled at i/2048 including phase 1.
    // Immutable with the oscillator asset; absent leaves ordinary tables unchanged.
    static constexpr size_t phaseRemapSize = 2049;
    std::vector<float> phaseRemap;
    static bool validPhaseRemap(const std::vector<float>& points) noexcept
    {
        return points.empty() || (points.size() == phaseRemapSize
            && std::all_of(points.begin(), points.end(), [](float v) {
                return std::isfinite(v) && v >= 0.0f && v <= 1.0f;
            }));
    }
    // Optional interleaved PCM sample metadata. PCM is packed across frames without cycle
    // normalization; sampleLength excludes the zero padding in the final frame.
    // Optional LFO grid: frames sample i / frameSize; endpoints store phase 1.
    // Empty retains the legacy endpoint-inclusive i / (frameSize - 1) contract.
    std::vector<float> curveLoopStarts; // -1: no ENV loop, otherwise normalized loop start
    float curveLoopStart(size_t frame) const noexcept
    { return frame < curveLoopStarts.size() ? curveLoopStarts[frame] : -1.0f; }
    std::vector<float> curveEndpoints;
    modulation::NativeLfoCurveData nativeCurves;
    const float* curveEndpoint(size_t frame) const noexcept
    { return frame < curveEndpoints.size() ? &curveEndpoints[frame] : nullptr; }

    size_t sampleLength = 0;
    double sampleRate = 0.0;
    int sampleChannels = 1; // absent persisted metadata retains legacy mono

    // Reduced mip storage. Logical level 0 is `frames`; mipLevels[N - 1]
    // stores logical level N at max(256, frameSize / 2^N) samples per frame.
    std::vector<std::vector<float>> mipLevels;
    std::vector<size_t> mipFrameSizes;
    // Total selectable logical levels, including base level 0.
    int mipLevelCount = 0;

    struct PreparedBandLimitedSample
    {
        const float* levelAFrameA = nullptr;
        const float* levelAFrameB = nullptr;
        size_t levelAFrameSize = 0;
        const float* levelBFrameA = nullptr;
        const float* levelBFrameB = nullptr;
        size_t levelBFrameSize = 0;
        float frameFraction = 0.0f;
        float levelFraction = 0.0f;
    };

    static bool isSupportedFrameSize(size_t size) noexcept;
    bool hasRenderableLayout() const noexcept;
    bool isValid() const noexcept;

    float sampleAtPosition(float position, float phase) const noexcept;

    // Band-limited sampling: selects mip level based on phaseDelta (freq/sampleRate)
    float sampleAtPositionBandLimited(float position, float phase, float phaseDelta) const noexcept;
    float sampleAtPositionBandLimitedWithLevel(float position,
                                               float phase,
                                               float idealMipLevel) const noexcept;
    // Resolves immutable frame/mip pointers without allocation. The prepared
    // view is valid only while this WavetableData remains alive and unchanged.
    bool prepareBandLimitedSample(float position,
                                  float idealMipLevel,
                                  PreparedBandLimitedSample& prepared) const noexcept;
    static float samplePreparedBandLimited(const PreparedBandLimitedSample& prepared,
                                           float phase) noexcept;
    static float calculateIdealMipLevel(size_t sourceFrameSize, float phaseDelta) noexcept;

    // Build mipmap from current frames after loading/publication preparation.
    // RT-unsafe by design: allocates FFT scratch and mip vectors; never call from audio callbacks.
    void buildMipmap();
};

WavetableData createBuiltinWavetable();
} // namespace fadmodel1::dsp::wavetable
