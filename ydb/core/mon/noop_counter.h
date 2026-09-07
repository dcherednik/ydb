#pragma once

#include <library/cpp/monlib/dynamic_counters/counters.h>

#include <type_traits>
#include <utility>

namespace NKikimr {

// A counter handle used by the atomic-counter performance experiment.  It keeps
// the counter registered (so monitoring schemas and counter pointers stay
// intact), but makes updates compile down to no-ops.
class TNoopCounter {
public:
    using TCounterPtr = ::NMonitoring::TDynamicCounters::TCounterPtr;
    using TValueBase = ::NMonitoring::TDeprecatedCounter::TValueBase;

    TNoopCounter() = default;

    TNoopCounter(TCounterPtr counter)
        : Counter(std::move(counter))
    {}

    template <typename T>
    TNoopCounter& operator=(T&& value) {
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, TCounterPtr>) {
            Counter = std::forward<T>(value);
        }
        return *this;
    }

    explicit operator bool() const {
        return static_cast<bool>(Counter);
    }

    operator TValueBase() const {
        return 0;
    }

    bool operator!() const {
        return !Counter;
    }

    operator const TCounterPtr&() const {
        return Counter;
    }

    const TCounterPtr& GetCounterPtr() const {
        return Counter;
    }

    TNoopCounter* operator->() {
        return this;
    }

    const TNoopCounter* operator->() const {
        return this;
    }

    TNoopCounter& operator*() {
        return *this;
    }

    const TNoopCounter& operator*() const {
        return *this;
    }

    TValueBase Inc() const { return 0; }
    TValueBase Dec() const { return 0; }
    TValueBase Add(TValueBase) const { return 0; }
    TValueBase Sub(TValueBase) const { return 0; }
    void Set(TValueBase) const {}

    void operator++() const {}
    void operator++(int) const {}
    void operator--() const {}
    void operator--(int) const {}
    void operator+=(TValueBase) const {}
    void operator-=(TValueBase) const {}

    TValueBase Val() const {
        return 0;
    }

    TValueBase Get() const {
        return Val();
    }

private:
    TCounterPtr Counter;
};

} // namespace NKikimr

template <>
inline void Out<NKikimr::TNoopCounter>(
        IOutputStream& out, TTypeTraits<NKikimr::TNoopCounter>::TFuncParam value) {
    Y_UNUSED(value);
    Out<ui64>(out, 0);
}
