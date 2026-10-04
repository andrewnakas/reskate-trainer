#pragma once
// No comply and boneless heights. The game's trick scripts launch these two through its
// jump-trajectory setter with a fixed upward speed, and no tuning value is read for it:
// the trainer multiplies that speed as the trajectory is set.
#include <cstdint>

namespace dingosdk::trainer {
enum class Trick : std::uint8_t { none, no_comply, boneless };
struct TrickLaunch {
    Trick trick{};
    float up_speed{}, factor{1}; // the game's own upward speed, and what it was multiplied by
};
// Installs the hook once the game's code is recognised. Safe to call every tick.
bool start_trick_heights(std::uintptr_t base) noexcept;
// Height multipliers (1: the game's own height). The launch speed is scaled by their roots.
void set_trick_heights(float no_comply, float boneless) noexcept;
// The scripted launches since the last call, oldest first; false when there are none.
bool take_trick_launch(TrickLaunch &out) noexcept;
} // namespace dingosdk::trainer
