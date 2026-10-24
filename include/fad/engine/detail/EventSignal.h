#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace fad::engine::detail
{
// Auto-reset notification for preparation/control/retirement workers only.
// Neither wait nor signal may be called by the audio callback.
class EventSignal
{
public:
    void signal() const noexcept
    {
        { const std::lock_guard lock(mutex_); signalled_ = true; }
        condition_.notify_all();
    }
    bool wait(int milliseconds = -1) const noexcept
    {
        std::unique_lock lock(mutex_);
        const auto ready = [&] { return signalled_; };
        if (milliseconds < 0) condition_.wait(lock, ready);
        else if (!condition_.wait_for(lock, std::chrono::milliseconds(milliseconds), ready)) return false;
        signalled_ = false;
        return true;
    }
private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable bool signalled_ = false;
};

#if defined(FAD_ENGINE_TEST_HOOKS) && FAD_ENGINE_TEST_HOOKS
// Borrow a test runner's rendezvous without exposing its framework in the
// core. These hooks do not exist in the installed production variant.
class TestEventRef
{
public:
    TestEventRef() noexcept = default;
    TestEventRef(std::nullptr_t) noexcept {}
    template<class Event> TestEventRef(Event* event) noexcept : owner_(event)
    {
        if (event != nullptr)
        {
            signal_ = [](void* owner) noexcept { static_cast<Event*>(owner)->signal(); };
            wait_ = [](void* owner, int timeout) noexcept { return static_cast<Event*>(owner)->wait(timeout); };
        }
    }
    bool operator!=(std::nullptr_t) const noexcept { return owner_ != nullptr; }
    const TestEventRef* operator->() const noexcept { return this; }
    void signal() const noexcept { if (owner_ != nullptr) signal_(owner_); }
    bool wait(int timeout = -1) const noexcept { return owner_ != nullptr && wait_(owner_, timeout); }
private:
    void* owner_ = nullptr;
    void (*signal_)(void*) noexcept = nullptr;
    bool (*wait_)(void*, int) noexcept = nullptr;
};
#endif
}
