#include "trainer_feel.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>

namespace dingosdk::trainer::workshop {
namespace {
std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
struct Rule { std::string_view id; double hard, easy; bool flag{}; };
// Port of the workshop's curated profiles. Tighten capture distances together;
// entry speed/angle limits are independent and are deliberately not scaled.
constexpr Rule rules[]{
    {"physicsmode.jumpmaxheight", .65, 1.8}, {"physicsmode.jumpminheight", .7, 1.5},
    {"physicsmode.grindjumpcommonmax", .75, 1.6}, {"physicsmode.grindjumpcommonmin", .75, 1.6},
    {"physicsmode.grindjumpboardslidemax", .75, 1.6}, {"physicsmode.grindjumpboardslidemin", .75, 1.6},
    {"physicsmode.grindjumptipslidemax", .75, 1.6}, {"physicsmode.grindjumptipslidemin", .75, 1.6},
    {"physicspush.maxpushablespeed", .8, 1.5}, {"physicsmode.speedwobblestartspeed", .65, 2.0},
    {"physicsreckoning.flipscalar", .8, 1.5}, {"physicsreckoning.flipmaxspeed", .8, 1.5},
    {"physicsairstates.maxspinspeed", .75, 1.5}, {"physicsmode.maxautobodyspinspeed", .75, 1.5},
    {"physicsmode.grindlockdist", minimum_grind_assist, 2},
    {"physicsgrindsair.maxdistboardslide", minimum_grind_assist, 2},
    {"physicsgrindsair.maxdisttipslide", minimum_grind_assist, 2},
    {"physicsgrind.commonfrictionscalar", 1.4, .55}, {"physicsgrind.curbfrictionscalar", 1.4, .55},
    {"physicsmode.wipeout_groundxzacceleration", .7, 2},
    {"wipeoutfallspeed.normalmaxspeed_ground", .8, 1.8}, {"wipeoutfallspeed.normalmaxspeed_grind", .8, 1.8},
    {"physicsmode.wipeoutcheckforbadlanding", 1, 0, true}, {"physicsmode.easybodyspins", 0, 1, true},
    {"jump.basejumpheight", .85, 1.5}, {"jump.maxjumpheight", .85, 1.5}, {"locomotion.sprintspeed", .9, 1.3},
};
double target(const Rule &rule, double stock, double value) {
    if (rule.flag) return value <= -.5 ? rule.hard : value >= .5 ? rule.easy : stock;
    return stock * std::pow(value < 0 ? rule.hard : rule.easy, std::abs(value));
}
}
std::vector<Change> preview(std::span<const Row> rows, double value) {
    std::vector<Change> result;
    if (!std::isfinite(value) || value < -1 || value > 1) return result;
    for (const auto &row : rows) {
        if (!row.used || row.detail || !std::isfinite(row.stock)) continue;
        const auto rule = std::ranges::find(rules, lower(row.id), &Rule::id);
        if (rule == std::end(rules)) continue;
        auto after = target(*rule, row.stock, value);
        if (row.kind == Kind::integer) after = std::round(after);
        if (row.kind == Kind::real) after = static_cast<float>(after); // native Float32 precision
        if (!std::isfinite(after)) continue;
        result.push_back({row.id, row.value, after, row.frozen || row.preset_locked});
    }
    return result;
}
std::optional<double> amount(std::string_view text) {
    const auto name = lower(text);
    if (name == "hardcore") return -1;
    if (name == "authentic") return -.5;
    if (name == "stock") return 0;
    if (name == "accessible") return .5;
    if (name == "arcade") return 1;
    return std::nullopt;
}
const std::vector<BuiltinPreset> &presets() {
    static const auto profiles = [] {
        std::vector<BuiltinPreset> result;
        const std::array<std::pair<std::string_view, double>, 4> levels{{{"Hardcore", -1}, {"Authentic", -.5}, {"Accessible", .5}, {"Arcade", 1}}};
        for (const auto &[name, value] : levels) {
            BuiltinPreset preset{name, "Curated stock-relative feel. Preview in Feel; locks and unrelated edits stay.", {}};
            for (const auto &rule : rules) {
                if (rule.flag) preset.rules.push_back({rule.id, false, target(rule, 0, value), false, true});
                else preset.rules.push_back({rule.id, true, target(rule, 1, value), false, true});
            }
            result.push_back(std::move(preset));
        }
        return result;
    }();
    return profiles;
}
std::optional<std::string> number_token(double value) {
    if (!std::isfinite(value)) return std::nullopt;
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) return std::nullopt;
    return std::string(buffer.data(), result.ptr);
}
}
