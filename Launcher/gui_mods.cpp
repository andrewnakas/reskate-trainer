#include "gui_internal.h"

#include "mod_manager.h"

#include "Engine/Core/Log/logging.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <format>

using Microsoft::WRL::ComPtr;

// The Mods page: a nav rail on the left, and a list where every mod is a
// full-width row that opens where it sits. MY MODS is install order and
// enabled state; GET MODS is Thunderstore (gui_mods_browse.cpp).
namespace dingosdk::launcher_gui::detail {
namespace {

void save(ModsPanel& panel) {
    try {
        mods::save_mod_order(panel.root, panel.list.entries);
        panel.list.issue.clear();
        panel.message = "Saved. Changes apply the next time Skate starts.";
        panel.message_error = false;
    } catch (const std::exception& failure) {
        panel.message = failure.what();
        panel.message_error = true;
    }
}

// Picks up a finished install: rescan and select the new mod, or ask to replace.
void collect_install(ModsPanel& panel, const launcher_app::Session& session) {
    std::lock_guard lock(panel.mutex);
    if (!panel.finished) return;
    panel.finished = false;
    panel.progress = -1;
    if (!panel.finished_conflict.empty()) {
        panel.conflict_name = panel.finished_conflict;
        panel.conflict_source = panel.finished_source;
        panel.message.clear();
        return;
    }
    scan(panel, session);
    if (!panel.finished_error.empty()) {
        panel.message = panel.finished_error;
        panel.message_error = true;
        return;
    }
    for (std::size_t i = 0; i < panel.list.entries.size(); ++i)
        if (panel.list.entries[i].mod.name == panel.finished_name) panel.selected = static_cast<int>(i);
    panel.message_error = false;
    if (!panel.finished_note.empty()) {
        panel.message = panel.finished_note;   // Thunderstore installs say what they did and log it themselves
        panel.finished_note.clear();
        return;
    }
    panel.message = "Installed " + panel.finished_name + ". It loads the next time Skate starts.";
    logging::write(logging::Level::info, logging::Channel::launcher, "Mod installed: " + panel.finished_name);
}

// The Windows file (or folder) picker; empty when cancelled.
fs::path pick(HWND owner, bool folder) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        const COMDLG_FILTERSPEC filter[]{{L"Mod archive (*.zip)", L"*.zip"}};
        dialog->SetFileTypes(1, filter);
    }
    dialog->SetTitle(folder ? L"Choose a mod folder to install" : L"Choose a mod .zip to install");
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path{};
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::string summary(const mods::Mod& mod) {
    std::vector<std::string> parts;
    if (!mod.version.empty()) parts.push_back("v" + mod.version);
    if (!mod.author.empty()) parts.push_back("by " + mod.author);
    if (!mod.levels.empty()) parts.push_back(mod.levels.size() == 1 ? "1 map" : std::to_string(mod.levels.size()) + " maps");
    else if (mod.provides_layout) parts.push_back("game data");
    if (!mod.park_maps.empty()) parts.push_back(mod.park_maps.size() == 1 ? "1 park" : std::to_string(mod.park_maps.size()) + " parks");
    if (!mod.provides_layout && !mod.provides_levels && mod.park_maps.empty()) parts.push_back("nothing to load");
    std::string text;
    for (const auto& part : parts) text += (text.empty() ? "" : "  /  ") + part;
    return text;
}

// ---------------------------------------------------------------- nav rail

// The rail: the two pages, then what you can do to the Mods folder. Returns
// true when BACK was pressed.
bool rail(const Fonts& fonts, ModsPanel& panel, HWND window, float width,
          const std::vector<const thunderstore::Package*>& pending, bool installing) {
    const auto installed = panel.list.entries.size();
    const auto& store = panel.store;
    bool leave = false;

    ImGui::BeginDisabled(installing);
    if (nav_tile(fonts, width, "MY MODS", panel.tab == 0,
            pending.empty() ? std::to_string(installed) : std::to_string(pending.size()), !pending.empty(),
            pending.empty() ? std::string()
                            : pending.size() == 1 ? std::string("1 update on Thunderstore")
                                                  : std::format("{} updates on Thunderstore", pending.size())))
        panel.tab = 0;
    if (nav_tile(fonts, width, "GET MODS", panel.tab == 1,
            store.loaded ? std::to_string(store.packages.size()) : std::string("..."), false,
            store.loaded ? std::string() : std::string("Loading the Thunderstore listing")))
        panel.tab = 1;

    ImGui::Dummy(ImVec2(0, S(8)));
    if (!pending.empty()) {
        push_primary_button();
        if (ImGui::Button(std::format("UPDATE ALL ({})", pending.size()).c_str(), ImVec2(-1, S(34)))) {
            std::vector<thunderstore::Package> packages;
            for (const auto* package : pending) packages.push_back(*package);
            start_store_install(panel, std::move(packages));
        }
        pop_primary_button();
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    if (ImGui::Button("Install .zip", ImVec2(-1, S(32))))
        if (const auto path = pick(window, false); !path.empty()) start_install(panel, path, false);
    if (ImGui::Button("Install folder", ImVec2(-1, S(32))))
        if (const auto path = pick(window, true); !path.empty()) start_install(panel, path, false);
    if (ImGui::Button("Open Mods folder", ImVec2(-1, S(32)))) open_path(panel.root);
    if (ImGui::Button("Open Thunderstore", ImVec2(-1, S(32))))
        open_url(utf8(thunderstore::community_page(thunderstore::community())));
    ImGui::EndDisabled();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(38));
    push_primary_button();
    if (ImGui::Button("\xe2\x86\x90  BACK", ImVec2(-1, S(34)))) leave = true;
    pop_primary_button();
    return leave;
}

// ---------------------------------------------------------------- MY MODS

// One installed mod: enabled state, its place in the load order, and a click
// anywhere else for everything the mod says about itself.
void installed_row(const Fonts& fonts, ModsPanel& panel, const thunderstore::Installed& installed, int index,
                   float tall, bool& changed, int& move_from, int& move_to) {
    auto& entry = panel.list.entries[static_cast<std::size_t>(index)];
    const auto& mod = entry.mod;
    ImGui::PushID(index);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    if (list_row("##row", width, tall, false)) {
        panel.selected = index;
        panel.overview = false;
    }
    const float right = start.x + width;
    const bool left_out = panel.list.excluded.contains(mod.name);
    const auto* package = package_for(panel.store, mod.name);
    const bool update = package && thunderstore::update_available(*package, installed);

    // The same icon the store shows, so a mod looks like itself on both pages.
    mod_icon(panel, package, ImVec2(start.x + S(48), start.y + (tall - S(52)) * 0.5f), S(52));
    const float text_x = start.x + S(112);
    const auto title = std::to_string(index + 1) + ".  " + mod.title;
    draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(text_x, start.y + S(16)),
        entry.enabled ? color::text : color::muted, title.c_str());
    if (const auto detail = summary(mod); !detail.empty())
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, start.y + S(40)), color::muted, detail.c_str());

    // The widgets that sit on the row, over its Selectable.
    ImGui::SetCursorScreenPos(ImVec2(start.x + S(14), start.y + (tall - ImGui::GetFrameHeight()) * 0.5f));
    if (ImGui::Checkbox("##enabled", &entry.enabled)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(entry.enabled ? "Enabled: loads when Skate starts" : "Disabled");

    // Right to left: the load order, then what you can do to the mod.
    const float arrow = ImGui::GetFrameHeight();
    const float arrows_x = right - S(16) - arrow * 2 - S(6);
    ImGui::SetCursorScreenPos(ImVec2(arrows_x, start.y + (tall - arrow) * 0.5f));
    ImGui::BeginDisabled(index == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { move_from = index; move_to = index - 1; }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load earlier: later mods win where they overlap");
    ImGui::SameLine(0, S(6));
    ImGui::BeginDisabled(index + 1 == static_cast<int>(panel.list.entries.size()));
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) { move_from = index; move_to = index + 1; }
    ImGui::EndDisabled();

    const float button = S(104), button_y = start.y + (tall - S(30)) * 0.5f;
    float next = arrows_x - S(14) - button;
    ImGui::SetCursorScreenPos(ImVec2(next, button_y));
    if (ImGui::Button("Uninstall", ImVec2(button, S(30)))) panel.confirm_remove = mod.name;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete this mod's folder (it goes to the Recycle Bin)");
    if (update) {
        next -= S(8) + button;
        ImGui::SetCursorScreenPos(ImVec2(next, button_y));
        push_primary_button();
        if (ImGui::Button("UPDATE", ImVec2(button, S(30)))) start_store_install(panel, {*package});
        pop_primary_button();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Thunderstore has v%s", package->latest().number.c_str());
    }
    float pill_x = next - S(14);
    const float pill_y = start.y + (tall - fonts.caption->FontSize - S(8)) * 0.5f;
    const auto pill = [&](const char* label, ImU32 fill) {
        pill_x -= badge_width(fonts, label);
        badge(draw, fonts, ImVec2(pill_x, pill_y), label, fill, color::ink);
        pill_x -= S(8);
    };
    if (!mod.outdated.empty()) pill("OUTDATED", color::warning);
    else if (left_out) pill("NOT LOADED", color::danger);
    ImGui::PopID();
}

