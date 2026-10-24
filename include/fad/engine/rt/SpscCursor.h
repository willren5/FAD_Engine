#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>

namespace fad::engine::rt
{
// One producer owns reserveWrite/commitWrite; one consumer owns
// reserveRead/commitRead. Capacity includes one sentinel slot. A full queue
// accepts zero new elements; it never overwrites an unread element or waits.
// The caller owns element storage and must reclaim nontrivial objects outside
// the audio callback. This cursor does not destroy or retain any element.
template<unsigned Capacity> class SpscCursor
{
    static_assert(Capacity >= 2);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
public:
    struct Region { unsigned start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                    unsigned size() const noexcept { return size1 + size2; } };
    static constexpr unsigned usableCapacity = Capacity - 1;
    Region reserveWrite(unsigned requested) const noexcept
    {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        const auto ready = distance(read, write);
        return region(write, std::min(requested, usableCapacity - ready));
    }
    Region reserveRead(unsigned requested) const noexcept
    {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        return region(read, std::min(requested, distance(read, write)));
    }
    void commitWrite(unsigned count) noexcept
    {
        assert(count <= usableCapacity);
        write_.store((write_.load(std::memory_order_relaxed) + count) % Capacity, std::memory_order_release);
    }
    void commitRead(unsigned count) noexcept
    {
        assert(count <= usableCapacity);
        read_.store((read_.load(std::memory_order_relaxed) + count) % Capacity, std::memory_order_release);
    }
    // Lifecycle boundary only: both producer and consumer must be stopped.
    void reset() noexcept { read_.store(0, std::memory_order_relaxed); write_.store(0, std::memory_order_relaxed); }
    int getNumReady() const noexcept
    { return static_cast<int>(distance(read_.load(std::memory_order_acquire), write_.load(std::memory_order_acquire))); }
    int getFreeSpace() const noexcept { return static_cast<int>(usableCapacity) - getNumReady(); }
    void prepareToWrite(int count, int& start1, int& size1, int& start2, int& size2) const noexcept
    {
        const auto region = reserveWrite(static_cast<unsigned>(std::max(0, count)));
        start1 = static_cast<int>(region.start1); size1 = static_cast<int>(region.size1);
        start2 = static_cast<int>(region.start2); size2 = static_cast<int>(region.size2);
    }
    void prepareToRead(int count, int& start1, int& size1, int& start2, int& size2) const noexcept
    {
        const auto region = reserveRead(static_cast<unsigned>(std::max(0, count)));
        start1 = static_cast<int>(region.start1); size1 = static_cast<int>(region.size1);
        start2 = static_cast<int>(region.start2); size2 = static_cast<int>(region.size2);
    }
    void finishedWrite(int count) noexcept { commitWrite(static_cast<unsigned>(count)); }
    void finishedRead(int count) noexcept { commitRead(static_cast<unsigned>(count)); }
private:
    static unsigned distance(unsigned read, unsigned write) noexcept
    { return (write + Capacity - read) % Capacity; }
    static Region region(unsigned start, unsigned count) noexcept
    {
        const auto first = std::min(count, Capacity - start);
        return {start, first, 0, count - first};
    }
    alignas(64) std::atomic<unsigned> read_{0};
    alignas(64) std::atomic<unsigned> write_{0};
};
}
