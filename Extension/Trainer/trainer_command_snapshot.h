#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <type_traits>

namespace dingosdk::trainer::physics_policy {
// A tiny mailbox between the game and physics threads. Publish the complete command in
// one critical section; never hold this lock while looking up or writing game bodies.
template <class T> class CommandSnapshot {
    static_assert(std::is_trivially_copyable_v<T>);
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    T value_{};
public:
    T load() const noexcept {
        AcquireSRWLockShared(&lock_);
        const T snapshot = value_;
        ReleaseSRWLockShared(&lock_);
        return snapshot;
    }
    void store(const T &command) noexcept {
        AcquireSRWLockExclusive(&lock_);
        value_ = command;
        ReleaseSRWLockExclusive(&lock_);
    }
};
}
