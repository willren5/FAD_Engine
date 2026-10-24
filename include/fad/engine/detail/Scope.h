#pragma once
#include <utility>

namespace fad::engine::detail
{
template<class Action> class ScopeExit
{
public:
    explicit ScopeExit(Action action) : action_(std::move(action)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() noexcept { action_(); }
private:
    Action action_;
};
template<class Value> class ScopedValue
{
public:
    ScopedValue(Value& destination, Value temporary) noexcept
        : destination_(destination), original_(destination) { destination_ = temporary; }
    ScopedValue(const ScopedValue&) = delete;
    ScopedValue& operator=(const ScopedValue&) = delete;
    ~ScopedValue() noexcept { destination_ = original_; }
private:
    Value& destination_;
    Value original_;
};
}
