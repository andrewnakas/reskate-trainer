#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace dingosdk::trainer::physics_policy {
// These are ownership keys, not dereferenceable pointers. The physics hook supplies the
// validated core before any carry or energy measurement is allowed to cross a step.
struct MotionOwner {
    std::uintptr_t client{}, entity{}, core{};
    bool operator==(const MotionOwner &) const = default;
};
struct PushCarry {
    MotionOwner owner{};
    int pushing{}, since_push{};
    bool carried{};
    float reached{};
    void clear(int hold_steps) noexcept {
        *this = {};
        since_push = hold_steps;
    }
    bool bind(MotionOwner next, int hold_steps) noexcept {
        if (owner == next) return false;
        clear(hold_steps);
        owner = next;
        return true;
    }
};
struct PumpAccumulator {
    MotionOwner owner{};
    std::uint32_t count{};
    bool primed{}, running{};
    float energy{}, sum{}, credit{};
    void clear() noexcept { *this = {}; }
    bool bind(MotionOwner next) noexcept {
        if (owner == next) return false;
        clear();
        owner = next;
        return true;
    }
};
struct RevertHistory {
    MotionOwner owner{};
    bool primed{};
    std::uint64_t seen{}, cooldown_until{};
    void clear() noexcept { *this = {}; }
    bool bind(MotionOwner next) noexcept {
        if (owner == next) return false;
        clear();
        owner = next;
        return true;
    }
    bool accept(std::uint64_t sequence) noexcept {
        if (!primed) {
            primed = true;
            seen = sequence; // the first landing predates this owner or enabling the boost
            return false;
        }
        if (sequence == seen) return false;
        seen = sequence;
        return true;
    }
};
enum class StepOwnership { unrelated, unchanged, changed };
template <class History, class... Args>
StepOwnership bind_step(History &history, MotionOwner owner, std::uintptr_t step_core, Args... args) noexcept {
    // The shared velocity hook also runs for other physics cores. Those callbacks do not
    // advance or erase the local skater's history, even while a fresh owner is being found.
    if (!owner.client || !owner.entity || !owner.core || owner.core != step_core) return StepOwnership::unrelated;
    return history.bind(owner, args...) ? StepOwnership::changed : StepOwnership::unchanged;
}
inline bool finite_values(std::initializer_list<float> values) noexcept {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}
inline float revert_strength(float value) noexcept {
    return std::isfinite(value) && value > 0 ? std::min(value, 1000.0f) : 0.0f;
}
inline float push_factor(float value) noexcept {
    return std::isfinite(value) && value > 0.02f && value <= 1.0e6f ? value : 1.0f;
}
inline float push_stock(float value) noexcept {
    return std::isfinite(value) && value > 1 && value < 100 ? value : 9.25f;
}
inline float cruise_speed(float value) noexcept {
    return std::isfinite(value) && value > 0 && value < 100 ? value : 0.0f;
}
inline float pump_factor(float value) noexcept {
    return std::isfinite(value) && value >= 0 && value <= 1.0e6f ? value : 1.0f;
}
inline float bound_reached(float reached, float ceiling) noexcept {
    if (!std::isfinite(reached) || !std::isfinite(ceiling) || ceiling <= 0) return 0;
    return std::clamp(reached, 0.0f, ceiling);
}
// A saved push target can never keep accelerating toward a superseded, higher setting.
inline float carry_gain(PushCarry &carry, float speed, float ceiling, float gain) noexcept {
    carry.reached = bound_reached(carry.reached, ceiling);
    if (!std::isfinite(speed) || speed < 0 || !std::isfinite(gain) || gain <= 0) return 0;
    return std::clamp(carry.reached - speed, 0.0f, gain);
}
// The caller supplies the global speed already selected by the session/catch policy.
// Keep a saved individual speed dormant during timed catch so it cannot finish the flip
// ahead of the chosen point in the flight; switching catch off restores that same value.
inline float effective_flip_factor(float global, float individual, bool advanced, bool timed_catch, bool blocked) noexcept {
    if (!std::isfinite(global) || global <= 0) global = 1;
    if (!std::isfinite(individual) || individual <= 0) individual = 1;
    return advanced && !timed_catch && !blocked ? global * individual : global;
}

struct FlipOwner {
    std::uintptr_t base{}, entity{}, block{}, speed_at{}, trick_at{};
    std::uint32_t speed_handle{}, trick_handle{};
    bool operator==(const FlipOwner &) const = default;
    explicit operator bool() const noexcept { return base && entity && block && speed_at && trick_at; }
};
struct FlipObservation {
    FlipOwner owner{};
    std::uint32_t trick{};
    float speed{};
};
inline bool flip_speed_valid(float speed) noexcept { return std::isfinite(speed) && speed >= 0 && speed <= 1000; }
inline bool same_flip_target(const FlipObservation &expected, const FlipObservation &current) noexcept {
    return expected.owner && current.owner && expected.owner == current.owner && expected.trick == current.trick;
}
// Used both before a changed speed is written and before the trainer restores a baseline.
// Re-reading the owner-derived live state is mandatory; matching a float at an old address
// alone does not establish that it is still this skater's animation state.
inline bool can_write_flip(const FlipObservation &expected, const FlipObservation &current, float desired) noexcept {
    return same_flip_target(expected, current) && flip_speed_valid(expected.speed) && flip_speed_valid(current.speed) &&
           current.speed == expected.speed && flip_speed_valid(desired);
}
}
