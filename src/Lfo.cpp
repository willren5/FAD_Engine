#include <fad/engine/modulation/Lfo.h>

#include <fad/engine/math/DspSafetyUtils.h>

namespace fadmodel1::dsp::modulation
{
namespace
{
float randomValueForCycle(uint64_t cycleIndex) noexcept
{
    uint64_t state = cycleIndex + 0x9e3779b97f4a7c15ull;
    state ^= state >> 30;
    state *= 0xbf58476d1ce4e5b9ull;
    state ^= state >> 27;
    state *= 0x94d049bb133111ebull;
    state ^= state >> 31;

    const auto normalised =
        static_cast<float>(state & 0xffffffffu) / static_cast<float>(0xffffffffu);
    return normalised * 2.0f - 1.0f;
}

float renderShape(double phase, parameters::LfoShape shape, uint64_t cycleIndex) noexcept
{
    switch (shape)
    {
        case parameters::LfoShape::triangle:
        {
            const auto normalisedPhase = phase / (2.0 * std::numbers::pi);
            return static_cast<float>(1.0 - 4.0 * std::abs(normalisedPhase - 0.5));
        }

        case parameters::LfoShape::sawUp:
            return static_cast<float>((phase / std::numbers::pi) - 1.0);

        case parameters::LfoShape::square:
            return phase < std::numbers::pi ? 1.0f : -1.0f;

        case parameters::LfoShape::random:
            return randomValueForCycle(cycleIndex);

        case parameters::LfoShape::sine:
        default:
            return static_cast<float>(std::sin(phase));
    }
}
}

void Lfo::prepare(double newSampleRate)
{
    sampleRate = fadmodel1::dsp::utils::sanitizeSampleRate(newSampleRate);
    reset();
}

void Lfo::reset() noexcept
{
    phaseOffset = 0.0;
    phaseDelta = 0.0;
    samplePosition = 0;
    cycleBase = 0;
}

void Lfo::setPhase(float newPhase) noexcept
{
    phaseOffset = wrapPhase(static_cast<double>(newPhase));
    samplePosition = 0;
    cycleBase = 0;
}

void Lfo::setRate(float newRateHz) noexcept
{
    const auto limitedRate = std::clamp(std::isfinite(newRateHz) ? newRateHz : 1.0f, 0.0f, static_cast<float>(sampleRate * 0.49));
    const auto nextPhaseDelta =
        (2.0 * std::numbers::pi) * static_cast<double>(limitedRate) / sampleRate;
    if (std::abs(nextPhaseDelta - phaseDelta) <= 1.0e-15)
        return;

    const auto totalPhase =
        phaseOffset + phaseDelta * static_cast<double>(samplePosition);
    const auto wraps = static_cast<uint64_t>(
        std::max(0.0, std::floor(totalPhase / (2.0 * std::numbers::pi))));
    cycleBase += wraps;
    phaseOffset = wrapPhase(totalPhase);
    samplePosition = 0;
    phaseDelta = nextPhaseDelta;
}

float Lfo::renderCurveSampleAtOffset(const float* samples, size_t count, int sampleOffset, bool oneShot, const float* endpoint, float loopStart) const noexcept
{
    if (samples == nullptr || count < 2 || ! std::isfinite(phaseOffset) || ! std::isfinite(phaseDelta)) return 0.0f;
    const auto totalPhase = phaseOffset + phaseDelta * (static_cast<double>(samplePosition) + std::max(0, sampleOffset));
    double phase01;
    if (oneShot && (cycleBase > 0 || totalPhase >= (2.0 * std::numbers::pi))) {
        if (!std::isfinite(loopStart) || loopStart < 0.0f || loopStart >= 1.0f)
            return endpoint != nullptr ? *endpoint : samples[count-1];
        const auto cycles = static_cast<double>(cycleBase) + totalPhase / (2.0 * std::numbers::pi);
        phase01 = loopStart + std::fmod(cycles - 1.0, 1.0 - static_cast<double>(loopStart));
    } else phase01 = wrapPhase(totalPhase) / (2.0 * std::numbers::pi);
    if (endpoint != nullptr) {
        const auto position = phase01 * static_cast<double>(count);
        const auto index = std::min(static_cast<size_t>(position), count - 1);
        const auto next = index + 1 < count ? samples[index + 1] : *endpoint;
        return samples[index] + static_cast<float>(position - static_cast<double>(index)) * (next - samples[index]);
    }
    const auto position = phase01 * static_cast<double>(count - 1);
    const auto index = std::min(static_cast<size_t>(position), count - 2);
    const auto fraction = static_cast<float>(position - static_cast<double>(index));
    return samples[index] + fraction * (samples[index + 1] - samples[index]);
}

double Lfo::nodePhaseAtOffset(int offset,bool oneShot,float loopStart) const noexcept
{
    const auto total=phaseOffset+phaseDelta*(double(samplePosition)+std::max(0,offset));
    if(!std::isfinite(total))return 0.;
    if(oneShot && (cycleBase>0 || total>=(2.0 * std::numbers::pi)))
    {
        if(!std::isfinite(loopStart)||loopStart<0||loopStart>=1)return 1.;
        const auto cycles=double(cycleBase)+total/(2.0 * std::numbers::pi);
        return loopStart+std::fmod(cycles-1.,1.-double(loopStart));
    }
    return wrapPhase(total)/(2.0 * std::numbers::pi);
}

float Lfo::renderPointCurveAtOffset(const NativeLfoCurveRuntime& curve, int sampleOffset, bool oneShot) const noexcept
{
    if (!std::isfinite(phaseOffset) || !std::isfinite(phaseDelta)) return 0.0f;
    const auto total = phaseOffset + phaseDelta * (static_cast<double>(samplePosition) + std::max(0, sampleOffset));
    if (oneShot && (cycleBase > 0 || total >= (2.0 * std::numbers::pi)))
    {
        const auto start = static_cast<double>(curve.getLoopStart());
        if (start < 0.0 || start >= 1.0 || !std::isfinite(start)) return curve.sample(1.0);
        const auto cycles = static_cast<double>(cycleBase) + total / (2.0 * std::numbers::pi);
        return curve.sample(start + std::fmod(cycles - 1.0, 1.0 - start));
    }
    return curve.sample(wrapPhase(total) / (2.0 * std::numbers::pi));
}

float Lfo::renderSampleAtOffset(parameters::LfoShape shape, int sampleOffset, bool oneShot) const noexcept
{
    if (! std::isfinite(phaseOffset) || ! std::isfinite(phaseDelta))
        return 0.0f;

    const auto absoluteOffset =
        static_cast<double>(samplePosition + static_cast<uint64_t>(std::max(0, sampleOffset)));
    const auto totalPhase = phaseOffset + phaseDelta * absoluteOffset;
    const auto wraps = static_cast<uint64_t>(
        std::max(0.0, std::floor(totalPhase / (2.0 * std::numbers::pi))));
    const auto phaseAtOffset = oneShot && (cycleBase > 0 || totalPhase >= (2.0 * std::numbers::pi))
        ? std::nextafter((2.0 * std::numbers::pi), 0.0) : wrapPhase(totalPhase);
    return renderShape(phaseAtOffset, shape, oneShot ? uint64_t{0} : cycleBase + wraps);
}

float Lfo::renderNextSample(parameters::LfoShape shape) noexcept
{
    const auto output = renderSampleAtOffset(shape, 0);
    advance(1);
    return output;
}

void Lfo::advance(int numSamples) noexcept
{
    if (! std::isfinite(phaseOffset) || ! std::isfinite(phaseDelta))
    {
        phaseOffset = 0.0;
        phaseDelta = 0.0;
        samplePosition = 0;
        cycleBase = 0;
        return;
    }

    samplePosition += static_cast<uint64_t>(std::max(0, numSamples));
    if (samplePosition < 1'000'000)
        return;

    const auto totalPhase = phaseOffset + phaseDelta * static_cast<double>(samplePosition);
    const auto wraps = static_cast<uint64_t>(
        std::max(0.0, std::floor(totalPhase / (2.0 * std::numbers::pi))));
    cycleBase += wraps;
    phaseOffset = wrapPhase(totalPhase);
    samplePosition = 0;
}

double Lfo::wrapPhase(double value) noexcept
{
    if (! std::isfinite(value))
        return 0.0;

    auto wrapped = std::fmod(value, (2.0 * std::numbers::pi));
    if (! std::isfinite(wrapped))
        return 0.0;

    if (wrapped < 0.0)
        wrapped += (2.0 * std::numbers::pi);

    return wrapped;
}
}
