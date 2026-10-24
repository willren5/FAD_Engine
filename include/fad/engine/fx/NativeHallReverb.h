#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fadmodel1::dsp::fx
{
struct NativeHallShelfCoefficients
{
    std::array<double,3> a{1,0,0}, b{1,0,0};
};

// Independent native impulse identification: both Hall tone controls are
// -24*x^2 dB shelves with Q=.3. They are not brick-wall Lo/Hi cut filters.
inline NativeHallShelfCoefficients nativeHallShelfCoefficients(double rate, double raw, bool high) noexcept
{
    rate=std::isfinite(rate)?std::clamp(rate,8000.0,384000.0):48000.0;
    raw=std::isfinite(raw)?std::clamp(raw,0.0,1.0):0.0;
    const double A=std::pow(10.0,-24.0*raw*raw/40.0);
    const double hz=high?3330.0-1000.0*raw*raw:90.0+270.0*raw;
    const double w=6.2831853071795864769*std::min(hz,rate*.49)/rate;
    const double c=std::cos(w),d=std::sin(w)*std::sqrt(A)/.3;
    NativeHallShelfCoefficients result;
    if(high) {
        result.b={A*((A+1)+(A-1)*c+d),-2*A*((A-1)+(A+1)*c),A*((A+1)+(A-1)*c-d)};
        result.a={(A+1)-(A-1)*c+d,2*((A-1)-(A+1)*c),(A+1)-(A-1)*c-d};
    } else {
        result.b={A*((A+1)-(A-1)*c+d),2*A*((A-1)-(A+1)*c),A*((A+1)-(A-1)*c-d)};
        result.a={(A+1)+(A-1)*c+d,-2*((A-1)+(A+1)*c),(A+1)+(A-1)*c-d};
    }
    const auto denominator=result.a[0];
    for(size_t i=0;i<3;++i) {result.a[i]/=denominator;result.b[i]/=denominator;}
    return result;
}

// Native impulse probes identify three 0.625 allpasses per side and crossed
// feedback. The early feed-through is exactly -0.625^3. This path is selected
// only by imported Hall state; the original FAD network remains separate.
class NativeHallReverb
{
    static constexpr double diffusionGain = 0.625;
    static constexpr double attenuationAt48k = 0.00175;
    static constexpr std::array<double, 2> firstDiffusionNotes{38.0, 40.0};
    struct Delay
    {
        std::vector<float> samples;
        size_t write = 0, mask = 0;
        size_t validSamples = 0;
        double previous = 0;
        void prepare(size_t maximum)
        {
            size_t capacity = 2;
            while (capacity <= maximum + 2) capacity *= 2;
            samples.assign(capacity, 0.0f); mask = capacity - 1; reset();
        }
        // Logical silence avoids clearing large delay buffers on a mode change
        // in the audio callback. Reads cannot see samples from before reset.
        void reset() noexcept { write = validSamples = 0; previous = 0; }
        double read(double delay, bool thiran) noexcept
        {
            delay = std::clamp(delay, 1.0, static_cast<double>(mask - 2));
            const auto whole = static_cast<size_t>(delay);
            const auto fraction = delay - static_cast<double>(whole);
            const auto first = whole <= validSamples ? static_cast<double>(samples[(write - whole) & mask]) : 0.0;
            const auto second = whole + 1 <= validSamples ? static_cast<double>(samples[(write - whole - 1) & mask]) : 0.0;
            if (!thiran) return first + fraction * (second - first);
            if (fraction < 1.e-9) { previous = first; return first; }
            const auto coefficient = (1.0 - fraction) / (1.0 + fraction);
            previous = second + coefficient * (first - previous);
            if (!std::isfinite(previous) || std::abs(previous) < 1.e-20) previous = 0;
            return previous;
        }
        void push(double value) noexcept
        {
            samples[write] = std::isfinite(value) ? static_cast<float>(std::clamp(value, -4.0, 4.0)) : 0.0f;
            write = (write + 1) & mask;
            validSamples = std::min(validSamples + 1, mask + 1);
        }
    };
public:
    // Retirement must wait through a whole diffusion/feedback cycle before
    // treating quiet output as empty storage. Use the full control domain:
    // delay history may still contain energy from a previously larger Size.
    static double maximumNetworkSilenceSeconds() noexcept
    {
        return diffusionCycleSeconds(std::exp2(5.0 + 0.80 / 12.0));
    }

    static double tailSeconds(float size, float decay, float spinDepth) noexcept
    {
        const auto unit = [](float v) { return std::isfinite(v) ? std::clamp(static_cast<double>(v), 0.0, 1.0) : 0.0; };
        const auto scale = std::exp2(5.0 * unit(size));
        const auto spinScale = std::exp2(0.80 * unit(spinDepth) / 12.0);
        const auto longestCycle = diffusionCycleSeconds(scale * spinScale);
        double smallestLoss = 1.0;
        for (size_t channel = 0; channel < 2; ++channel) {
            const auto lastDelaySeconds = 1.0 / (440.0 * std::exp2((firstDiffusionNotes[channel] - 8.0 - 69.0) / 12.0));
            smallestLoss = std::min(smallestLoss, attenuationAt48k * 48000.0
                * std::pow(0.001, unit(decay)) * lastDelaySeconds * scale);
        }
        // Budget to -80 dB, with room for the shelving filters and diffusion.
        return std::clamp(longestCycle * (1.0 + std::log(10000.0) / smallestLoss), 0.0, 1800.0);
    }

    void prepare(double rate)
    {
        sampleRate = rate;
        idleLimit = static_cast<int>(std::llround(rate * (1504.0 / 48000.0)));
        sizeCoefficient = 1.0 - std::exp(-1.0 / (rate * .0625));
        for (auto& channel : allpasses) for (auto& delay : channel) delay.prepare(static_cast<size_t>(rate * .75) + 4);
        for (auto& delay : feedback) delay.prepare(static_cast<size_t>(rate * .025) + 4);
        for (size_t channel = 0; channel < 2; ++channel) {
            crossDelay[channel] = rate / (440.0 * std::exp2(((channel == 0 ? 92.0 : 90.0) - 69.0) / 12.0));
            for (size_t stage = 0; stage < 3; ++stage)
                baseDelay[channel][stage] = rate / (440.0 * std::exp2((firstDiffusionNotes[channel] - 4.0 * static_cast<double>(stage) - 69.0) / 12.0));
        }
        lastSize = lastDecay = lastHiCut = lastLoCut = -1.0;
        setControls(.35f, .35f, .35f, 0.0f, 0.0f, 0.0f);
        reset();
    }
    void reset() noexcept
    {
        for (auto& channel : allpasses) for (auto& delay : channel) delay.reset();
        for (auto& delay : feedback) delay.reset();
        filterState = {}; lowFilterState = {}; currentSize = 0.0; idleRemaining = idleLimit;
        spinPhase = 0.0; spinStarted = false;
    }
    void setControls(float size, float decay, float hiCut, float loCut = 0.0f,
                     float spinRate = 0.0f, float spinDepth = 0.0f) noexcept
    {
        const auto unit = [](float value) { return std::isfinite(value) ? std::clamp(static_cast<double>(value), 0.0, 1.0) : 0.0; };
        const auto s = unit(size), d = unit(decay), h = unit(hiCut), l = unit(loCut);
        currentSpinRate = unit(spinRate);
        currentSpinDepth = unit(spinDepth);
        if (s != lastSize || d != lastDecay) {
            targetSize = std::exp2(5.0 * s);
            const auto attenuation = attenuationAt48k * (48000.0 / sampleRate) * std::pow(.001, d);
            for (size_t channel = 0; channel < 2; ++channel)
                feedbackGain[channel] = std::exp(-attenuation * baseDelay[channel][2] * targetSize);
            lastSize = s; lastDecay = d;
        }
        if (h != lastHiCut) {
            const auto coefficients=nativeHallShelfCoefficients(sampleRate,h,true);
            filterA=coefficients.a;filterB=coefficients.b;
            lastHiCut = h;
        }
        if(l != lastLoCut) {
            const auto coefficients=nativeHallShelfCoefficients(sampleRate,l,false);
            lowFilterA=coefficients.a;lowFilterB=coefficients.b;
            lastLoCut=l;
        }
    }
    std::array<float, 2> process(float left, float right) noexcept
    {
        const std::array<double, 2> input{std::isfinite(left) ? left : 0.0f, std::isfinite(right) ? right : 0.0f};
        if (std::max(std::abs(input[0]), std::abs(input[1])) > 1.e-10) { idleRemaining = idleLimit; spinStarted = true; }
        if (idleRemaining > 0) { currentSize += sizeCoefficient * (targetSize - currentSize); --idleRemaining; }
        // Serum Hall Spin is a stereo-opposed modulation of the diffusion
        // delays, not a global Size shift. Independent impulse captures anchor
        // full Depth at roughly +/-0.8 semitone at the fixed startup phase.
        // A zero Rate therefore keeps a static decorrelating offset. The phase
        // starts only after the Hall is actually excited: long pre-roll silence
        // leaves Serum's first impulse byte-identical.
        constexpr double maximumSpinSemitones = 0.80;
        const auto spin = currentSpinDepth * maximumSpinSemitones
            * std::cos(6.2831853071795864769 * spinPhase);
        std::array<double, 2> delayed{};
        std::array<double, 2> spinScale{};
        for (size_t channel = 0; channel < 2; ++channel) {
            const auto signedSpin = channel == 0 ? spin : -spin;
            spinScale[channel] = std::exp2(signedSpin / 12.0);
            delayed[channel] = feedback[channel].read(1.0 + crossDelay[channel] * currentSize * spinScale[channel], false);
        }
        std::array<float, 2> output{};
        for (size_t channel = 0; channel < 2; ++channel) {
            auto value = input[channel] + delayed[channel];
            auto& state = filterState[channel];
            const auto filtered = filterB[0] * value + state[0];
            state[0] = filterB[1] * value - filterA[1] * filtered + state[1];
            state[1] = filterB[2] * value - filterA[2] * filtered;
            value = std::isfinite(filtered) ? filtered : 0.0;
            for (auto& sample : state) if (!std::isfinite(sample) || std::abs(sample) < 1.e-20) sample = 0;
            auto& lowState=lowFilterState[channel];
            const auto lowFiltered=lowFilterB[0]*value+lowState[0];
            lowState[0]=lowFilterB[1]*value-lowFilterA[1]*lowFiltered+lowState[1];
            lowState[1]=lowFilterB[2]*value-lowFilterA[2]*lowFiltered;
            value=std::isfinite(lowFiltered)?lowFiltered:0.0;
            for(auto& sample:lowState)if(!std::isfinite(sample)||std::abs(sample)<1.e-20)sample=0;
            for (size_t stage = 0; stage < 3; ++stage) {
                const auto delay = allpasses[channel][stage].read(baseDelay[channel][stage] * currentSize * spinScale[channel], true);
                const auto write = value + diffusionGain * delay;
                allpasses[channel][stage].push(write);
                value = delay - diffusionGain * write;
            }
            if (!std::isfinite(value)) value = 0;
            output[channel] = static_cast<float>(value);
            feedback[1 - channel].push(value * feedbackGain[channel]);
        }
        if (std::max(std::abs(output[0]), std::abs(output[1])) > 1.e-10f) idleRemaining = idleLimit;
        if (spinStarted && idleRemaining > 0 && currentSpinRate > 0.0) {
            // The public control is percentage-like rather than Hz. A squared
            // one-Hz ceiling is intentionally conservative until the long-tail
            // rate sweep is fully identified; Depth=0 remains bit-exact.
            const auto rateHz = currentSpinRate * currentSpinRate;
            spinPhase += rateHz / sampleRate;
            spinPhase -= std::floor(spinPhase);
        }
        if (spinStarted && idleRemaining <= 0) spinStarted = false;
        return output;
    }
private:
    static double diffusionCycleSeconds(double scale) noexcept
    {
        double longest = 0.0;
        for (size_t channel = 0; channel < 2; ++channel) {
            double diffusion = 0.0;
            for (size_t stage = 0; stage < 3; ++stage)
                diffusion += 1.0 / (440.0 * std::exp2((firstDiffusionNotes[channel]
                    - 4.0 * static_cast<double>(stage) - 69.0) / 12.0));
            const auto cross = 1.0 / (440.0 * std::exp2(((channel == 0 ? 92.0 : 90.0) - 69.0) / 12.0));
            // Maximum allpass group delay, plus room for the shelving filters.
            longest = std::max(longest, scale * (diffusion * (1.0 + diffusionGain)
                / (1.0 - diffusionGain) + cross) + 0.1);
        }
        return longest;
    }

    std::array<std::array<Delay, 3>, 2> allpasses;
    std::array<Delay, 2> feedback;
    std::array<std::array<double, 3>, 2> baseDelay{};
    std::array<double, 2> crossDelay{}, feedbackGain{};
    std::array<std::array<double, 2>, 2> filterState{};
    std::array<double, 3> filterA{}, filterB{};
    std::array<std::array<double,2>,2> lowFilterState{};
    std::array<double,3> lowFilterA{1,0,0},lowFilterB{1,0,0};
    double sampleRate = 48000.0, currentSize = 0.0, targetSize = 1.0, sizeCoefficient = 0.0;
    double currentSpinRate = 0.0, currentSpinDepth = 0.0, spinPhase = 0.0;
    bool spinStarted = false;
    double lastSize = -1.0, lastDecay = -1.0, lastHiCut = -1.0, lastLoCut = -1.0;
    int idleLimit = 0, idleRemaining = 0;
};
}
