#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace fadmodel1::auv3
{
// AUv3 render events use the wrapper's existing parameter indices and normalised
// value space. No parameter-ID/address policy is introduced here.
class RenderTimeline final
{
public:
    using SampleTime = std::int64_t;
    static constexpr std::size_t eventCapacity = 256;
    static constexpr std::size_t rampCapacity = 64;
    static constexpr int maximumFrames = 4096;

    struct Diagnostics
    {
        std::uint64_t rejectedEvents = 0;
        std::uint64_t eventOverflows = 0;
        std::uint64_t rampOverflows = 0;
        std::uint64_t discontinuities = 0;
    };

    void reset() noexcept
    {
        eventCount = 0;
        for (auto& ramp : ramps) ramp.active = false;
        hasPreviousBlock = false;
    }

    bool beginBlock(SampleTime start, int frames) noexcept
    {
        if (frames < 0 || frames > maximumFrames
            || start > std::numeric_limits<SampleTime>::max() - frames)
            return false;
        if (hasPreviousBlock && start != previousBlockEnd)
        {
            reset();
            ++diagnostics.discontinuities;
        }
        blockStart = start;
        blockFrames = frames;
        return true;
    }

    // The wrapper synchronises before enqueuing this block's host events.
    // A restore during rendering cancels its old pending events and ramps.
    bool synchroniseEpoch(std::uint64_t epoch) noexcept
    {
        const bool changed = hasEpoch && epoch != renderEpoch;
        if (changed) reset();
        renderEpoch = epoch;
        hasEpoch = true;
        return changed;
    }

    // A full queue rejects the new event, retaining the already accepted order
    // and ramps. It never evicts an event into a different parameter.
    bool addEvent(SampleTime sample, int parameterIndex, float target,
                  std::uint32_t duration, bool rampable) noexcept
    {
        if (parameterIndex < 0 || !std::isfinite(target)
            || (duration > 0 && !rampable)
            || sample > std::numeric_limits<SampleTime>::max() - duration)
        {
            ++diagnostics.rejectedEvents;
            return false;
        }
        if (eventCount == eventCapacity)
        {
            ++diagnostics.eventOverflows;
            return false;
        }
        sample = std::max(sample, blockStart); // late events apply at this block's first sample
        Event event{sample, parameterIndex, std::clamp(target, 0.0f, 1.0f), duration};
        // Stable, fixed-storage insertion. Equal sample times retain host order.
        auto position = eventCount;
        while (position > 0 && events[position - 1].sample > sample)
        {
            events[position] = events[position - 1];
            --position;
        }
        events[position] = event;
        ++eventCount;
        return true;
    }

    // read/write touch only the requested DSP parameter. renderRange advances
    // the existing render timeline, never a whole processor/transport callback.
    // Ramps are linear in the AU's normalised value space, including across
    // blocks. The endpoint is reached at start + duration; a new event cancels
    // the prior ramp from its value at that exact sample.
    template <typename Read, typename Write, typename RenderRange>
    void render(Read&& read, Write&& write, RenderRange&& renderRange) noexcept
    {
        renderControlled(std::forward<Read>(read), std::forward<Write>(write),
                         std::forward<RenderRange>(renderRange),
                         [](int) noexcept { return true; }, []() noexcept {});
    }

    // Bracket only one parameter point. A refused point cancels its schedule
    // and renders the rest with the last coherent state; it never waits.
    template <typename Read, typename Write, typename RenderRange, typename BeginPoint, typename EndPoint>
    void renderControlled(Read&& read, Write&& write, RenderRange&& renderRange,
                          BeginPoint&& beginPoint, EndPoint&& endPoint) noexcept
    {
        const auto end = blockStart + blockFrames;
        auto sample = blockStart;
        std::size_t consumed = 0;
        while (sample < end)
        {
            if (!beginPoint(static_cast<int>(sample - blockStart)))
            {
                reset();
                consumed = 0;
                renderRange(static_cast<int>(sample - blockStart), static_cast<int>(end - sample));
                sample = end;
                break;
            }
            updateRamps(sample, write);
            while (consumed < eventCount && events[consumed].sample <= sample)
                apply(events[consumed++], sample, read, write);
            endPoint();

            const bool ramping = std::any_of(ramps.begin(), ramps.end(),
                                             [](const auto& ramp) { return ramp.active; });
            const auto next = ramping ? sample + 1
                : (consumed < eventCount ? std::min(end, events[consumed].sample) : end);
            renderRange(static_cast<int>(sample - blockStart), static_cast<int>(next - sample));
            sample = next;
        }
        // Also publish the exact boundary value for the next block/ramp, without
        // consuming events at end (which belong to the following block).
        if (blockFrames > 0)
        {
            if (beginPoint(blockFrames))
            {
                updateRamps(end, write);
                endPoint();
            }
            else
            {
                reset();
                consumed = 0;
            }
        }
        for (std::size_t i = consumed; i < eventCount; ++i) events[i - consumed] = events[i];
        eventCount -= consumed;
        previousBlockEnd = end;
        hasPreviousBlock = true;
    }

    const Diagnostics& getDiagnostics() const noexcept { return diagnostics; }
    std::size_t pendingEvents() const noexcept { return eventCount; }
    std::size_t activeRamps() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(ramps.begin(), ramps.end(),
                                      [](const auto& ramp) { return ramp.active; }));
    }

private:
    struct Event { SampleTime sample; int index; float target; std::uint32_t duration; };
    struct Ramp
    {
        SampleTime start = 0;
        int index = -1;
        float initial = 0.0f, target = 0.0f;
        std::uint32_t duration = 0;
        bool active = false;
    };

    template <typename Write>
    void updateRamps(SampleTime sample, Write& write) noexcept
    {
        for (auto& ramp : ramps)
        {
            if (!ramp.active) continue;
            const auto elapsed = sample - ramp.start;
            const bool finished = elapsed >= ramp.duration;
            const auto fraction = finished ? 1.0 : static_cast<double>(elapsed) / ramp.duration;
            write(ramp.index, static_cast<float>(ramp.initial
                + (static_cast<double>(ramp.target) - ramp.initial) * fraction));
            if (finished) ramp.active = false;
        }
    }

    template <typename Read, typename Write>
    void apply(const Event& event, SampleTime sample, Read& read, Write& write) noexcept
    {
        Ramp* existing = nullptr;
        Ramp* free = nullptr;
        for (auto& ramp : ramps)
        {
            if (ramp.active && ramp.index == event.index) existing = &ramp;
            else if (!ramp.active && free == nullptr) free = &ramp;
        }
        if (event.duration == 0)
        {
            if (existing != nullptr) existing->active = false;
            write(event.index, event.target);
            return;
        }
        auto* slot = existing != nullptr ? existing : free;
        if (slot == nullptr)
        {
            ++diagnostics.rampOverflows;
            return;
        }
        const auto initial = read(event.index);
        if (!std::isfinite(initial))
        {
            ++diagnostics.rejectedEvents;
            return;
        }
        *slot = {sample, event.index, std::clamp(initial, 0.0f, 1.0f), event.target, event.duration, true};
    }

    std::array<Event, eventCapacity> events{};
    std::array<Ramp, rampCapacity> ramps{};
    std::size_t eventCount = 0;
    SampleTime blockStart = 0, previousBlockEnd = 0;
    int blockFrames = 0;
    bool hasPreviousBlock = false;
    std::uint64_t renderEpoch = 0;
    bool hasEpoch = false;
    Diagnostics diagnostics{}; // read only off the render thread, or after it stops
};
}
