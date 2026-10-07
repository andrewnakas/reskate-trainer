#include "Extension/Trainer/trainer_feel.h"
#include <bit>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dingosdk::trainer;
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
Row row(std::string id, double stock) { Row r; r.id = std::move(id); r.value = r.stock = stock; r.used = true; return r; }
int main() {
    try {
        std::vector<Row> rows{row("PhysicsMode.GrindLockDist", .9f), row("PhysicsGrindsAir.MaxDistBoardSlide", .75f),
            row("PhysicsGrindsAir.MaxDistTipSlide", .7f), row("PhysicsGrind.CommonFrictionScalar", .35f),
            row("PhysicsMode.GrindJumpCommonMax", 2.3f), row("unknown", 99), row("PhysicsPush.MaxPushableSpeed", 8)};
        const auto hard = workshop::preview(rows, -1);
        require(hard.size() == 6, "Only supported controls participate");
        for (std::size_t i = 0; i < 3; ++i) require(hard[i].after == static_cast<float>(rows[i].stock * .1), "Hardcore uses the old dial minimum for all grind capture distances");
        require(hard[3].after == static_cast<float>(rows[3].stock * 1.4), "Hardcore retains stronger friction");
        require(hard[4].after == static_cast<float>(rows[4].stock * .75), "Hardcore reduces grind pop");
        rows[0].frozen = true; rows[1].preset_locked = true;
        const auto locked = workshop::preview(rows, -1);
        require(locked[0].locked && locked[1].locked && !locked[2].locked, "Direct and linked locks are visible in review");
        rows[2].used = false; rows[3].detail = true;
        require(workshop::preview(rows, -1).size() == 4, "Unavailable and detail controls are not inferred");
        require(workshop::preview(rows, 2).empty() && workshop::preview(rows, std::numeric_limits<double>::quiet_NaN()).empty(), "Reject invalid mixer values");
        const auto stock = workshop::preview(rows, 0);
        for (const auto &change : stock) {
            const auto found = std::ranges::find(rows, change.id, &Row::id);
            require(change.after == static_cast<float>(found->stock), "Stock restores precisely its curated controls");
        }
        const auto &profiles = workshop::presets();
        require(profiles.size() == 4 && profiles[0].name == "Hardcore", "Ported named profiles survive independently of the mixer");
        for (const auto &profile : profiles) {
            const auto plan = workshop::preview(rows, *workshop::amount(profile.name));
            for (const auto &change : plan) {
                const auto source = std::ranges::find(rows, change.id, &Row::id);
                const auto rule = std::ranges::find(profile.rules, [&] { auto key = change.id; for(auto &c:key)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return key; }(), &PresetRule::pattern);
                require(rule != profile.rules.end() && rule->exact, "Named presets use the same exact rules as the preview");
                require(static_cast<float>(source->stock * rule->amount) == change.after, "Named profile and mixer agree");
            }
        }
        for (const double value : {0.1, 0.4050000011920929, 1.0000000000000002, 1e10, -1e-12, std::numeric_limits<double>::max()}) {
            const auto token = workshop::number_token(value); double decoded{};
            require(token.has_value(), "Finite transport exists");
            const auto parsed = std::from_chars(token->data(), token->data()+token->size(), decoded);
            require(parsed.ec == std::errc{} && decoded == value, "Numeric commands preserve exact doubles");
        }
        require(!workshop::number_token(std::numeric_limits<double>::infinity()), "Reject nonfinite numeric input");
        std::cout << "Feel profiles, grind defaults, locks and numeric transport passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
