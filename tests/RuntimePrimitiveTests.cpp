#include <fad/engine/AudioView.h>
#include <fad/engine/math/LinearSmoother.h>
#include <fad/engine/math/DspSafetyUtils.h>
#include <fad/engine/rt/SpscCursor.h>
#include <fad/engine/rt/ScopedNoDenormals.h>
#include <array>
#include <atomic>
#include <cfenv>
#include <iostream>
#include <stdexcept>
#include <thread>

#pragma STDC FENV_ACCESS ON

namespace
{
int checks = 0;
void require(bool condition, const char* message)
{ ++checks; if (!condition) throw std::runtime_error(message); }
}
int main()
{
    try
    {
        std::array<float, 17> left{}, right{};
        left.fill(2); right.fill(3);
        std::array<float*, 2> pointers{left.data(), right.data()};
        fad::engine::AudioBufferView view(pointers.data(), 2, 17);
        view.subview(1, 15).clear();
        require(left.front() == 2 && left.back() == 2 && left[1] == 0, "view subrange damaged left guard");
        require(right.front() == 3 && right.back() == 3 && right[15] == 0, "view subrange damaged right guard");
        view.subview(17, 0).clear();
        require(view.channel(0) == left.data(), "view copied rather than borrowed samples");
        fad::engine::math::LinearSmoother<float> ramp;
        ramp.reset(4); ramp.setTargetValue(1);
        require(ramp.getNextValue() == .25f && ramp.getNextValue() == .5f, "ramp cadence changed");
        ramp.setTargetValue(0);
        require(ramp.getNextValue() == .375f, "retarget was not anchored to current value");
        require(ramp.skip(3) == 0 && !ramp.isSmoothing(), "skip did not finish at target");
        ramp.setTargetValue(1); ramp.getNextValue(); ramp.reset(48000, .005);
        require(ramp.getCurrentValue() == 1 && !ramp.isSmoothing(), "reset did not snap previous target");
        ramp.reset(0); ramp.setTargetValue(-2);
        require(ramp.getNextValue() == -2, "zero-duration ramp did not snap");

        const auto rounding = std::fegetround();
        require(std::fesetround(FE_TONEAREST) == 0, "nearest rounding unavailable");
        require(fad::engine::math::roundToNearestInt(.5) == 0
                && fad::engine::math::roundToNearestInt(1.5) == 2, "nearest ties changed");
        require(std::fesetround(FE_TOWARDZERO) == 0, "toward-zero rounding unavailable");
        require(fad::engine::math::roundToNearestInt(-.5) == -1, "legacy negative toward-zero conversion changed");
        require(std::fesetround(rounding) == 0, "rounding environment not restored");
        using DenormalScope = fad::engine::rt::ScopedNoDenormals;
        const auto control = DenormalScope::readControl();
        {
            DenormalScope scope;
            require(DenormalScope::readControl() == (control | DenormalScope::flushMask), "denormal control mask changed");
            volatile float input = std::numeric_limits<float>::min();
            const auto result = input * .25f;
            require(result == 0, "hardware denormal output was not flushed");
        }
        require(DenormalScope::readControl() == control, "denormal scope did not restore caller state");

        fad::engine::rt::SpscCursor<5> cursor;
        auto w = cursor.reserveWrite(9);
        require(w.size() == 4 && w.size2 == 0, "FIFO usable capacity changed");
        cursor.commitWrite(4);
        require(cursor.reserveWrite(1).size() == 0, "full FIFO overwrote unread values");
        cursor.commitRead(3);
        w = cursor.reserveWrite(3);
        require(w.start1 == 4 && w.size1 == 1 && w.size2 == 2, "FIFO wrap spans wrong");
        cursor.commitWrite(3);
        auto r = cursor.reserveRead(9);
        require(r.start1 == 3 && r.size1 == 2 && r.size2 == 2, "FIFO read order wrong");
        cursor.commitRead(4);
        require(cursor.reserveRead(1).size() == 0, "empty FIFO was readable");

        // Stress only the SPSC ownership contract; waiting occurs in the test,
        // never in cursor methods or a render callback.
        fad::engine::rt::SpscCursor<127> queue;
        std::array<unsigned, 127> values{};
        constexpr unsigned count = 200000;
        std::atomic<unsigned> errors{0};
        std::thread producer([&] {
            unsigned next = 0;
            while (next < count)
            {
                const auto region = queue.reserveWrite(std::min(17u, count - next));
                for (unsigned i = 0; i < region.size1; ++i) values[region.start1 + i] = next++;
                for (unsigned i = 0; i < region.size2; ++i) values[region.start2 + i] = next++;
                queue.commitWrite(region.size());
                if (region.size() == 0) std::this_thread::yield();
            }
        });
        unsigned expected = 0;
        while (expected < count)
        {
            const auto region = queue.reserveRead(19);
            for (unsigned i = 0; i < region.size1; ++i)
                if (values[region.start1 + i] != expected++) ++errors;
            for (unsigned i = 0; i < region.size2; ++i)
                if (values[region.start2 + i] != expected++) ++errors;
            queue.commitRead(region.size());
            if (region.size() == 0) std::this_thread::yield();
        }
        producer.join();
        require(errors.load() == 0 && expected == count, "SPSC lost or reordered values");
        std::cout << "RuntimePrimitiveTests PASS checks=" << checks << " concurrent_values=" << count << '\n';
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
