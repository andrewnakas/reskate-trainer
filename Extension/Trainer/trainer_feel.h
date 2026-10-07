#pragma once
#include "trainer_presets.h"
#include <optional>
#include <span>

namespace dingosdk::trainer::workshop {
struct Change { std::string id; double before{}, after{}; bool locked{}; };
// -1 Hardcore, 0 Stock, +1 Arcade. Pure preview; the game thread applies it.
std::vector<Change> preview(std::span<const Row> rows, double amount);
std::optional<double> amount(std::string_view name);
const std::vector<BuiltinPreset> &presets();
// Lossless command transport, independent of the compact widget label.
std::optional<std::string> number_token(double value);
inline constexpr double minimum_grind_assist = .1;
}
