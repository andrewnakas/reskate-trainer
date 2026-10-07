#include "trainer_page.h"
#include "trainer.h"
#include "trainer_feel.h"
#include "trainer_widgets.h"
#include <imgui_internal.h>
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <format>
#include <string>
#include <vector>

// The TRAINER page and the skating HUD. Presentation thread: reads the trainer's snapshots
// and queues `trainer ...` console commands; it never touches the game.
namespace dingosdk::overlay::menu {
namespace {
struct Page {
    int tab{}, tools_tab{}; // Feel, Settings, Presets, Fun, Tools
    float difficulty{};
    bool confirm_reset{}, confirm_tricks{};
    int restrictions{};
    std::string confirm_delete, confirm_save, review_preset, map;
    std::array<char, 96> search{};
    int mode{2}; // retained value-list filter: 2 is every group
    int group{1}; // Everything: 1: every group, 2..: one of the view's groups
    bool only_changed{}, graph_points{}, show_unused{};
    std::size_t hidden_unused{}; // rows the filters would show but for "no use found"
    // The rows the filters leave, rebuilt when the snapshot or a filter changes.
    std::vector<std::size_t> shown;
    std::uint64_t shown_revision{};
    std::string shown_key;
    // The value under the pointer while it is dragged: the snapshot lags a frame or two.
    std::string active;
    double active_value{};
    std::array<char, 49> preset_name{};
    std::array<float, 3> teleport{};
    double speed_edit{-1}, speed_until{};
    float hippy_edit{1}, nocomply_edit{1}, boneless_edit{1}, offboard_edit{1}, flip_edit{1};
    float revert_edit{};
    bool revert_editing{};
    // The Tricklining list: rows of the value table per group, for one snapshot.
    std::array<std::vector<std::size_t>, 5> trick_rows;
    std::uint64_t trick_revision{~0ull};
    bool hippy_editing{}, nocomply_editing{}, boneless_editing{}, offboard_editing{}, flip_editing{};
    std::uint64_t open_serial{}; // the last `trainer open` acted on
    std::uint64_t share_serial{}; // the last `preset export` put on the clipboard
    bool share_seen{};
    bool show_page{};
};
void trick_heights(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const std::string &words);
void feel_buttons(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::View &view);
void trickline_section(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view);
Page &page() {
    static auto *value = new Page;
    return *value;
}
// A tooltip that wraps at a readable width instead of running off the screen as one line.
void wrapped_tooltip(const char *format, ...) {
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    va_list arguments;
    va_start(arguments, format);
    ImGui::TextV(format, arguments);
    va_end(arguments);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
// What the control just drawn does, shown while the pointer (or the controller's focus) is on it.
void tip(const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || (ImGui::IsItemFocused() && GImGui->NavCursorVisible)) wrapped_tooltip("%s", text);
}
std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
bool contains_words(const std::string &haystack, const std::string &words) {
    std::size_t start = 0;
    while (start < words.size()) {
        auto end = words.find(' ', start);
        if (end == std::string::npos) end = words.size();
        if (end > start && haystack.find(words.substr(start, end - start)) == std::string::npos) return false;
        start = end + 1;
    }
    return true;
}
std::string display_number(double value) { return std::format("{:.6g}", value); }
std::string number(double value) { return trainer::workshop::number_token(value).value_or("nan"); }
void trainer_command(SkateMenu &menu, const CallbacksV3 &callbacks, const std::string &text) {
    send_console(menu, callbacks, "trainer " + text);
}

void filter_rows(Page &p, const trainer::View &view) {
    const auto words = lower(p.search.data());
    const auto key = std::format("{}|{}|{}|{}|{}|{}", words, p.group, p.only_changed, p.graph_points, p.show_unused, p.mode);
    if (p.shown_revision == view.revision && p.shown_key == key) return;
    p.shown_revision = view.revision;
    p.shown_key = key;
    p.shown.clear();
    p.hidden_unused = 0;
    const std::string *group = p.mode == 2 && p.group > 1 && static_cast<std::size_t>(p.group) <= view.groups.size() + 1
        ? &view.groups[static_cast<std::size_t>(p.group) - 2] : nullptr;
    const bool essentials = p.mode < 2 && words.empty(); // searching looks through every value, whatever the list
    const std::uint8_t list = p.mode == 0 ? trainer::mode_realistic : trainer::mode_fun;
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const auto &row = view.rows[i];
        if (essentials) {
            // Only what the game was found or seen to read: a short list has no room for maybes.
            if (!row.friendly.empty() && (row.modes & list) && (row.used || (row.modes & trainer::mode_trial))) p.shown.push_back(i);
            continue;
        }
        if (row.detail && !p.graph_points && !row.touched) continue;
        if (p.only_changed && !row.touched && !row.frozen) continue;
        // Searching looks through every group.
        if (words.empty() ? (group && row.group != *group) : !contains_words(lower(row.id + " " + row.label + " " + row.friendly), words))
            continue;
        // A value nothing in the game reads would be a slider that does nothing.
        if (!row.used && !p.show_unused && !row.touched && !row.frozen) {
            ++p.hidden_unused;
            continue;
        }
        p.shown.push_back(i);
    }
    if (essentials) std::ranges::sort(p.shown, {}, [&](std::size_t i) { return view.rows[i].rank; });
}

void value_row(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const trainer::Row &row,
               bool with_group, bool friendly) {
    ImGui::PushID(row.id.c_str());
    bool frozen = row.frozen;
    ImGui::BeginDisabled(!callbacks.queue_console_command);
    if (ImGui::Checkbox("##freeze", &frozen)) trainer_command(menu, callbacks, std::format("freeze {} {}", row.id, frozen ? 1 : 0));
    tip("Lock this value against presets, feel changes and resets. Linked child values inherit the lock.");
    ImGui::EndDisabled(); ImGui::SameLine();
    const auto label = friendly && !row.friendly.empty() ? row.friendly : with_group ? row.group + " / " + row.label : row.label;
    const auto at = ImGui::GetCursorScreenPos();
    const auto width = ImGui::GetContentRegionAvail().x;
    const auto height = ImGui::GetFrameHeight();
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(at.x, at.y + ImGui::GetStyle().FramePadding.y),
        ImVec2(at.x + width, at.y + height), at.x + width, at.x + width, label.c_str(), nullptr, nullptr);
    ImGui::Dummy(ImVec2(width, height));
    tip(std::format("{}\n{}\n{}", label, row.id, row.help).c_str());
    ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command);
    double value = p.active == row.id ? p.active_value : row.value;
    bool finished{};
    if (row.kind == trainer::Kind::flag) {
        bool on = value != 0;
        if (ImGui::Checkbox("Enabled", &on)) { value = on ? 1 : 0; finished = true; }
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool scale = row.kind == trainer::Kind::curve || row.kind == trainer::Kind::graph;
        const bool whole = row.kind == trainer::Kind::integer;
        const double low = scale ? -1e9 : -1e12, high = scale ? 1e9 : 1e12;
        const auto speed = whole ? .1f : static_cast<float>(std::max({std::abs(row.stock), std::abs(value), .01}) * .004);
        const auto input = scalar_input("##value", whole ? "%.0f" : scale ? "%.3fx" : "%.6g");
        if (ImGui::DragScalar("##value", ImGuiDataType_Double, &value, speed, &low, &high, input.format, ImGuiSliderFlags_AlwaysClamp)) {
            p.active = row.id; p.active_value = value;
        }
        keep_scalar_visible();
        if (input.cancelled) p.active.clear();
        else if (ImGui::IsItemDeactivatedAfterEdit() && p.active == row.id) { finished = true; p.active.clear(); }
        tip("Drag to edit. Ctrl-click or double-click to type precisely. Enter commits; Escape cancels. Changes apply when editing finishes.");
    }
    if (finished) trainer_command(menu, callbacks, "set " + row.id + " " + number(value));
    ImGui::BeginDisabled(!row.touched || row.preset_locked || row.frozen);
    if (ImGui::Button("Reset")) { trainer_command(menu, callbacks, "reset " + row.id); p.active.clear(); }
    ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::SameLine();
    const auto status = std::format("Stock {}{}{}", display_number(row.stock), row.touched ? " / Changed" : "", (row.frozen || row.preset_locked) ? " / Locked" : !row.used ? " / Unverified" : "");
    const auto status_at = ImGui::GetCursorScreenPos(); const auto status_width = ImGui::GetContentRegionAvail().x;
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(status_at.x, status_at.y + ImGui::GetStyle().FramePadding.y),
        ImVec2(status_at.x + status_width, status_at.y + height), status_at.x + status_width, status_at.x + status_width, status.c_str(), nullptr, nullptr);
    ImGui::Dummy(ImVec2(status_width, height)); tip(status.c_str());
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); ImGui::PopID();
}