void installed_page(Launcher& launcher, const Fonts& fonts, ModsPanel& panel, const thunderstore::Installed& installed,
                    float height, bool installing) {
    auto& entries = panel.list.entries;
    const float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - S(110));
    if (ImGui::Button("Refresh", ImVec2(S(110), 0))) refresh_mods(launcher, panel);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Mods load top to bottom: where two change the same thing, the higher one wins. Changes apply "
                        "the next time Skate starts. Click a mod to see everything about it.");
    if (launcher.game())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::warning),
            "Skate is running: restart it to apply changes.");
    if (!panel.list.issue.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            "mods.json could not be read (%s), so the game loads no mods. Any change here rewrites it.",
            panel.list.issue.c_str());
    std::vector<std::string> dropped;
    for (const auto& entry : entries)
        if (entry.enabled && panel.list.excluded.contains(entry.mod.name) && entry.mod.outdated.empty())
            dropped.push_back(entry.mod.title);
    if (!dropped.empty()) {
        std::string names;
        for (const auto& title : dropped) names += (names.empty() ? "" : ", ") + title;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            dropped.size() == 1 ? "%s did not load: the game could not merge it cleanly, so none of its content is "
                                  "used. Open it to see what went wrong."
                                : "%s did not load: the game could not merge them cleanly, so none of their content "
                                  "is used. Open one to see what went wrong.", names.c_str());
    }
    if (!panel.list.missing.empty()) {
        std::string missing;
        for (const auto& name : panel.list.missing) missing += (missing.empty() ? "" : ", ") + name;
        ImGui::TextDisabled("mods.json also lists folders that are not installed: %s", missing.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const float body = std::max(S(120), height - (ImGui::GetCursorPosY() - top));
    ImGui::BeginChild("##mod_list", ImVec2(0, body), ImGuiChildFlags_Borders);
    if (entries.empty()) {
        ImGui::Spacing();
        ImGui::Indent(S(14));
        ImGui::TextDisabled(panel.list.present ? "No mods installed yet." : "No Mods folder yet.");
        ImGui::TextDisabled("Use GET MODS to browse Thunderstore, or drop a mod .zip on the window.");
        ImGui::Unindent(S(14));
    }
    bool changed = false;
    int move_from = -1, move_to = -1;
    const float row = S(76);
    ImGui::BeginDisabled(installing);
    virtual_rows(static_cast<int>(entries.size()), [&](int) { return row; }, [&](int index, float tall) {
        installed_row(fonts, panel, installed, index, tall, changed, move_from, move_to);
    });
    ImGui::EndDisabled();
    ImGui::EndChild();

    if (move_from >= 0) {
        std::swap(entries[static_cast<std::size_t>(move_from)], entries[static_cast<std::size_t>(move_to)]);
        if (panel.selected == move_from) panel.selected = move_to;
        else if (panel.selected == move_to) panel.selected = move_from;
        changed = true;
    }
    if (changed) save(panel);
}

// Everything an installed mod says about itself, in a popup over the page.
void mod_overview(const Fonts& fonts, ModsPanel& panel, const thunderstore::Installed& installed, ImVec2 size,
                  bool installing) {
    if (panel.selected < 0 || panel.selected >= static_cast<int>(panel.list.entries.size())) {
        panel.selected = -1;
        return;
    }
    auto& entry = panel.list.entries[static_cast<std::size_t>(panel.selected)];
    const auto& mod = entry.mod;
    const auto* package = package_for(panel.store, mod.name);
    const bool update = package && thunderstore::update_available(*package, installed);

    if (!panel.overview) {
        ImGui::OpenPopup("##mod_overview");
        panel.overview = true;
    }
    const ImVec2 extent(std::min(S(680), size.x - S(80)), std::min(S(560), size.y - S(80)));
    ImGui::SetNextWindowPos(ImVec2((size.x - extent.x) * 0.5f, (size.y - extent.y) * 0.5f));
    ImGui::SetNextWindowSize(extent);
    if (!ImGui::BeginPopupModal("##mod_overview", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        panel.overview = false;        // dismissed with Escape
        panel.selected = -1;
        return;
    }
    const auto close = [&] {
        panel.overview = false;
        panel.selected = -1;
        ImGui::CloseCurrentPopup();
    };
    ImGui::PushFont(fonts.heading);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(mod.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::TextDisabled("%s", summary(mod).c_str());
    ImGui::Spacing();
    if (update) {
        ImGui::BeginDisabled(installing);
        push_primary_button();
        if (ImGui::Button(("UPDATE TO v" + package->latest().number).c_str(), ImVec2(0, S(34)))) {
            start_store_install(panel, {*package});
            close();
        }
        pop_primary_button();
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::Button("Open folder", ImVec2(0, S(34)))) open_path(mod.directory);
    if (package && !package->package_url.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Thunderstore page", ImVec2(0, S(34)))) open_url(package->package_url);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(installing);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::danger));
    // The confirmation is its own modal, so this one has to go first.
    if (ImGui::Button("Uninstall", ImVec2(0, S(34)))) {
        const auto name = mod.name;
        close();
        panel.confirm_remove = name;
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::Spacing();

    ImGui::BeginChild("##mod_overview_body",
        ImVec2(0, std::max(S(80), extent.y - ImGui::GetCursorPosY() - S(24) - ImGui::GetFrameHeight())));
    ImGui::PushTextWrapPos(0);
    if (!mod.outdated.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            "Outdated: this mod does not load. It was made for another version of Skate (%s). Get an updated "
            "version, or rebuild it with the latest ReSkate Studio.", mod.outdated.c_str());
    if (const auto missing = panel.list.excluded.find(mod.name); missing != panel.list.excluded.end()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            "Not loaded: the game could not merge this mod cleanly, so none of it is used. Reinstall the whole "
            "mod folder, or rebuild it with a current ReSkate Studio.");
        if (!missing->second.empty()) ImGui::TextDisabled("%s", missing->second.front().c_str());
    }
    field(fonts, "AUTHOR", mod.author);
    field(fonts, "VERSION", mod.version);
    field(fonts, "DESCRIPTION", mod.description);
    field(fonts, "FOLDER", "Mods\\" + mod.name);
    if (package)
        field(fonts, "THUNDERSTORE", package->full_name + (update ? "  (v" + package->latest().number + " available)"
                                                                  : std::string("  (up to date)")));
    if (!mod.tool.empty() || !mod.built.empty())
        field(fonts, "BUILT WITH", mod.tool + (mod.built.empty() ? "" : (mod.tool.empty() ? "" : ", ") + mod.built));
    std::string levels;
    for (const auto& level : mod.levels) {
        const auto slash = level.rfind('/');
        levels += (levels.empty() ? "" : "\n") + (slash == std::string::npos ? level : level.substr(slash + 1));
    }
    field(fonts, "MAPS", levels);
    std::string parks;
    for (const auto& map : mod.park_maps) parks += (parks.empty() ? "" : ", ") + map;
    field(fonts, "PARKS", parks);
    if (!mod.provides_layout && !mod.provides_levels && mod.park_maps.empty())
        field(fonts, "NOTE", "This folder has no layout.toc or reskate-levels.json, so the game has nothing to load from it.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    ImGui::SetCursorPosY(extent.y - S(24) - ImGui::GetFrameHeight());
    bool enabled = entry.enabled;
    if (ImGui::Checkbox("Loads when Skate starts", &enabled)) {
        entry.enabled = enabled;
        save(panel);
    }
    ImGui::SameLine(extent.x - S(28) - S(110));
    if (ImGui::Button("CLOSE", ImVec2(S(110), 0))) close();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- installing

// What a mod manager shows while it works: everything else dimmed, and one
// panel saying what is being downloaded.
void install_modal(const Fonts& fonts, ModsPanel& panel, ImVec2 size) {
    std::string activity;
    {
        std::lock_guard lock(panel.mutex);
        activity = panel.activity;
    }
    const float fraction = std::clamp(panel.progress.load(), 0.0f, 1.0f);
    const auto time = static_cast<float>(ImGui::GetTime());
    // Cancelling is not instant: the worker notices on its next chunk or file.
    // Say so, or the button looks dead and gets pressed again.
    const bool cancelling = panel.cancel.load();
    // Its own window over the page: the page's own draw list renders under
    // its children, so a panel drawn there would sit beneath the mod list.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##installing", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(0, 0), size, rgba(4, 6, 9, 0.72f));
    const ImVec2 extent(S(540), S(206));
    const ImVec2 origin((size.x - extent.x) * 0.5f, (size.y - extent.y) * 0.5f);
    const ImVec2 end(origin.x + extent.x, origin.y + extent.y);
    skate_theme::rough_rect(draw, origin, end, color::blue, 11, g_scale);
    draw->AddText(fonts.tile, fonts.tile->FontSize, ImVec2(origin.x + S(26), origin.y + S(18)), color::ink,
        cancelling ? "CANCELLING" : "INSTALLING");
    const auto line = cancelling ? std::string("Stopping as soon as the current file is done. "
                                               "The Mods folder is left as it was.")
                                 : activity;
    if (!line.empty())
        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(origin.x + S(26), origin.y + S(70)),
            rgba(0, 0, 0, 0.78f), line.c_str(), nullptr, extent.x - S(52));
    skate_theme::striped_bar(draw, ImVec2(origin.x + S(26), origin.y + S(118)),
        ImVec2(end.x - S(26), origin.y + S(134)), fraction, time, g_scale);
    draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(origin.x + S(26), origin.y + S(142)), rgba(0, 0, 0, 0.78f),
        std::format("{:.0f}%", fraction * 100).c_str());
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(rgba(0, 0, 0, 0.38f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(rgba(0, 0, 0, 0.52f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(rgba(0, 0, 0, 0.66f)));
    ImGui::SetCursorScreenPos(ImVec2(end.x - S(26) - S(130), end.y - S(24) - S(34)));
    ImGui::BeginDisabled(cancelling);
    const bool pressed = ImGui::Button(cancelling ? "CANCELLING" : "CANCEL", ImVec2(S(130), S(34)));
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    // Escape too: the button is the only way out of this panel otherwise.
    if (!cancelling && (pressed || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        panel.cancel = true;
        logging::write(logging::Level::info, logging::Channel::launcher,
            pressed ? "Mod install cancelled by the Cancel button."
                    : "Mod install cancelled with Escape.");
    }
    ImGui::End();
}

} // namespace

void refresh_mods(Launcher& launcher, ModsPanel& panel) {
    scan(panel, launcher.session());
    refresh_listing(panel, ImGui::GetTime(), true);
    panel.message.clear();
}

void scan(ModsPanel& panel, const launcher_app::Session& session) {
    panel.root = launcher_mods::mods_root(session.paths.directory);
    panel.list = mods::scan_mods(panel.root.parent_path());
    panel.scanned = true;
    if (panel.selected >= static_cast<int>(panel.list.entries.size())) panel.selected = -1;
}

void start_install(ModsPanel& panel, const fs::path& source, bool replace) {
    if (panel.installing) return;
    if (panel.worker.joinable()) panel.worker.join();
    panel.installing = true;
    panel.cancel = false;
    panel.progress = 0;
    panel.message.clear();
    panel.message_error = false;
    {
        std::lock_guard lock(panel.mutex);
        panel.activity = "Installing " + utf8(source.filename().wstring());
    }
    const auto root = panel.root;
    panel.worker = std::thread([&panel, root, source, replace] {
        std::string name, error, conflict;
        try {
            name = launcher_mods::install(root, source, replace,
                [&panel](float fraction) { panel.progress = fraction; }, panel.cancel);
        } catch (const launcher_mods::AlreadyInstalled& existing) {
            conflict = existing.name;
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        std::lock_guard lock(panel.mutex);
        panel.finished = true;
        panel.finished_name = name;
        panel.finished_error = error;
        panel.finished_conflict = conflict;
        panel.finished_source = source;
        panel.activity.clear();
        panel.installing = false;
    });
}

// What PLAY shows instead of launching when the merge left mods out. The game
// would leave them out too, silently, three minutes into a loading screen.
void mods_broken_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, ModsPanel& panel,
                        const std::vector<ModProblem>& problems) {
    const bool one = problems.size() == 1;
    const float rows = static_cast<float>(problems.size()) * S(52);
    const auto frame = begin_panel("##mods_broken_panel", size,
        ImVec2(S(620), std::min(size.y - S(80), S(300) + rows)));
    panel_title(fonts, one ? "A MOD COULD NOT BE MERGED" : "MODS COULD NOT BE MERGED");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(one
        ? "This mod cannot be combined with the game's files, so none of its content would load. Skate would "
          "start without it and never say why."
        : "These mods cannot be combined with the game's files, so none of their content would load. Skate would "
          "start without them and never say why.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const float footer = ImGui::GetFrameHeight() + ImGui::GetTextLineHeight() + S(56);
    ImGui::BeginChild("##broken_list", ImVec2(0, frame.y - ImGui::GetCursorPosY() - footer),
        ImGuiChildFlags_Borders);
    for (const auto& problem : problems) {
        ImGui::PushFont(fonts.bold);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::danger));
        ImGui::TextUnformatted(problem.title.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", problem.reason.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::PushFont(fonts.caption);
    ImGui::TextDisabled("Reinstall the whole mod folder, or rebuild it with a current ReSkate Studio.");
    ImGui::PopFont();

    ImGui::SetCursorPosY(frame.y - S(24) - ImGui::GetFrameHeight());
    // A rejected mods.json is not a mod, so there is nothing to switch off.
    const bool switchable = std::any_of(problems.begin(), problems.end(),
        [](const ModProblem& problem) { return !problem.name.empty(); });
    bool disable = false;
    if (switchable) {
        push_primary_button();
        disable = ImGui::Button(one ? "SWITCH IT OFF AND PLAY" : "SWITCH THEM OFF AND PLAY", ImVec2(S(260), 0));
        pop_primary_button();
        ImGui::SameLine(0, S(8));
    }
    if (disable) {
        // mods.json is the game's own switch, so the next launch skips them
        // without the merge finding out the hard way again.
        const auto root = launcher_mods::mods_root(launcher.session().paths.directory);
        auto list = mods::scan_mods(root.parent_path());
        for (auto& entry : list.entries)
            for (const auto& problem : problems)
                if (entry.mod.name == problem.name) entry.enabled = false;
        try {
            mods::save_mod_order(root, list.entries);
            logging::write(logging::Level::info, logging::Channel::launcher,
                "Mods switched off after a failed merge; launching without them");
        } catch (const std::exception& failure) {
            logging::write(logging::Level::warning, logging::Channel::launcher,
                std::string("Could not switch the mods off: ") + failure.what());
        }
        panel.scanned = false;
        launcher.play_anyway();
    }
    if (ImGui::Button("Open Mod Manager", ImVec2(S(160), 0))) {
        launcher.dismiss_mod_problems();
        ui.mods = true;
        panel.tab = 0;
        panel.scanned = false;
    }
    ImGui::SameLine(frame.x - S(28) - S(110));
    if (ImGui::Button("Play anyway", ImVec2(S(110), 0))) launcher.play_anyway();
    ImGui::End();
}

void mods_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, ModsPanel& panel, HWND window) {
    const auto& session = launcher.session();
    if (!panel.scanned) scan(panel, session);
    collect_install(panel, session);
    refresh_listing(panel, ImGui::GetTime());
    // A page, not a panel: the mod list and the Thunderstore browser both want
    // the whole window. It sits at (0, 0), so window and screen space agree.
    const auto frame = begin_page("##mods_panel", size);
    // Both lists draw mod icons, so both need finished ones uploaded.
    pump_icons(panel);
    auto* draw = ImGui::GetWindowDrawList();
    const bool installing = panel.installing;
    auto& entries = panel.list.entries;
    const auto installed = installed_versions(panel.list);
    const auto pending = updates(panel.store, installed);
    const auto close = [&] { ui.mods = false; panel.message.clear(); panel.scanned = false; };

    // The page covers the main screen's buttons, so it draws its own.
    window_buttons(draw, window, frame);
    const float rail_x = S(28), rail_width = S(236);
    const float content_x = rail_x + rail_width + S(26);
    const float top = S(52), bottom = frame.y - S(24);
    const float title_size = S(44);
    page_title(draw, fonts, ImVec2(rail_x + S(2), top - S(4)), "MOD\nMANAGER", title_size);

    const float rail_top = top + title_size * 2 + S(16);
    ImGui::SetCursorPos(ImVec2(rail_x, rail_top));
    ImGui::BeginChild("##rail", ImVec2(rail_width, bottom - rail_top));
    const bool leave = rail(fonts, panel, window, rail_width, pending, installing);
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(content_x, top));
    ImGui::BeginChild("##content", ImVec2(frame.x - content_x - S(28), bottom - top));
    // With nothing to say, the list runs all the way down to BACK's bottom
    // edge; a message takes two lines off it until it is gone.
    const float status = panel.message.empty() ? S(4) : ImGui::GetTextLineHeight() * 2 + S(12);
    const float body = ImGui::GetWindowHeight() - status;
    if (panel.tab == 1) browse_page(launcher, fonts, panel, body, installing);
    else installed_page(launcher, fonts, panel, installed, body, installing);
    if (!panel.message.empty()) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - status + S(8));
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(panel.message_error ? color::danger : color::good), "%s",
            panel.message.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    if (panel.tab == 1) package_overview(fonts, panel, frame, installing);
    else mod_overview(fonts, panel, installed, frame, installing);
    if (leave) close();
    if (!installing && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close();

    // ------------------------------------------------ confirmations
    if (!panel.confirm_remove.empty() && !ImGui::IsPopupOpen("Uninstall mod")) ImGui::OpenPopup("Uninstall mod");
    if (!panel.conflict_name.empty() && !ImGui::IsPopupOpen("Replace mod")) ImGui::OpenPopup("Replace mod");
    ImGui::SetNextWindowSize(ImVec2(S(440), 0));
    if (ImGui::BeginPopupModal("Uninstall mod", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushTextWrapPos(0);
        ImGui::Text("Uninstall \"%s\"? Its folder goes to the Recycle Bin.", panel.confirm_remove.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(S(110), 0))) { panel.confirm_remove.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        push_primary_button();
        if (ImGui::Button("UNINSTALL", ImVec2(S(110), 0))) {
            const auto name = panel.confirm_remove;
            try {
                launcher_mods::remove(panel.root, name);
                std::erase_if(entries, [&](const auto& entry) { return entry.mod.name == name; });
                save(panel);
                panel.message = "Uninstalled " + name + ". It is in the Recycle Bin if you want it back.";
                panel.selected = -1;
                logging::write(logging::Level::info, logging::Channel::launcher, "Mod removed: " + name);
            } catch (const std::exception& failure) {
                panel.message = failure.what();
                panel.message_error = true;
            }
            const auto message = panel.message;
            const bool error = panel.message_error;
            scan(panel, session);
            panel.message = message;
            panel.message_error = error;
            panel.confirm_remove.clear();
            ImGui::CloseCurrentPopup();
        }
        pop_primary_button();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(S(440), 0));
    if (ImGui::BeginPopupModal("Replace mod", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushTextWrapPos(0);
        ImGui::Text("\"%s\" is already installed. Replace it? The installed copy goes to the Recycle Bin.",
            panel.conflict_name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(S(110), 0))) {
            panel.conflict_name.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        push_primary_button();
        if (ImGui::Button("REPLACE", ImVec2(S(110), 0))) {
            const auto source = panel.conflict_source;
            panel.conflict_name.clear();
            ImGui::CloseCurrentPopup();
            start_install(panel, source, true);
        }
        pop_primary_button();
        ImGui::EndPopup();
    }
    g_drag_allowed = !ImGui::IsAnyItemHovered() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGui::End();
    if (installing) install_modal(fonts, panel, frame);
}

} // namespace dingosdk::launcher_gui::detail
