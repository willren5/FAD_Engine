#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace fad::engine::assets
{
// Preparation-only radix-2 DFT decomposition. Scratch/twiddles allocate here;
// no renderer may construct or execute this transform inside its callback.
// Mathematical basis: Cooley & Tukey, Mathematics of Computation 19 (1965),
// 297–301. This implementation is written here, not copied from JUCE/another FFT.
class PreparationFft final
{
public:
    explicit PreparationFft(size_t size) : size_(size), roots_(size / 2)
    {
        assert(size >= 2 && (size & (size - 1)) == 0);
        for (size_t i = 0; i < roots_.size(); ++i)
        {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
            roots_[i] = {std::cos(angle), std::sin(angle)};
        }
    }
    void transform(std::vector<std::complex<double>>& values, bool inverse) const noexcept
    {
        assert(values.size() == size_);
        for (size_t i = 1, reversed = 0; i < size_; ++i)
        {
            size_t bit = size_ >> 1;
            while ((reversed & bit) != 0) { reversed ^= bit; bit >>= 1; }
            reversed ^= bit;
            if (i < reversed) std::swap(values[i], values[reversed]);
        }
        for (size_t length = 2; length <= size_; length <<= 1)
        {
            const size_t half = length / 2, stride = size_ / length;
            for (size_t first = 0; first < size_; first += length)
                for (size_t i = 0; i < half; ++i)
                {
                    const auto root = inverse ? std::conj(roots_[i * stride]) : roots_[i * stride];
                    const auto even = values[first + i];
                    const auto odd = values[first + i + half] * root;
                    values[first + i] = even + odd;
                    values[first + i + half] = even - odd;
                }
        }
        if (inverse)
            for (auto& value : values) value /= static_cast<double>(size_);
    }
private:
    size_t size_;
    std::vector<std::complex<double>> roots_;
};
}