void dial_row(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::PresetRow &preset) {
    ImGui::PushID(preset.name.c_str());
    ImGui::Spacing(); ImGui::TextWrapped("%s", preset.title.c_str());
    const auto id = "dial:" + preset.name;
    double value = p.active == id ? p.active_value : preset.factor;
    const double low = .1, high = std::max(5.0, preset.factor);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const auto input = scalar_input("##dial", "%.2fx stock");
    if (ImGui::SliderScalar("##dial", ImGuiDataType_Double, &value, &low, &high, input.format,
            ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) { p.active = id; p.active_value = value; }
    keep_scalar_visible();
    if (input.cancelled) p.active.clear();
    else if (ImGui::IsItemDeactivatedAfterEdit() && p.active == id) {
        trainer_command(menu, callbacks, "dial " + number(value) + " " + preset.name); p.active.clear();
    }
    tip(std::format("{}\n1x is stock. Ctrl-click to type an exact multiplier; Escape cancels. Preset shortcuts are in Fun.", preset.note).c_str());
    ImGui::BeginDisabled(std::abs(preset.factor - 1) < 1e-6);
    if (ImGui::Button("Restore stock")) { trainer_command(menu, callbacks, "dial 1 " + preset.name); p.active.clear(); }
    ImGui::EndDisabled(); ImGui::Spacing(); ImGui::PopID();
}

// A built-in preset as a switch: blue while every value it sets holds what it sets.
void preset_switch(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::PresetRow &preset) {
    if (preset.active) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
    const bool pressed = ImGui::Button(preset.name.c_str());
    if (preset.active) ImGui::PopStyleColor();
    if (pressed) trainer_command(menu, callbacks, std::string(preset.active ? "preset remove " : "preset apply ") + preset.name);
    if (ImGui::IsItemHovered() && !preset.note.empty()) wrapped_tooltip("%s", preset.note.c_str());
}
// Native workshop layout, sharing the trainer's authoritative state and commands.
void workshop_heading(SkateMenu &menu, const char *title, const char *help = nullptr) {
    ImGui::Spacing(); ImGui::PushFont(menu.bold); ImGui::TextUnformatted(title); ImGui::PopFont();
    if (help) note(help);
    ImGui::Spacing();
}
void workshop_navigation(SkateMenu &menu, int &selected, std::initializer_list<const char *> labels, const char *id) {
    ImGui::PushID(id);
    const int columns = ImGui::GetContentRegionAvail().x >= px(520) ? static_cast<int>(labels.size()) : std::min(3, static_cast<int>(labels.size()));
    const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (columns - 1)) / columns;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        if (i % columns) ImGui::SameLine();
        const bool active = selected == i;
        if (active) skate_theme::push_primary_button();
        if (ImGui::Button(labels.begin()[i], ImVec2(width, px(36)))) {
            selected = i; menu.feedback.clear();
        }
        if (active) skate_theme::pop_primary_button();
    }
    ImGui::PopID(); ImGui::Spacing();
}
// Script controls use the same full-width, precise editors as the Feel dials.
void command_slider(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view,
                    const char *label, const char *id, std::string_view command, float current, float low, float high,
                    const char *help, const char *format = "%.2fx", bool enabled = true) {
    ImGui::PushID(id);
    ImGui::Spacing(); ImGui::TextWrapped("%s", label); tip(help);
    const std::string key = std::string("option:") + id;
    float value = p.active == key ? static_cast<float>(p.active_value) : current;
    const bool editable = enabled && view.editable && !view.session_enforced && !view.boosts_blocked && callbacks.queue_console_command;
    ImGui::BeginDisabled(!editable);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const auto input = scalar_input("##option", format, "%.9g");
    if (ImGui::SliderFloat("##option", &value, low, high, input.format,
            high > 10 && low > 0 ? ImGuiSliderFlags_Logarithmic : 0)) {
        p.active = key; p.active_value = value;
    }
    keep_scalar_visible();
    if (input.cancelled || !editable) { if (p.active == key) p.active.clear(); }
    else if (ImGui::IsItemDeactivatedAfterEdit() && p.active == key) {
        if (std::isfinite(value)) trainer_command(menu, callbacks, std::string(command) + " " + number(value));
        else feedback(menu, "Enter a finite number.");
        p.active.clear();
    }
    tip(help); ImGui::EndDisabled(); ImGui::PopID();
}
void option_slider(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view,
                   const char *label, const char *option, float current, float low, float high,
                   const char *help, const char *format = "%.2fx", bool enabled = true) {
    command_slider(menu, callbacks, p, view, label, option, std::string("option ") + option,
                   current, low, high, help, format, enabled);
}
void workshop_flips(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const bool editable = view.editable && !view.session_enforced && !view.boosts_blocked && callbacks.queue_console_command;
    option_slider(menu, callbacks, p, view, "Board flip speed", "flip_speed", view.flip_speed, .1f, 3,
        "Changes board rotation without changing pop height. Below 1, slow flips stay slow automatically. Ctrl-click to type; Enter applies and Escape cancels. Accepted range: 0.01 to 100.", "%.2fx", !view.catch_at);
    if (view.catch_at) note("Automatic catch timing controls flip speed while it is on. Your custom speeds are kept.");
    else note("Slower flips need enough air to finish. The old slow-flip switch is now automatic.");
    if (!view.flip_live) note("Preparing animation controls after loading the map. This can take about 15 seconds; your settings are kept.");
    bool catch_at = view.catch_at;
    if (toggle_row(menu, "Automatic catch timing", "Catch a completed flip at a chosen point in the jump. This is scheduled assistance, not a manual catch.", catch_at, editable))
        trainer_command(menu, callbacks, std::format("option catch_at {}", catch_at ? 1 : 0));
    option_slider(menu, callbacks, p, view, "Catch point in the jump", "catch_percent", view.catch_percent, 1, 100,
        "Percent from pop to landing. Start around 70%; 55–100% is the useful range. The estimate assumes level ground: drops and gaps can catch earlier. At 100% you may land while still flipping.", "%.0f%% of airtime", catch_at);
    if (catch_at && !view.flip_gate_found) note("Catch timing is preparing after the map load.");
    bool advanced = view.flip_advanced;
    if (toggle_row(menu, "Per-trick flip speeds", "Give each flip its own multiplier, combined with Board flip speed. Nollie tricks use their regular trick's value. Stored values are kept when this is off.", advanced, editable))
        trainer_command(menu, callbacks, std::format("option flip_advanced {}", advanced ? 1 : 0));
    if (ImGui::CollapsingHeader("Individual flip speeds")) {
        note("Each value multiplies the main board flip speed. Automatic catch timing takes priority.");
        for (std::size_t i = 0; i < trainer::flip_tricks.size(); ++i) {
            const auto &trick = trainer::flip_tricks[i];
            option_slider(menu, callbacks, p, view, std::string(trick.label).c_str(), std::string(trick.key).c_str(), view.flip_trick[i], .1f, 3,
                "1 keeps the main speed. Ctrl-click to type a multiplier from 0.01 to 100. Enter applies; Escape cancels.", "%.2fx main speed", advanced && !catch_at);
        }
    }
}
void workshop_pumping(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    option_slider(menu, callbacks, p, view, "Pump power", "pump_power", view.pump_power, 0, 5,
        "1 keeps the game's pumping. Above 1 adds speed while pumping transitions; below 1 removes speed. 2 adds roughly one extra native pump's gain. Ctrl-click accepts 0 to 1000; large values are strong boosts.");
    if (!view.pump_live) note("Preparing transition pumping after the map load. This can take about 15 seconds.");
}

void workshop_feel(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    workshop_heading(menu, "Find your feel", "Choose a starting point, review its changes, then apply. Fine tuning and locks stay yours.");
    const std::array<const char *, 5> names{"Hardcore", "Authentic", "Stock", "Accessible", "Arcade"};
    const int columns = ImGui::GetContentRegionAvail().x < px(550) ? 3 : 5;
    const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (columns - 1)) / columns;
    for (int i = 0; i < 5; ++i) {
        if (i % columns) ImGui::SameLine();
        const auto amount = static_cast<float>(i) * .5f - 1;
        const bool selected = std::abs(p.difficulty - amount) < .005f;
        if (selected) skate_theme::push_primary_button();
        if (ImGui::Button(names[i], ImVec2(width, px(38)))) p.difficulty = amount;
        if (selected) skate_theme::pop_primary_button();
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##feel-mixer", &p.difficulty, -1, 1, "Custom feel: %.2f", ImGuiSliderFlags_AlwaysClamp);
    tip("Hardcore on the left; Stock in the middle; Arcade on the right. Selecting or moving this control only changes the preview.");
    const auto plan = trainer::workshop::preview(view.rows, p.difficulty);
    std::size_t changes{}, locked{};
    for (const auto &change : plan) { if (change.locked) ++locked; else if (change.before != change.after) ++changes; }
    note(std::format("Preview: {} changes, {} locked, {} supported controls.", changes, locked, plan.size()).c_str());
    if (primary_button(menu, "Apply this feel", view.editable && callbacks.queue_console_command && changes))
        trainer_command(menu, callbacks, "workshop " + number(p.difficulty));
    note("Hardcore: 0.1x grind capture, firmer friction, lower grind pop. Test the feel on familiar spots.");
    if (ImGui::CollapsingHeader("Review affected values")) {
        if (ImGui::BeginTable("feel-preview", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Current"); ImGui::TableSetupColumn("Preview"); ImGui::TableHeadersRow();
            for (const auto &change : plan) {
                const auto row = std::ranges::find(view.rows, change.id, &trainer::Row::id);
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", row == view.rows.end() ? change.id.c_str() : row->friendly.empty() ? row->label.c_str() : row->friendly.c_str());
                tip(change.id.c_str()); ImGui::TableNextColumn(); ImGui::TextUnformatted(display_number(change.before).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(change.locked ? "Locked" : display_number(change.after).c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing(); ImGui::Separator();
    workshop_heading(menu, "Fine tune your feel", "Drag a slider or Ctrl-click to type. Changes apply when editing finishes.");
    const auto dials = [&](std::initializer_list<const char *> names) {
        ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command);
        for (const auto *name : names) {
            const auto preset = std::ranges::find(view.presets, name, &trainer::PresetRow::name);
            if (preset != view.presets.end() && preset->dial) dial_row(menu, callbacks, p, *preset);
        }
        ImGui::EndDisabled();
    };
    if (ImGui::CollapsingHeader("Pop and airtime", ImGuiTreeNodeFlags_DefaultOpen)) dials({"Super Ollie", "Grind Pop", "Gravity"});
    if (ImGui::CollapsingHeader("Speed and rotations")) dials({"Fast", "Fast Spins", "Fast Flips"});
    if (ImGui::CollapsingHeader("Flips and catches")) workshop_flips(menu, callbacks, p, view);
    if (ImGui::CollapsingHeader("Transition pumping")) workshop_pumping(menu, callbacks, p, view);
    if (ImGui::CollapsingHeader("Landings and bails")) dials({"Hard To Bail"});
    if (ImGui::CollapsingHeader("Grinds and slides", ImGuiTreeNodeFlags_DefaultOpen)) {
        note("Capture assistance sets the distance for locking onto a rail or ledge. Entry speed and angle are separate.");
        const auto lock = std::ranges::find(view.rows, std::string("physicsmode.grindlockdist"), [](const auto &row) { return lower(row.id); });
        if (lock != view.rows.end() && lock->stock > 0) {
            ImGui::PushID("capture-assist");
            ImGui::TextUnformatted("Grind capture assistance");
            const double low = .1, high = std::max(2.0, lock->value / lock->stock);
            double value = p.active == "capture-assist" ? p.active_value : lock->value / lock->stock;
            ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command);
            ImGui::SetNextItemWidth(-FLT_MIN);
            const auto input = scalar_input("##assist", "%.2fx stock");
            if (ImGui::SliderScalar("##assist", ImGuiDataType_Double, &value, &low, &high, input.format, ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
                p.active = "capture-assist"; p.active_value = value;
            }
            if (input.cancelled) p.active.clear();
            else if (ImGui::IsItemDeactivatedAfterEdit() && p.active == "capture-assist") {
                trainer_command(menu, callbacks, "assist " + number(value)); p.active.clear();
            }
            tip("Adjusts rail, boardslide and nose/tail-slide capture distances together, relative to this map's stock. Locked values are skipped.");
            ImGui::EndDisabled();
            ImGui::TextDisabled("Rail capture: %.3f m (stock %.3f m)", lock->value, lock->stock);
            ImGui::PopID();
        }
        dials({"Slick Grinds", "Revert Friction"});
        if (ImGui::CollapsingHeader("Individual grind settings")) for (const auto &row : view.rows) {
            const auto key = lower(row.id);
            if (row.used && !row.detail && (key == "physicsmode.grindlockdist" || key == "physicsgrindsair.maxdistboardslide" ||
                key == "physicsgrindsair.maxdisttipslide" || key == "physicsgrind.commonfrictionscalar" || key == "physicsgrind.curbfrictionscalar" ||
                key == "physicsgrind.grind_tipslide_minangletoprimitive"))
                value_row(menu, callbacks, p, view, row, false, true);
        }
        note("Animations, grind input gestures and balance behaviour are still governed by the game. Tuning these values alone cannot reproduce real skating.");
    }
}
void workshop_settings(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    workshop_heading(menu, "Physics settings", "Search and edit individual controls. Locks keep a value through presets and resets.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search physics settings...", p.search.data(), p.search.size());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (p.group < 1) p.group = 1;
    const char *group = p.group > 1 && static_cast<std::size_t>(p.group) <= view.groups.size() + 1 ? view.groups[p.group - 2].c_str() : "Every group";
    if (ImGui::BeginCombo("##group", group)) {
        if (ImGui::Selectable("Every group", p.group == 1)) p.group = 1;
        for (std::size_t i = 0; i < view.groups.size(); ++i)
            if (ImGui::Selectable(view.groups[i].c_str(), p.group == static_cast<int>(i) + 2)) p.group = static_cast<int>(i) + 2;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Only changed or locked", &p.only_changed);
    if (ImGui::CollapsingHeader("Advanced filters")) {
        ImGui::Checkbox("Individual graph points", &p.graph_points);
        ImGui::Checkbox("Values with no use found", &p.show_unused);
        note("Unverified controls may have no effect. Individual graph values can interact with their parent multiplier.");
    }
    p.mode = 2; filter_rows(p, view); trainer::note_class_list_shown();
    note(std::format("{} settings shown, {} hidden as unverified.", p.shown.size(), p.hidden_unused).c_str());
    if (ImGui::CollapsingHeader("Reset and maintenance")) {
        ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command);
        if (ImGui::Button("Reset unlocked physics")) trainer_command(menu, callbacks, "reset all");
        tip("Restores physics values to stock; locked values and script-driven trick options remain.");
        if (ImGui::Button("Reset everything...")) p.confirm_reset = true;
        if (p.confirm_reset) {
            warn("Clear all locks and restore physics and trick settings? Your saved preset library stays.");
            if (ImGui::Button("Confirm full reset")) { trainer_command(menu, callbacks, "reset everything"); p.confirm_reset = false; p.active.clear(); }
            ImGui::SameLine(); if (ImGui::Button("Cancel reset")) p.confirm_reset = false;
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    ImGuiListClipper clipper;
    // Each row occupies a stable three-line height; multiline labels must not use a
    // guessed clipper height, so let ImGui measure a representative row.
    clipper.Begin(static_cast<int>(p.shown.size()));
    while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        value_row(menu, callbacks, p, view, view.rows[p.shown[static_cast<std::size_t>(i)]], !p.search[0] ? p.group <= 1 : true, true);
    if (p.shown.empty()) note("No settings match. Clear the search or broaden the filters.");
}
void workshop_presets(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    workshop_heading(menu, "Your presets", "Save a setup, review its values, or share it with another skater.");
    ImGui::TextUnformatted("Preset name"); ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##preset-name", p.preset_name.data(), p.preset_name.size());
    if (primary_button(menu, "Save current setup", view.editable && callbacks.queue_console_command && p.preset_name[0] && view.capture_changed)) {
        if (std::ranges::any_of(view.presets, [&](const auto &preset) { return !preset.builtin && lower(preset.name) == lower(p.preset_name.data()); }))
            p.confirm_save = p.preset_name.data();
        else trainer_command(menu, callbacks, std::string("preset save ") + p.preset_name.data());
    }
    if (!view.capture_changed) note("Change a physics value or trick setting before saving.");
    if (!p.confirm_save.empty()) {
        warn(("Replace the saved preset \"" + p.confirm_save + "\"?").c_str());
        ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command);
        if (ImGui::Button("Replace preset")) { trainer_command(menu, callbacks, "preset save " + p.confirm_save); p.confirm_save.clear(); }
        ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button("Keep original")) p.confirm_save.clear();
    }
    ImGui::Spacing(); ImGui::Separator();
    std::size_t count{};
    for (const auto &preset : view.presets) {
        if (preset.builtin) continue;
        ++count; ImGui::PushID(preset.name.c_str());
        ImGui::PushFont(menu.bold); ImGui::TextWrapped("%s", preset.name.c_str()); ImGui::PopFont();
        if (!preset.note.empty()) note(preset.note.c_str());
        ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command);
        if (preset.active) {
            if (ImGui::Button("Restore affected values")) trainer_command(menu, callbacks, "preset remove " + preset.name);
        } else if (ImGui::Button("Review preset")) p.review_preset = preset.name;
        if (p.review_preset == preset.name) {
            note("Requested values below. Locked controls stay; linked graphs follow their parent. Unsupported values are retained but skipped on this build.");
            std::size_t locked{}, unknown{};
            for (const auto &[key, value] : preset.values) {
                (void)value;
                const auto row = std::ranges::find_if(view.rows, [&](const auto &item) { return lower(item.id) == lower(key); });
                if (row != view.rows.end()) locked += row->frozen || row->preset_locked;
                else if (!key.starts_with("trick.")) ++unknown;
            }
            note(std::format("{} saved values / {} locked / {} unavailable", preset.values.size(), locked, unknown).c_str());
            if (primary_button(menu, "Apply reviewed preset", view.editable && callbacks.queue_console_command)) {
                trainer_command(menu, callbacks, "preset apply " + preset.name); p.review_preset.clear();
            }
            if (ImGui::Button("Cancel review")) p.review_preset.clear();
            if (ImGui::CollapsingHeader("Saved values", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("saved-values", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 3);
                    ImGui::TableSetupColumn("Requested"); ImGui::TableHeadersRow();
                    for (const auto &[key, value] : preset.values) {
                        const auto row = std::ranges::find_if(view.rows, [&](const auto &item) { return lower(item.id) == lower(key); });
                        ImGui::TableNextRow(); ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", row == view.rows.end() ? key.c_str() : row->friendly.empty() ? row->label.c_str() : row->friendly.c_str());
                        tip(key.c_str()); ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row != view.rows.end() && (row->frozen || row->preset_locked) ? "Locked" : display_number(value).c_str());
                    }
                    ImGui::EndTable();
                }
            }
        }
        if (ImGui::Button("Use on this map")) trainer_command(menu, callbacks, "profile set " + preset.name);
        ImGui::EndDisabled();
        if (ImGui::Button("Share preset")) trainer_command(menu, callbacks, "preset export " + preset.name);
        if (ImGui::Button("Delete...")) p.confirm_delete = preset.name;
        if (p.confirm_delete == preset.name) {
            warn("Delete this saved preset? Current tuning will stay.");
            if (ImGui::Button("Confirm delete")) { trainer_command(menu, callbacks, "preset delete " + preset.name); p.confirm_delete.clear(); }
            ImGui::SameLine(); if (ImGui::Button("Keep preset")) p.confirm_delete.clear();
        }
        ImGui::PopID(); ImGui::Spacing(); ImGui::Separator();
    }
    if (!count) note("Your saved setups will appear here. Give your current setup a name or import a shared preset.");
    workshop_heading(menu, "Import and share");
    if (ImGui::Button("Import from clipboard")) {
        const auto *text = ImGui::GetClipboardText();
        if (text && trainer::stage_import(text)) trainer_command(menu, callbacks, "preset import");
        else feedback(menu, "Copy a complete RST1 preset line first.");
    }
    note("Imports add to your library and never apply automatically. Existing names are preserved with a numbered copy.");
    if (ImGui::Button("Share current setup")) trainer_command(menu, callbacks, "preset export current");
    info(menu, "This map applies", view.profile_preset.empty() ? "No preset" : view.profile_preset);
    if (!view.profile_preset.empty() && ImGui::Button("Stop applying on this map")) trainer_command(menu, callbacks, "profile clear");
    if (ImGui::CollapsingHeader("Skate 3 reference presets")) {
        note("These retain the community trainer's Skate 3 tuning. They are separate from the curated Feel profiles.");
        ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command); feel_buttons(menu, callbacks, view); ImGui::EndDisabled();
    }
}
struct FunShortcut {
    const char *name, *label, *hint;
    std::size_t group;
};
constexpr FunShortcut fun_shortcuts[]{
    {"Super Ollie", "Super ollie", "Larger ollies and grind pops for clearing big gaps.", 0},
    {"Mega Pop", "Mega pop", "Raises held, quick and grind pops together.", 0},
    {"Grind Pop", "Higher grind pops", "Doubles pops out of grinds, boardslides and nose or tail slides.", 0},
    {"Gravity", "Stronger board gravity", "1.3x board gravity: quicker drops and less float.", 0},
    {"Fast Flips", "Faster body flips", "Faster front and back flips. Board kickflips stay unchanged.", 1},
    {"Fast Spins", "Faster body spins", "Triple body rotation speed for bigger spins.", 1},
    {"Fast", "Faster pushing", "Higher push targets and a later speed-wobble threshold.", 2},
    {"Auto Push", "Auto push", "Keeps an already rolling skater moving up to the auto-push speed.", 2},
    {"No Speed Wobble", "Delay speed wobble", "Raises wobble onset to 20x stock; it is not removed entirely.", 2},
    {"Smooth Surfaces", "Smooth surfaces", "Makes rough ground ride more smoothly without changing its appearance.", 2},
    {"Long Wheelbase", "Longer wheelbase", "Changes both truck positions. Takes effect on your next respawn.", 2},
    {"Sticky Grinds", "Easier grind entry", "Wider capture distances and more forgiving entry speed and angle.", 3},
    {"Slick Grinds", "Slick grinds", "Lower grind and curb friction to carry speed farther.", 3},
    {"Soft Landings", "Softer landings", "Raises landing-speed limits for larger drops and sideways landings.", 3},
    {"Hard To Bail", "Harder to bail", "Raises impact limits and disables the bad-landing check.", 3},
    {"Revert Friction", "More revert friction", "Reverts scrub three times as much speed. This does not add a boost.", 3},
    {"Moon Jump", "Moon jump", "Higher on-foot jumps; can combine with the off-board height multiplier.", 4},
    {"Fast On Foot", "Faster sprinting", "Raises on-foot sprint speed to 1.8x stock.", 4},
    {"Super Glide", "Super glide", "Lighter spread-eagle gravity and stronger air steering.", 4},
    {"Torpedo Boost", "Torpedo steering", "Stronger torpedo and falling air control.", 4},
    {"Board Mount", "Calmer board mount", "Experimental: lower roll-away speed when mounting from a jog or sprint.", 5},
    {"Easy Body Flips", "Easier body flips", "Experimental: lower the airtime needed to start a front or back flip.", 5},
    {"Deep Landings", "Deeper landings", "Experimental: more leg compression on landing.", 5},
};
bool fun_preset(const trainer::PresetRow &preset) {
    return preset.builtin && preset.name != "Stock" && !preset.name.starts_with("Skate 3") &&
           preset.name != "Realistic" && !trainer::workshop::amount(preset.name);
}
// Native selectable rows keep the menu's controller navigation, with a readable
// explanation and explicit state rather than a wall of undifferentiated buttons.
void fun_switch(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::View &view,
                const trainer::PresetRow &preset, const FunShortcut *shortcut) {
    const char *label = shortcut ? shortcut->label : preset.name.c_str();
    std::string hint = shortcut ? shortcut->hint : preset.note;
    if (view.boosts_blocked && preset.name == "Auto Push") hint = "Auto-push assistance is disabled by this host. Your stored setting is kept.";
    if (view.boosts_blocked && preset.name == "Fast") hint += " Extra push carry is disabled by this host.";
    const bool available = view.editable && !view.session_enforced && callbacks.queue_console_command &&
                           !(view.boosts_blocked && preset.name == "Auto Push");
    ImGui::BeginDisabled(!available);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float pad = px(12), block = px(62), gap = px(12);
    const float text_width = std::max(px(80), width - 2 * pad - block - gap);
    const float title_size = px(16), hint_size = px(14);
    const float title_height = menu.bold->CalcTextSizeA(title_size, FLT_MAX, text_width, label).y;
    const float hint_height = hint.empty() ? 0 : menu.body->CalcTextSizeA(hint_size, FLT_MAX, text_width, hint.c_str()).y;
    const float height = std::max(px(68), 2 * pad + title_height + px(4) + hint_height);
    const std::string id = "##fun-preset-" + preset.name;
    if (ImGui::Selectable(id.c_str(), false, ImGuiSelectableFlags_None, ImVec2(width, height)))
        trainer_command(menu, callbacks, std::string(preset.active ? "preset remove " : "preset apply ") + preset.name);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    auto *draw = ImGui::GetWindowDrawList();
    if (hovered && available) draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), skate_theme::tile_light);
    if (ImGui::IsItemFocused()) draw->AddRect(at, ImVec2(at.x + width, at.y + height), skate_theme::white, 0, 0, px(2));
    draw->PushClipRect(at, ImVec2(at.x + width, at.y + height), true);
    draw->AddText(menu.bold, title_size, ImVec2(at.x + pad, at.y + pad),
                  ImGui::GetColorU32(skate_theme::white), label, nullptr, text_width);
    if (!hint.empty()) draw->AddText(menu.body, hint_size, ImVec2(at.x + pad, at.y + pad + title_height + px(4)),
                                    ImGui::GetColorU32(skate_theme::grey_text), hint.c_str(), nullptr, text_width);
    const ImVec2 box(at.x + width - pad - block, at.y + pad);
    const bool on = preset.active;
    draw->AddRectFilled(box, ImVec2(box.x + block, box.y + px(28)),
                        ImGui::GetColorU32(on ? skate_theme::blue : skate_theme::tile_grey));
    const char *state = on ? "ON" : "OFF";
    const auto size = menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, state);
    draw->AddText(menu.bold, px(14), ImVec2(box.x + (block - size.x) / 2, box.y + (px(28) - size.y) / 2),
                  ImGui::GetColorU32(on ? skate_theme::black : skate_theme::white), state);
    draw->AddLine(ImVec2(at.x + pad, at.y + height), ImVec2(at.x + width - pad, at.y + height),
                  ImGui::GetColorU32(IM_COL32(255, 255, 255, 24)));
    draw->PopClipRect();
    if (hovered || (ImGui::IsItemFocused() && GImGui->NavCursorVisible)) wrapped_tooltip("%s\n\n%s", hint.c_str(),
        available ? "Toggle this shortcut. Restoring it puts its affected settings back to stock; locked values stay." :
                    "Unavailable under the current session rules or while the command dispatcher is disconnected.");
    ImGui::EndDisabled();
}
void fun_toggles(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::View &view) {
    struct Group { const char *id, *title; };
    static constexpr Group groups[]{
        {"pop", "Pop and airtime"}, {"rotations", "Body rotations"}, {"board", "Speed and board feel"},
        {"grinds", "Grinds and landings"}, {"foot", "On foot and falling"}, {"experimental", "Experimental tweaks"},
        {"other", "Other shortcuts"},
    };
    std::array<std::vector<std::pair<const trainer::PresetRow *, const FunShortcut *>>, std::size(groups)> grouped;
    std::size_t active{};
    for (const auto &preset : view.presets) {
        if (!fun_preset(preset)) continue;
        const auto found = std::ranges::find(fun_shortcuts, preset.name, &FunShortcut::name);
        const auto *shortcut = found == std::end(fun_shortcuts) ? nullptr : found;
        grouped[shortcut ? shortcut->group : std::size(groups) - 1].push_back({&preset, shortcut});
        active += preset.active ? 1 : 0;
    }
    workshop_heading(menu, "Quick toggles", "Expand a category for one-click experiments. ON means the shortcut's values are set.");
    note(std::format("{} {} on. Shortcuts share settings; restoring one resets its affected values to stock. Locks stay.", active, active == 1 ? "shortcut" : "shortcuts").c_str());
    for (std::size_t group = 0; group < std::size(groups); ++group) {
        const auto &entries = grouped[group];
        if (entries.empty()) continue;
        const auto on = std::ranges::count_if(entries, [](const auto &entry) { return entry.first->active; });
        const std::string label = std::format("{} ({} on)###fun-{}", groups[group].title, on, groups[group].id);
        if (!ImGui::CollapsingHeader(label.c_str())) continue;
        const int columns = ImGui::GetContentRegionAvail().x >= px(840) ? 2 : 1;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(4), px(4)));
        if (ImGui::BeginTable(groups[group].id, columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
            for (const auto &[preset, shortcut] : entries) {
                ImGui::TableNextColumn();
                fun_switch(menu, callbacks, view, *preset, shortcut);
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar(); ImGui::Spacing();
    }
}
void workshop_fun(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    workshop_heading(menu, "Tricklining and experiments", "Build momentum through a line, then explore heights and quick toggles below.");
    if (ImGui::CollapsingHeader("Tricklining and reverts", ImGuiTreeNodeFlags_DefaultOpen)) trickline_section(menu, callbacks, p, view);
    if (ImGui::CollapsingHeader("Jump heights")) {
        ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command); trick_heights(menu, callbacks, p, view, {}); ImGui::EndDisabled();
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    fun_toggles(menu, callbacks, view);
    ImGui::Spacing();
    note("Never bail is in Skater > Movement. Flip speed, catch timing and pump power are in Feel.");
    (void)model;
}
void trick_heights(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const std::string &words) {
    (void)words;
    begin_card(menu, "trick-heights", "JUMP HEIGHTS");
    note("1x keeps the game's height. These multipliers can combine with the shortcuts below.");
    option_slider(menu, callbacks, p, view, "No comply height", "nocomply_height", view.nocomply_height, .1f, 50, "Height relative to stock. Ctrl-click to type a precise multiplier.");
    option_slider(menu, callbacks, p, view, "Boneless height", "boneless_height", view.boneless_height, .1f, 50, "Height relative to stock. Ctrl-click to type a precise multiplier.");
    option_slider(menu, callbacks, p, view, "Hippy jump height", "hippy_height", view.hippy_height, .1f, 50, "Height relative to stock. Ctrl-click to type a precise multiplier.");
    option_slider(menu, callbacks, p, view, "Off-board jump height", "offboard_height", view.offboard_height, .1f, 50, "Jump height on foot, relative to stock. Ctrl-click to type a precise multiplier.");

    ImGui::BeginDisabled(!view.editable || view.session_enforced || view.boosts_blocked || !callbacks.queue_console_command);
    if (ImGui::Button("Reset tricks...")) p.confirm_tricks = true;
    if (p.confirm_tricks) {
        warn("Restore all trick options, including flip speeds, catch timing, pump power and these boosts? Saved presets stay.");
        if (ImGui::Button("Confirm trick reset")) { trainer_command(menu, callbacks, "reset tricks"); p.confirm_tricks = false; p.active.clear(); }
        ImGui::SameLine(); if (ImGui::Button("Keep trick settings")) p.confirm_tricks = false;
    }
    ImGui::EndDisabled();
    note("Reset tricks also restores flip speed, catch timing and pump power in Feel. Saved presets stay.");
    end_card();
}


void practice_tab(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const auto telemetry = trainer::telemetry();
    begin_card(menu, "speed", "GAME SPEED", "Slow motion for learning a line");
    {
        const NamedSettingModel *setting = nullptr;
        for (const auto &row : model.engine_settings)
            if (row.name == "SimulationTime.TimeScale") {
                setting = &row;
                break;
            }
        if (!setting || !setting->available) {
            note(setting && !setting->reason.empty() ? setting->reason.c_str() : "Game speed is unavailable right now.");
            // The engine resolves a setting the first time it is asked for: ask.
            if (setting && callbacks.queue_console_command && ImGui::GetTime() >= p.speed_until) {
                p.speed_until = ImGui::GetTime() + 2.0;
                p.speed_edit = -1;
                std::array<char, 256> ignored{};
                callbacks.queue_console_command(callbacks.user, "reset SimulationTime.TimeScale", ignored.data(), ignored.size());
            }
        } else {
            double current = 1;
            try {
                const auto first = setting->value.find_first_of("-.0123456789");
                if (first != std::string::npos) current = std::stod(setting->value.substr(first));
            } catch (...) {}
            if (p.speed_edit >= 0 && ImGui::GetTime() < p.speed_until) current = p.speed_edit;
            const auto set = [&](double speed) {
                p.speed_edit = speed;
                p.speed_until = ImGui::GetTime() + 1.0;
                send_console(menu, callbacks, speed == 1.0 ? "reset SimulationTime.TimeScale" : "SimulationTime.TimeScale " + number(speed));
            };
            field(menu, "Speed");
            auto value = static_cast<float>(current);
            if (ImGui::SliderFloat("##game-speed", &value, 0.05f, 2.0f, current == 0.0 ? "paused" : "%.2fx", ImGuiSliderFlags_AlwaysClamp))
                set(value);
                if (!ImGui::IsItemActive()) tip("Slows the whole game down or speeds it up. 1x is normal speed.");
            if (ImGui::Button(current == 0.0 ? "Resume" : "Pause")) set(current == 0.0 ? 1.0 : 0.0);
            tip("Freezes the game where it is. Press again to carry on.");
            for (const auto quick : {0.1, 0.25, 0.5, 0.75, 1.0}) {
                ImGui::SameLine();
                if (ImGui::Button(std::format("{}x", number(quick)).c_str())) set(quick);
                tip("Sets the game speed to this straight away.");
            }
            note("The jump read-out measures real time, so it reads low while the game is slowed.");
        }
    }
    end_card();

    begin_card(menu, "markers", "MARKERS", "Saved for each map");
    int slot = view.slot;
    if (choice(menu, "marker-slot", slot, {"1", "2", "3", "4", "5"})) trainer_command(menu, callbacks, std::format("slot {}", slot + 1));
    const auto &marker = view.markers[static_cast<std::size_t>(std::clamp(view.slot, 0, static_cast<int>(trainer::marker_slots) - 1))];
    info(menu, "Selected", marker.set ? std::format("{:.1f}, {:.1f}, {:.1f}", marker.position[0], marker.position[1], marker.position[2]) : "empty");
    ImGui::BeginDisabled(view.map.empty());
    ImGui::BeginDisabled(!telemetry.skater);
    if (ImGui::Button("Save here")) trainer_command(menu, callbacks, "marker save");
    tip("Saves where you are standing in the selected slot, for this map.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!marker.set);
    if (ImGui::Button("Go")) trainer_command(menu, callbacks, "marker go");
    tip("Puts you back at the selected marker.");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) trainer_command(menu, callbacks, "marker clear");
    tip("Empties the selected slot.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    bool auto_return = view.auto_return;
    if (toggle_row(menu, "Return after a bail", "Go back to the selected marker when the skater wipes out.", auto_return))
        trainer_command(menu, callbacks, std::format("option auto_return {}", auto_return ? 1 : 0));
    field(menu, "Return delay");
    float delay = view.return_delay;
    ImGui::SliderFloat("##return-delay", &delay, 0.0f, 10.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
    if (!ImGui::IsItemActive()) tip("How long after a bail before you are put back at the marker.");
    if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("option return_delay {:.1f}", delay));
    bool pad = view.pad_shortcuts;
    if (toggle_row(menu, "Controller shortcuts", "Hold LB + RB, then D-pad: up saves, down goes, left / right pick the slot.", pad))
        trainer_command(menu, callbacks, std::format("option pad {}", pad ? 1 : 0));
    bool pad_menu = view.pad_menu;
    if (toggle_row(menu, "Open this menu from the controller",
            "LB + RB + click the right stick opens and closes the menu. Inside: D-pad or left stick moves, A presses, B goes back, LB / RB change a slider in big steps.", pad_menu))
        trainer_command(menu, callbacks, std::format("option pad_menu {}", pad_menu ? 1 : 0));
    end_card();

    begin_card(menu, "teleport", "TELEPORT");
    if (telemetry.skater)
        info(menu, "You are at", std::format("{:.2f}, {:.2f}, {:.2f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]));
    field(menu, "X, Y, Z");
    ImGui::InputFloat3("##teleport", p.teleport.data(), "%.2f");
    tip("A position in the game's own coordinates: X, height, Z.");
    ImGui::BeginDisabled(!telemetry.skater);
    if (ImGui::Button("Here")) p.teleport = telemetry.position;
    tip("Fills the boxes with where you are now.");
    ImGui::SameLine();
    if (ImGui::Button("Go##teleport"))
        trainer_command(menu, callbacks, std::format("tp {:.3f} {:.3f} {:.3f}", p.teleport[0], p.teleport[1], p.teleport[2]));
        tip("Moves you to the position in the boxes. You arrive on foot.");
    ImGui::SameLine();
    if (ImGui::Button("Copy position"))
        ImGui::SetClipboardText(std::format("{:.3f}, {:.3f}, {:.3f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]).c_str());
        tip("Copies where you are to the clipboard as X, Y, Z.");
    ImGui::SameLine();
    if (ImGui::Button("Copy for Blender"))
        ImGui::SetClipboardText(std::format("{:.3f}, {:.3f}, {:.3f}", telemetry.position[0], -telemetry.position[2], telemetry.position[1]).c_str());
        tip("Copies where you are in Blender's axes (Z up), for map makers.");
    ImGui::EndDisabled();
    end_card();
}

void jump_lines(SkateMenu &menu, const trainer::Jump &jump) {
    info(menu, "Takeoff", std::format("{:.1f} km/h at {:.1f} degrees", jump.takeoff_speed * 3.6f, jump.takeoff_angle));
    info(menu, "Air time", std::format("{:.2f} s", jump.air_time));
    info(menu, "Height", std::format("{:.2f} m", jump.height));
    info(menu, "Distance", std::format("{:.2f} m, {:.2f} m drop", jump.distance, jump.drop));
    info(menu, "Landing", std::format("{:.1f} km/h at {:.1f}, {:.1f}, {:.1f}", jump.landing_speed * 3.6f, jump.landing[0], jump.landing[1],
                                      jump.landing[2]));
    info(menu, "Rotation", std::format("spin {:.0f} degrees (peak {:.0f} per second), flip {:.0f} degrees", jump.spin, jump.spin_rate, jump.flip));
}
// Which game the skating plays like: the game's own, or Skate 3 on one of its three difficulties.
// One is always lit; a Skate 3 one goes out again as soon as one of its values is changed by hand.
void feel_buttons(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::View &view) {
    struct Choice {
        const char *label, *preset, *command, *tip;
    };
    static constexpr Choice choices[]{
        {"skate.", "", "feel stock", "The game's own tuning."},
        {"Skate 3 Easy", "Skate 3 Easy", "feel easy", "Skate 3 on Easy: full-height pops every time, strong pushes, generous grind lock-on, easy spins."},
        {"Skate 3", "Skate 3", "feel normal", "Skate 3 on Normal: its pop, grind pops, pushing, pumping, steering, manuals, spins, flips and bails."},
        {"Skate 3 Hardcore", "Skate 3 Hardcore", "feel hardcore", "Skate 3 on Hardcore: lower pops, weaker pushes, tight grind lock-on, slow auto spins."},
    };
    const auto active = [&](const char *preset) {
        return std::ranges::any_of(view.presets, [&](const trainer::PresetRow &row) { return row.builtin && row.active && row.name == preset; });
    };
    const bool any = active("Skate 3 Easy") || active("Skate 3") || active("Skate 3 Hardcore");
    const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4;
    for (std::size_t i = 0; i < std::size(choices); ++i) {
        if (i) ImGui::SameLine();
        const bool on = i == 0 ? !any : active(choices[i].preset);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
        if (ImGui::Button(choices[i].label, ImVec2(width, 0))) trainer_command(menu, callbacks, choices[i].command);
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) wrapped_tooltip("%s", choices[i].tip);
    }
}

// Everything that makes a trick line, in one place: the same values and sliders as the Tune tab,
// gathered, with one button for Skate 3's and one for the game's own.
void trickline_section(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const bool can_edit = view.editable && !view.session_enforced && callbacks.queue_console_command != nullptr;
    ImGui::BeginDisabled(!can_edit);

    // Rows of the value table by id, looked up once per snapshot.
    static constexpr const char *groups[][2]{
        {"REVERTS AND POWERSLIDES", "onboard_powerslide.frictionscalar_revert onboard_powerslide.frictionscalar_autorevert "
                                    "onboard_powerslide.powerslide_forwardforcescalar_revert onboard_powerslide.powerslide_forwardforcescalar_autorevert "
                                    "onboard_powerslide.frictionscalar_slide onboard_powerslide.powerslide_forwardforcescalar_slide"},
        {"PUMPING", "physicsmode.pumpeffectfactor physicsmode.pumpmaxacceleration physicsmode.pumpmaxdeceleration physicsmode.unintentionalpumpscalar "
                    "physicspumping.pumpangle physicspumping.pumpminfactor physicspumping.pumpoutoftransitionscalar"},
        {"POPS", "physicsmode.jumpmaxheight physicsmode.jumpminheight physicsmode.jumpminheightmanual physicsmode.grindjumpcommonmax physicsmode.grindjumpcommonmin "
                 "physicsmode.grindjumpboardslidemax physicsmode.grindjumpboardslidemin physicsmode.grindjumptipslidemax physicsmode.grindjumptipslidemin "
                 "physicsonboardjumptuning.jumpadjustvelocityboostlimitground physicsonboardjumptuning.jumpadjustvelocityboostlimitgrind"},
        {"SPINS AND FLIPS", "physicsairstates.maxspinspeed physicsmode.maxautobodyspinspeed physicsmode.easybodyspins physicsreckoning.flipscalar "
                            "physicsreckoning.flipmaxspeed heldflip.straightflipcatchtime heldflip.shuvmincatchtime heldflip.varialmincatchtime "
                            "heldflip.bigflipmincatchtime"},
        {"MANUALS AND GRINDS", "physicsmanual.manualscalar_p physicsmanual.manualscalar_i physicsmanual.manualscalar_d physicsmanual.maxtiltangle "
                               "physicsmode.grindlockdist physicsgrind.commonfrictionscalar physicsgrindsair.maxdistboardslide physicsgrindsair.maxdisttipslide "
                               "physicsgrind.grind_tipslide_minangletoprimitive physicsgrind.slowgrindexitspeed physicsgrind.copingexitstarttime "
                               "physicsgrind.curbexitstarttime physicsgrind.fairlysteepexitstarttime physicsfriction.noinputtime "
                               "physicssteering.generalscalar physicssteering.damping"},
    };
    // What each group is for in a line, said once at the top of its card.
    static constexpr const char *group_notes[]{
        "",
        "Pumping is speed without pushing: crouch into a transition and stand up out of it. Strength is how much a pump gives, the limit how "
        "fast it may build. Skate 3 pumped harder (18 against 13) but capped it lower.",
        "A line lives on pops you can count on. Max is a held pop, min a quick flick; bring them together and every pop is the same height. "
        "Each kind of grind has its own pair, and the stick boost is the speed you can add by pushing the stick as you pop.",
        "How fast you and the board turn, and how long a flip may take to come round (its catch time). Slower spins and longer catch times "
        "read as style; faster ones fit more into a small gap.",
        "Manual balance (the three balance numbers are how hard the game corrects you), how far a rail reaches out to grab you, how quickly a "
        "grind loses speed, and when the game ends a grind for you. The tuning skate. grew out of ended slow grinds by itself; in skate. those exit timers are off (-1).",
    };
    if (p.trick_revision != view.revision) {
        p.trick_revision = view.revision;
        for (std::size_t g = 0; g < std::size(groups); ++g) {
            p.trick_rows[g].clear();
            const std::string keys = groups[g][1];
            for (std::size_t start = 0; start < keys.size();) {
                auto end = keys.find(' ', start);
                if (end == std::string::npos) end = keys.size();
                const auto key = keys.substr(start, end - start);
                for (std::size_t i = 0; i < view.rows.size() && !key.empty(); ++i)
                    if (lower(view.rows[i].id) == key) {
                        p.trick_rows[g].push_back(i);
                        break;
                    }
                start = end + 1;
            }
        }
    }
    const auto rows = [&](std::size_t g) {
        for (const auto i : p.trick_rows[g])
            if (i < view.rows.size()) value_row(menu, callbacks, p, view, view.rows[i], false, true);
    };

    begin_card(menu, "trick-revert", "MOMENTUM AND REVERTS");
    note("Board bending can add speed when the board lands out of line with your body. 0x keeps it off; 1x uses the boost amounts below. Tune how reverts and powerslides carry momentum below.");
    option_slider(menu, callbacks, p, view, "Board bending boost", "revert_boost", view.revert_boost, 0, 5,
        "0 is off. Adds speed when a twisted landing snaps straight. Ordinary 180s do not count by default. From AutoRevertBoost by Sivaes, jaq and OVM.",
        "%.2fx");
    ImGui::Spacing();
    note("Tricklining extras apply at least 1x bending, 3x revert friction and 1.5x powerslide friction. Restore turns bending off and restores those friction values to stock. Locks stay.");
    ImGui::BeginDisabled(!can_edit || view.boosts_blocked);
    const float actions_width = ImGui::GetContentRegionAvail().x;
    const bool paired = actions_width >= px(580);
    const float action_width = paired ? (actions_width - ImGui::GetStyle().ItemSpacing.x) / 2 : actions_width;
    if (ImGui::Button("Apply tricklining extras", ImVec2(action_width, px(36))))
        trainer_command(menu, callbacks, "trickline extras on");
    tip("Applies the extras bundle, respecting locks. This is separate from your custom bending amount.");
    if (paired) ImGui::SameLine();
    if (ImGui::Button("Restore stock extras", ImVec2(action_width, px(36))))
        trainer_command(menu, callbacks, "trickline extras off");
    tip("Turns board bending off and restores revert, auto-revert and powerslide friction. It does not undo to previous custom values; locks stay.");
    ImGui::EndDisabled();
    if (ImGui::TreeNodeEx("What counts as a bend, and what it is worth", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        const auto &r = view.revert;
        const auto rule = [&](const char *label, const char *command, float value, float low, float high, const char *format, const char *help) {
            command_slider(menu, callbacks, p, view, label, command, command, value, low, high, help, format);
        };
        rule("Spin needed", "revert spin", r.min_spin, 0, 180, "%.0f deg", "Minimum spin in the air before a landing can earn a bending boost.");
        rule("Board off the travel (90 deg disables this rule)", "revert slip", r.min_slip, 0, 90, "%.0f deg", "Minimum angle to the travel direction. 90 disables this travel-angle rule.");
        rule("Board off your body", "revert twist", r.min_twist, 0, 90, "%.0f deg", "Minimum board-to-body twist at landing that can count as a bend.");
        rule("Small bend boost", "revert bend", r.bend_boost, 0, 10, "+%.1f m/s", "Speed added for a small bend, multiplied by Board bending boost.");
        rule("Full bend boost", "revert full", r.full_boost, 0, 10, "+%.1f m/s", "Speed added for a full bend, multiplied by Board bending boost.");
        rule("Auto revert boost", "revert auto", r.auto_boost, 0, 10, "+%.1f m/s", "Speed added for an automatic revert, multiplied by Board bending boost.");
        rule("No boost above", "revert max_speed", r.max_speed, 5, 100, "%.1f m/s", "No extra speed above this limit. 1 m/s is 3.6 km/h.");
        rule("Time between boosts", "revert cooldown", r.cooldown, 0, 3, "%.2f s", "Minimum wait between bending boosts.");
        rule("Shortest flight", "revert min_air", r.min_air, 0, 2, "%.2f s", "Shorter hops cannot earn a bending boost.");
        ImGui::Spacing();
        ImGui::BeginDisabled(view.boosts_blocked);
        if (ImGui::Button("Reset these rules")) trainer_command(menu, callbacks, "revert reset");
        tip("Restores the nine rules above. Board bending strength and saved presets stay.");
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Friction and forward force")) {
        note("Friction removes speed; forward force adds a push during a revert or slide. Blue values are changed. Ctrl-click to type precisely.");
        rows(0);
    }
    end_card();
    static constexpr const char *tuning_sections[]{"", "Transition tuning", "Pop tuning", "Rotation tuning", "Manuals and grind tuning"};
    for (std::size_t g = 1; g < std::size(groups); ++g) {
        if (p.trick_rows[g].empty() || !ImGui::CollapsingHeader(tuning_sections[g])) continue;
        begin_card(menu, groups[g][0]);
        note(group_notes[g]);
        rows(g);
        end_card();
    }
    ImGui::EndDisabled();
}

void map_tab(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::View &view) {
    const auto telemetry = trainer::telemetry();
    begin_card(menu, "hud", "HUD");
    bool hud = view.hud, jump = view.hud_jump, logging = view.logging;
    if (toggle_row(menu, "Speed and air HUD", "Speed, air time and height in the corner while you skate.", hud))
        trainer_command(menu, callbacks, std::format("option hud {}", hud ? 1 : 0));
    if (toggle_row(menu, "Jump read-out", "After each landing: takeoff speed and angle, height, distance, landing speed.", jump))
        trainer_command(menu, callbacks, std::format("option hud_jump {}", jump ? 1 : 0));
    if (toggle_row(menu, "Record telemetry", "Write position and speed every frame to a CSV in %LOCALAPPDATA%\\ReSkate\\trainer\\telemetry.",
            logging))
        trainer_command(menu, callbacks, std::format("option log {}", logging ? 1 : 0));
    end_card();

    begin_card(menu, "now", "RIGHT NOW");
    if (!telemetry.skater) {
        note("No skater yet.");
    } else {
        info(menu, "Speed", std::format("{:.1f} km/h ({:.2f} m/s), top {:.1f} km/h", telemetry.speed * 3.6f, telemetry.speed, telemetry.top_speed * 3.6f));
        info(menu, "Position", std::format("{:.2f}, {:.2f}, {:.2f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]));
        info(menu, "Heading", std::format("{:.0f} degrees", telemetry.heading));
    }
    end_card();

    if (telemetry.last.serial) {
        begin_card(menu, "last-jump", "LAST JUMP", std::format("Jump {}", telemetry.last.serial).c_str());
        jump_lines(menu, telemetry.last);
        end_card();
    }
    if (telemetry.best.serial && telemetry.best.serial != telemetry.last.serial) {
        begin_card(menu, "best-jump", "LONGEST JUMP ON THIS MAP");
        jump_lines(menu, telemetry.best);
        end_card();
    }

    begin_card(menu, "map", "THIS MAP", view.map.empty() ? "No level loaded" : view.map.c_str());
    if (!view.map_note.empty()) note(view.map_note.c_str());
    if (!view.map_preset.empty()) {
        info(menu, "Author's preset", view.map_preset);
        ImGui::BeginDisabled(!view.editable);
        if (ImGui::Button("Apply the author's preset")) trainer_command(menu, callbacks, "preset apply map");
        tip("Sets the values this map's maker recommends for it.");
        ImGui::EndDisabled();
    }
    for (std::size_t i = 0; i < view.spots.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button("Go")) trainer_command(menu, callbacks, std::format("spot {}", i + 1));
        tip("Takes you to this spot.");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(view.spots[i].name.c_str());
        ImGui::PopID();
    }
    if (view.map_note.empty() && view.map_preset.empty() && view.spots.empty())
        note("Map makers can ship a trainer.json in their mod folder with spots and a recommended preset.");
    end_card();
}
} // namespace

bool trainer_take_open() {
    const auto view = trainer::view();
    auto &p = page();
    if (view->open_serial == p.open_serial) return false;
    p.open_serial = view->open_serial;
    switch (view->open_tab) {
    case 1: p.tab = 2; break;
    case 2: p.tab = 4; p.tools_tab = 0; break;
    case 3: p.tab = 4; p.tools_tab = 1; break;
    case 5: case 8: p.tab = 3; break;
    case 6: case 10: p.tab = 1; break;
    case 7: p.tab = 4; p.tools_tab = 0; break; // legacy camera route
    default: p.tab = 0; break;
    }
    p.show_page = true;
    return true;
}
bool trainer_page_wanted() {
    auto &p = page();
    const bool wanted = p.show_page;
    p.show_page = false;
    return wanted;
}
void trainer_page(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks) {
    auto &p = page();
    const auto view = trainer::view();
    const int restrictions = (view->session_enforced ? 1 : 0) | (view->boosts_blocked ? 2 : 0) | (!view->editable ? 4 : 0);
    if (p.map != view->map || p.restrictions != restrictions) {
        p.map = view->map; p.restrictions = restrictions; p.active.clear(); p.confirm_reset = p.confirm_tricks = false;
        p.confirm_delete.clear(); p.confirm_save.clear(); ImGui::ClearActiveID();
    }
    note(view->ready ? (view->stock ? "Stock setup. No trainer changes are active." : std::format("Custom setup: {} physics values changed. Save it in Presets.", view->touched).c_str()) : view->status.c_str());
    // `preset export` leaves its line here; the clipboard belongs to this thread.
    if (!p.share_seen) {
        p.share_seen = true;
        p.share_serial = view->share_serial;
    } else if (p.share_serial != view->share_serial) {
        p.share_serial = view->share_serial;
        ImGui::SetClipboardText(view->share_text.c_str());
    }
    if (view->session_enforced) {
        warn("The host controls physics in this session.");
        note("Your setup is kept for when you leave. HUD and practice markers remain available.");
    } else if (view->boosts_blocked) {
        warn("The host has disabled boosts.");
        note("Physics values still work. Board flip, catch, pumping and boost options are unavailable until the host allows them or you leave.");
    }
    const auto old_tab = p.tab;
    workshop_navigation(menu, p.tab, {"FEEL", "SETTINGS", "PRESETS", "FUN", "TOOLS"}, "trainer-tabs");
    if (old_tab != p.tab) { p.active.clear(); p.confirm_tricks = false; p.confirm_reset = false; p.confirm_save.clear(); p.confirm_delete.clear(); ImGui::ClearActiveID(); }
    ImGui::PushID(p.tab);
    ImGui::BeginChild("trainer-tab", ImVec2(0, page_body_height(menu)));
    if (!view->ready && p.tab < 4) note(view->status.c_str());
    else {
        if (!view->editable && p.tab < 4) warn(view->blocked.c_str());
        switch (p.tab) {
        case 0: workshop_feel(menu, callbacks, p, *view); break;
        case 1: workshop_settings(menu, callbacks, p, *view); break;
        case 2: workshop_presets(menu, callbacks, p, *view); break;
        case 3: workshop_fun(menu, model, callbacks, p, *view); break;
        default:
            workshop_navigation(menu, p.tools_tab, {"PRACTICE", "MAP & HUD"}, "trainer-tools");
            if (p.tools_tab == 0) practice_tab(menu, model, callbacks, p, *view);
            else map_tab(menu, callbacks, *view);
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}
} // namespace dingosdk::overlay::menu

namespace dingosdk::overlay {
bool trainer_open_requested() { return menu::trainer_take_open(); }
bool trainer_pad_menu_pressed(bool menu_visible) {
    static ControllerComboLatch latch;
    ControllerInput input;
    DingoSDKOverlayReadControllerInput(&input, true);
    constexpr std::uint32_t combo = 0x100 | 0x200 | 0x80; // LB + RB + R3
    if (!latch.update(combo, input, !trainer::view()->pad_menu)) return false;
    if (!menu_visible) menu::page().show_page = true;
    return true;
}
bool trainer_hud_pending() {
    const auto view = trainer::view();
    return (view->hud || view->hud_jump) && trainer::telemetry().skater;
}
void draw_trainer_hud() {
    const auto view = trainer::view();
    if (!view->hud && !view->hud_jump) return;
    const auto telemetry = trainer::telemetry();
    if (!telemetry.skater) return;
    static std::uint64_t shown_jump{};
    static double jump_until{};
    if (telemetry.last.serial != shown_jump) {
        shown_jump = telemetry.last.serial;
        jump_until = shown_jump ? ImGui::GetTime() + 7.0 : 0.0;
    }
    std::vector<std::string> lines;
    if (view->hud) {
        lines.push_back(std::format("{:.0f} km/h", telemetry.speed * 3.6f));
        if (telemetry.airborne) lines.push_back(std::format("air {:.2f} s   {:.2f} m", telemetry.air_time, telemetry.height));
    }
    if (view->hud_jump && telemetry.last.serial && ImGui::GetTime() < jump_until) {
        const auto &jump = telemetry.last;
        lines.push_back(std::format("jump {}:  {:.1f} km/h at {:.0f} deg", jump.serial, jump.takeoff_speed * 3.6f, jump.takeoff_angle));
        lines.push_back(std::format("{:.2f} s   {:.2f} m high   {:.2f} m far", jump.air_time, jump.height, jump.distance));
        lines.push_back(std::format("landed at {:.1f} km/h, {:.2f} m lower", jump.landing_speed * 3.6f, jump.drop));
        if (jump.spin >= 45.0f || jump.flip >= 90.0f) lines.push_back(std::format("spin {:.0f} deg   flip {:.0f} deg", jump.spin, jump.flip));
    }
    if (lines.empty()) return;
    auto *draw = ImGui::GetForegroundDrawList();
    const auto &display = ImGui::GetIO().DisplaySize;
    const float scale = std::clamp(display.y / 1080.0f, 0.75f, 2.5f);
    auto *font = ImGui::GetFont();
    const float big = 34.0f * scale, small = 18.0f * scale, pad = 10.0f * scale;
    float width = 0, height = pad * 2;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float size = i == 0 && view->hud ? big : small;
        width = std::max(width, font->CalcTextSizeA(size, FLT_MAX, 0, lines[i].c_str()).x);
        height += size + 4.0f * scale;
    }
    const ImVec2 origin(display.x - width - pad * 2 - 24.0f * scale, 24.0f * scale);
    draw->AddRectFilled(origin, ImVec2(origin.x + width + pad * 2, origin.y + height), IM_COL32(12, 14, 18, 170), 6.0f * scale);
    float y = origin.y + pad;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float size = i == 0 && view->hud ? big : small;
        draw->AddText(font, size, ImVec2(origin.x + pad, y), IM_COL32(245, 245, 240, 255), lines[i].c_str());
        y += size + 4.0f * scale;
    }
}
} // namespace dingosdk::overlay
