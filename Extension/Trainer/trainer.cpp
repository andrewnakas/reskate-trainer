#include "trainer.h"
#include "trainer_classes.h"
#include "trainer_gamestate.h"
#include "trainer_jump.h"
#include "trainer_presets.h"
#include "trainer_session.h"
#include "trainer_feel.h"
#include "trainer_atomic_file.h"
#include "trainer_preset_io.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/physics_tuning.h"
#include "Engine/Game/Multiplayer/session_physics.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/physics_tuning.h"
#include "Extension/Skater/skater_observer.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>

// Game thread only. See trainer.h for how the menu reaches this.
namespace dingosdk::trainer {
namespace {
namespace tuning = dingosdk::physics_tuning;
using Vec3 = std::array<float, 3>;
constexpr float max_coordinate = 1.0e6f;
constexpr std::uint32_t air_states_first = 200, air_states_end = 300;
constexpr std::uint32_t xinput_up = 0x1, xinput_down = 0x2, xinput_left = 0x4, xinput_right = 0x8,
                        xinput_lb = 0x100, xinput_rb = 0x200;

struct Entry {
    std::string id, key, label, group; // key: lower-case id
    Kind kind{};
    std::uint16_t offset{}, size{};
    std::size_t curve{};
    double value{}, stock{};
    bool touched{}, frozen{};
    bool detail{};
    bool used{}; // the game's code was found to read it (trainer_used.inc), or it is linked
    std::vector<std::size_t> drives; // graph multipliers this value scales (value_links)
    std::vector<std::size_t> members; // graph: its outputs (Y0.., Min_Y, Max_Y)
    int class_field{-1};              // a field of one of the game's data-defined classes (trainer_classes.h), not of the tuning asset
};
struct MapData {
    std::array<Marker, marker_slots> markers{};
    std::string preset;
};
struct MapFile { // Mods/<mod>/trainer.json
    std::string note, preset_name;
    std::vector<std::pair<std::string, double>> preset;
    std::vector<Spot> spots;
};
struct Saved {
    double value{};
    bool frozen{};
};
struct SelfTest {
    int step{}; // 0: idle
    std::uint64_t until{};
    std::size_t entry{};
    double before{}, target{};
    Vec3 origin{};
    int failures{};
};
struct Motion {
    bool valid{};
    Vec3 position{}, velocity{};
    std::int64_t stamp{};
    bool airborne{};
    Vec3 takeoff{}, takeoff_velocity{};
    std::int64_t takeoff_stamp{};
    float apex{};
    bool axes_valid{};
    Vec3 forward{}, up{};
    float spin{}, spin_rate{}, flip{};
    float board_turn{}; // the board's fastest turn rate in this jump, degrees per second
    std::uint32_t air_state{}, ground_state{};
};
// The trick multipliers and auto push in force: x of the game's own, and for auto push the
// speed a rolling skater is carried up to (m/s, 0: off).
struct Boosts {
    float flip{1}, hippy{1}, nocomply{1}, boneless{1}, offboard{1}, cruise{};
};
// How long a new skater alone waits for a memory search: doubled while searches find nothing new.
constexpr std::uint64_t class_skater_wait_low = 20000, class_skater_wait_high = 600000;
struct State {
    bool loaded{}; // the store was read
    std::shared_ptr<const tuning::Model> model;
    std::vector<Entry> entries;
    std::map<std::string, std::size_t, std::less<>> index;
    std::uintptr_t base{}, asset{}, entity{};
    tuning::Values own, want;
    bool write_due{}, view_due{true}, save_due{};
    bool profile_due{}; // the loaded map's preset is still to be applied
    bool refresh_due{}; // values were written while the skater had no cached copy to refresh
    std::uint64_t next_hold{}, next_save{}, next_skater{}, revision{};
    std::string status = "Waiting for the game.", last;
    bool ready{};
    std::map<std::string, Saved, std::less<>> saved;                                    // by key
    std::map<std::string, std::map<std::string, double, std::less<>>, std::less<>> user; // presets
    std::vector<std::string> active;                                                    // presets applied since Stock
    std::map<std::string, MapData, std::less<>> maps;
    std::string map;
    MapFile map_file;
    int slot{};
    bool auto_return{}, pad_shortcuts{}, hud{}, hud_jump{}, logging{};
    bool pad_menu{true}; // LB + RB + R3 opens and closes the menu
    // The hippy jump's height is set by the game's trick scripts, not by tuning: the trainer
    // scales the upward velocity when it sees one start.
    float hippy_height{1};
    // An on-foot jump: the same scaling, when a skater standing off the board starts to rise.
    float offboard_height{1};
    // Board flip tricks: a multiplier on the game's flip speed curves (trainer_classes.h).
    float flip_speed{1};
    // "Advanced trick speed": each flip trick also turns at a speed of its own (flip_tricks, trainer_gamestate.h).
    bool flip_advanced{};
    std::array<float, flip_tricks.size()> flip_trick = [] {
        std::array<float, flip_tricks.size()> ones{};
        ones.fill(1.0f);
        return ones;
    }();
    // The flip speed in the air now: the game's own number, what the trainer made of it, and whether it can be reached at all.
    struct FlipDrive {
        bool active{};
        physics_policy::FlipObservation observed{};
        float game{}, written{};
        float factor{1}; // the flip speed times the turning trick's own
    } flip_drive;
    bool flip_live{};
    std::uint32_t flip_trick_seen{}; // the last trick that turned (CurrentFlipTrick)
    bool catch_at{};           // flips are caught at catch_percent of the jump's air time
    float catch_percent{70};
    float catch_seconds{flip_gate_stock}; // what that comes to for the jump in the air now, as seconds before landing
    bool offboard_grounded{}; // off the board and not moving up or down at the last tick
    const char *boost_name{""};
    // The no comply and the boneless are launched by the trick scripts too (trainer_jump.cpp).
    float nocomply_height{1}, boneless_height{1};
    float revert_boost{};            // 0: off
    float pump_power{1};             // x what pumping a transition gains
    bool pump_live{};                // the game's pumping states are found
    RevertTuning revert;
    std::uint64_t revert_boosts_seen{};
    std::uint32_t last_state{};
    float boost_factor{}; // velocity factor of the jump now starting, 0: none
    std::uint64_t boost_until{};
    float return_delay{1.5f};
    std::uint64_t seen_wipeouts{}, return_at{}, open_serial{};
    std::uint64_t class_search_at{}; // when to look for the game's tuning classes (0: not due)
    int class_search_tries{};
    std::uint64_t class_search_seen{};      // the searches that had finished when this level loaded
    bool class_list_wanted{};               // the player has the list of every value open on this level
    float flip_applied{1};                  // the flip speed the catch times were last set for
    std::uint64_t class_entity{};           // the skater the last search was for
    std::uint64_t class_research_after{};   // no new search before this, however many skaters come and go
    std::uint64_t class_creations{};        // the skaters the game had built at the last look
    bool class_research_due{};              // a new skater came while a search could not start: look when it can
    bool class_skater_due{};                // the same, for a new skater alone (nothing looked lost)
    bool class_skater_judged{true};         // the search a new skater asked for has been compared with what was known
    std::uint64_t class_skater_after{};     // no search for a new skater alone before this
    std::uint64_t class_skater_wait{class_skater_wait_low};
    std::uint64_t class_found{};            // class_search_found() before that search
    std::vector<std::size_t> class_entries; // the entries that are fields of those classes
    // A session whose host sets everyone's physics (session_physics.h): what the host shares
    // beyond its tuning, and what each class field holds for it. The player's own stand down.
    bool enforced{};
    std::uint64_t host_revision{};
    SessionExtras host;
    std::array<float, class_field_count> host_classes{};
    Boosts boosts;         // what is in force this tick
    std::string share_text; // the last exported preset, for the menu to put on the clipboard
    std::uint64_t share_serial{};
    bool extras_due{true}; // the player's own extras are to be published for a session again
    bool wipeouts_known{}; // the wipeout count the session started with was read
    int open_tab{};
    std::uint32_t pad_previous{};
    Motion motion;
    Telemetry telemetry;
    std::ofstream log;
    std::int64_t log_start{};
    std::map<std::uint32_t, std::uint64_t> states; // physics state -> ticks seen
    SelfTest test;
};
State &state() {
    static auto *value = new State;
    return *value;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
// "MaxPushSpeed" -> "Max Push Speed"
std::string spaced(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool upper = std::isupper(c) != 0;
        if (i && upper) {
            const auto before = static_cast<unsigned char>(text[i - 1]);
            const bool next_lower = i + 1 < text.size() && std::islower(static_cast<unsigned char>(text[i + 1]));
            if (std::islower(before) || std::isdigit(before) || (std::isupper(before) && next_lower)) result += ' ';
        }
        result += text[i] == '.' ? ' ' : text[i];
        if (text[i] == '.') result += "/ ";
    }
    return result;
}
std::optional<double> number(std::string_view text) {
    double value{};
    const auto *last = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) return std::nullopt;
    return value;
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::skater, text); }

// ---- storage ---------------------------------------------------------------------------
std::filesystem::path data_directory() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"trainer";
    if (raw) CoTaskMemFree(raw);
    return result;
}
std::filesystem::path game_directory() {
    std::vector<wchar_t> exe(32768);
    const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) return {};
    return std::filesystem::path(exe.data()).parent_path();
}
std::optional<Json> read_json(const std::filesystem::path &path) {
    try {
        const auto text = storage::read_bounded(path, 32 * 1024 * 1024);
        if (!text || text->empty()) return std::nullopt;
        return Json::parse(*text, {.bytes = 32 * 1024 * 1024, .events = 4 * 1024 * 1024});
    } catch (...) {
        return std::nullopt;
    }
}
std::optional<Vec3> read_position(const Json &value) {
    if (!value.is_array() || value.size() != 3) return std::nullopt;
    Vec3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!value.at(i).is_number()) return std::nullopt;
        result[i] = value.at(i).get<float>();
        if (!std::isfinite(result[i]) || std::abs(result[i]) > max_coordinate) return std::nullopt;
    }
    return result;
}
Json write_position(const Vec3 &p) { return Json::array({Json(p[0]), Json(p[1]), Json(p[2])}); }

struct RevertField {
    std::string_view name;
    float RevertTuning::*member;
    float low, high;
};
constexpr RevertField revert_fields[]{
    {"spin", &RevertTuning::min_spin, 0.0f, 100000.0f},     {"slip", &RevertTuning::min_slip, 0.0f, 90.0f},
    {"twist", &RevertTuning::min_twist, 0.0f, 90.0f},       {"bend", &RevertTuning::bend_boost, -1000.0f, 1000.0f},
    {"full", &RevertTuning::full_boost, -1000.0f, 1000.0f}, {"auto", &RevertTuning::auto_boost, -1000.0f, 1000.0f},
    {"max_speed", &RevertTuning::max_speed, 0.0f, 1000.0f}, {"cooldown", &RevertTuning::cooldown, 0.0f, 60.0f},
    {"min_air", &RevertTuning::min_air, 0.0f, 60.0f},
};
RevertTuning sane_revert(RevertTuning tuning) {
    const RevertTuning stock;
    for (const auto &field : revert_fields) {
        auto &value = tuning.*field.member;
        value = std::isfinite(value) ? std::clamp(value, field.low, field.high) : stock.*field.member;
    }
    return tuning;
}
void load_store() {
    auto &s = state();
    s.loaded = true;
    const auto json = read_json(data_directory() / L"trainer.json");
    if (!json || !json->is_object()) return;
    try {
        if (json->contains("options") && json->at("options").is_object()) {
            const auto &o = json->at("options");
            s.auto_return = o.value("auto_return", false);
            s.pad_shortcuts = o.value("pad_shortcuts", false);
            s.pad_menu = o.value("pad_menu", true);
            s.hud = o.value("hud", false);
            s.hud_jump = o.value("hud_jump", false);
            s.return_delay = std::clamp(o.value("return_delay", 1.5f), 0.0f, 10.0f);
            s.hippy_height = std::clamp(o.value("hippy_height", 1.0f), height_low, height_high);
            s.nocomply_height = std::clamp(o.value("nocomply_height", 1.0f), height_low, height_high);
            s.offboard_height = std::clamp(o.value("offboard_height", 1.0f), height_low, height_high);
            s.flip_speed = std::clamp(o.value("flip_speed", 1.0f), flip_low, flip_high);
            s.flip_advanced = o.value("flip_advanced", false);
            for (std::size_t i = 0; i < flip_tricks.size(); ++i)
                s.flip_trick[i] = std::clamp(o.value(std::string(flip_tricks[i].key), 1.0f), flip_low, flip_high);
            s.catch_at = o.value("catch_at", false);
            s.catch_percent = std::clamp(o.value("catch_percent", 70.0f), catch_low, catch_high);
            s.boneless_height = std::clamp(o.value("boneless_height", 1.0f), height_low, height_high);
            s.revert_boost = std::clamp(o.value("revert_boost", 0.0f), 0.0f, revert_boost_high);
            s.pump_power = std::clamp(o.value("pump_power", 1.0f), 0.0f, pump_high);
            s.slot = std::clamp(o.value("slot", 0), 0, static_cast<int>(marker_slots) - 1);
        }
        if (json->contains("values") && json->at("values").is_object())
            for (const auto &[key, row] : json->at("values").items())
                if (row.is_object() && row.contains("value") && row.at("value").is_number())
                    s.saved[lower(key)] = {row.at("value").get<double>(), row.value("frozen", false)};
        if (json->contains("presets") && json->at("presets").is_object())
            for (const auto &[name, values] : json->at("presets").items()) {
                if (!values.is_object() || name.empty() || name.size() > 48) continue;
                auto &preset = s.user[name];
                for (const auto &[key, value] : values.items())
                    if (value.is_number()) preset[lower(key)] = value.get<double>();
            }
        if (json->contains("active") && json->at("active").is_array())
            for (const auto &name : json->at("active"))
                // (Fast Parkour Flips and Pump Power were presets of earlier versions that changed nothing.)
                if (name.is_string() && name.string() != "Fast Parkour Flips" && name.string() != "Pump Power") s.active.push_back(name.string());
        if (json->contains("revert") && json->at("revert").is_object()) {
            for (const auto &field : revert_fields)
                if (const auto name = std::string(field.name); json->at("revert").contains(name) && json->at("revert").at(name).is_number())
                    s.revert.*field.member = json->at("revert").at(name).get<float>();
            // 0.4.1 saved its own default for the off-the-travel rule (12 degrees), which paid ordinary
            // 180s: a file from before the rules were numbered that still holds it gets today's (off).
            if (!json->at("revert").contains("rules") && s.revert.min_slip == 12.0f) s.revert.min_slip = RevertTuning{}.min_slip;
            s.revert = sane_revert(s.revert);
        }
        if (json->contains("maps") && json->at("maps").is_object())
            for (const auto &[level, row] : json->at("maps").items()) {
                if (!row.is_object()) continue;
                auto &map = s.maps[lower(level)];
                map.preset = row.value("preset", "");
                if (row.contains("markers") && row.at("markers").is_array())
                    for (std::size_t i = 0; i < marker_slots && i < row.at("markers").size(); ++i)
                        if (const auto position = read_position(row.at("markers").at(i))) map.markers[i] = {true, *position};
            }
    } catch (...) {
        say(logging::Level::warning, "Trainer: trainer.json is damaged; starting from what could be read.");
    }
}
void save_store() {
    auto &s = state();
    s.save_due = false;
    try {
        Json json = Json::object();
        json["schema"] = 1;
        Json options = Json::object();
        options["auto_return"] = s.auto_return;
        options["pad_shortcuts"] = s.pad_shortcuts;
        options["pad_menu"] = s.pad_menu;
        options["hud"] = s.hud;
        options["hud_jump"] = s.hud_jump;
        options["return_delay"] = s.return_delay;
        options["hippy_height"] = s.hippy_height;
        options["nocomply_height"] = s.nocomply_height;
        options["offboard_height"] = s.offboard_height;
        options["flip_speed"] = s.flip_speed;
        options["flip_advanced"] = s.flip_advanced;
        for (std::size_t i = 0; i < flip_tricks.size(); ++i)
            if (s.flip_trick[i] != 1.0f) options[std::string(flip_tricks[i].key)] = s.flip_trick[i];
        options["catch_at"] = s.catch_at;
        options["catch_percent"] = s.catch_percent;
        options["boneless_height"] = s.boneless_height;
        options["revert_boost"] = s.revert_boost;
        options["pump_power"] = s.pump_power;
        options["slot"] = s.slot;
        json["options"] = std::move(options);
        Json values = Json::object();
        // Values of fields this build's tuning does not have are kept as they were read.
        for (const auto &[key, saved] : s.saved) {
            Json row = Json::object();
            row["value"] = saved.value;
            row["frozen"] = saved.frozen;
            values[key] = std::move(row);
        }
        json["values"] = std::move(values);
        Json presets = Json::object();
        for (const auto &[name, preset] : s.user) {
            Json row = Json::object();
            for (const auto &[key, value] : preset) row[key] = value;
            presets[name] = std::move(row);
        }
        json["presets"] = std::move(presets);
        Json active = Json::array();
        for (const auto &name : s.active) active.push_back(name);
        json["active"] = std::move(active);
        Json maps = Json::object();
        for (const auto &[level, map] : s.maps) {
            if (level.empty()) continue;
            Json row = Json::object();
            Json markers = Json::array();
            for (const auto &marker : map.markers) markers.push_back(marker.set ? write_position(marker.position) : Json(nullptr));
            row["markers"] = std::move(markers);
            row["preset"] = map.preset;
            maps[level] = std::move(row);
        }
        json["maps"] = std::move(maps);
        Json revert = Json::object();
        for (const auto &field : revert_fields) revert[std::string(field.name)] = s.revert.*field.member;
        revert["rules"] = 2;
        json["revert"] = std::move(revert);
        const auto directory = data_directory();
        if (directory.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) { say(logging::Level::warning, "Trainer: could not create the profile folder."); return; }
        if (const auto saved = storage::write_atomic(directory / L"trainer.json", json.dump(2)); saved)
            say(logging::Level::warning, "Trainer: save failed; previous trainer.json was preserved: " + saved.message());
    } catch (...) {
        say(logging::Level::warning, "Trainer: could not save trainer.json.");
    }
}
void changed(bool save = true) {
    auto &s = state();
    s.view_due = true;
    s.extras_due = true;
    if (save) {
        s.save_due = true;
        s.next_save = GetTickCount64() + 1500;
    }
}

// ---- values ----------------------------------------------------------------------------
double read_field(const std::vector<std::uint8_t> &image, const Entry &e) {
    const auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        float value;
        std::memcpy(&value, at, 4);
        return value;
    }
    if (e.kind == Kind::flag) return *at ? 1 : 0;
    switch (e.size) {
    case 1: { std::int8_t v; std::memcpy(&v, at, 1); return v; }
    case 2: { std::int16_t v; std::memcpy(&v, at, 2); return v; }
    case 4: { std::int32_t v; std::memcpy(&v, at, 4); return v; }
    default: { std::int64_t v; std::memcpy(&v, at, 8); return static_cast<double>(v); }
    }
}
void write_field(std::vector<std::uint8_t> &image, const Entry &e, double value) {
    auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        const auto v = static_cast<float>(value);
        std::memcpy(at, &v, 4);
    } else if (e.kind == Kind::flag) {
        *at = value != 0 ? 1 : 0;
    } else {
        switch (e.size) {
        case 1: { const auto v = static_cast<std::int8_t>(std::clamp(value, -128.0, 127.0)); std::memcpy(at, &v, 1); break; }
        case 2: { const auto v = static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0)); std::memcpy(at, &v, 2); break; }
        case 4: { const auto v = static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0)); std::memcpy(at, &v, 4); break; }
        default: { const auto v = static_cast<std::int64_t>(value); std::memcpy(at, &v, 8); break; }
        }
    }
}
// A curve's outputs times `factor`: a point is 0x1c bytes with its output at +0x14.
tuning::Curve scaled_curve(const tuning::Curve &source, double factor) {
    auto result = source;
    for (std::size_t p = 0; p + addr::physics_tuning::curve_point_size <= result.points.size(); p += addr::physics_tuning::curve_point_size) {
        float y;
        std::memcpy(&y, result.points.data() + p + 0x14, 4);
        y = static_cast<float>(y * factor);
        if (std::isfinite(y)) std::memcpy(result.points.data() + p + 0x14, &y, 4);
    }
    return result;
}
double sane(const Entry &e, double value) {
    if (!std::isfinite(value)) return e.stock;
    if (e.kind == Kind::flag) return value != 0 ? 1 : 0;
    if (e.kind == Kind::integer) return std::round(value);
    // No limits of the trainer's own beyond keeping a number a number: what a value may sensibly
    // be is the player's call (a slider's ends are only where the slider stops).
    if (e.kind == Kind::curve || e.kind == Kind::graph) return std::clamp(value, -1.0e9, 1.0e9);
    return std::clamp(value, -1.0e12, 1.0e12);
}
// The held flip class's catch times: how long each kind of flip may take to come round.
bool slowed_catch_time(std::string_view key) {
    return key.starts_with("heldflip.") && key.ends_with("catchtime");
}
// What a class field should hold: the player's value, or the host's while a session's host
// sets everyone's physics.
void want_class(const Entry &e) {
    auto &s = state();
    const auto field = static_cast<std::size_t>(e.class_field);
    auto value = s.enforced ? s.host_classes[field] : static_cast<float>(e.touched ? e.value : e.stock);
    // The game hurries any flip that would not come round inside its catch time (measured: at
    // every flip speed below about x0.9 a kickflip turned at 360 degrees per 0.2 s, the stock
    // catch time). A flip slowed by the trick slider is given that much longer.
    if (s.boosts.flip < 1.0f && slowed_catch_time(e.key)) value /= std::max(s.boosts.flip, flip_low);
    want_class_value(field, value);
}
void apply_entry(Entry &e) {
    auto &s = state();
    if (e.class_field >= 0) {
        want_class(e);
        return;
    }
    if (e.kind == Kind::graph) {
        // A point the player set by hand keeps their value.
        for (const auto index : e.members) {
            const auto &member = s.entries[index];
            if (!member.touched && static_cast<std::size_t>(member.offset) + member.size <= s.want.image.size())
                write_field(s.want.image, member, e.touched ? member.stock * e.value : member.stock);
        }
    } else if (e.kind == Kind::curve) {
        if (e.curve < s.own.curves.size() && s.own.curves[e.curve] && e.curve < s.want.curves.size())
            s.want.curves[e.curve] = e.touched ? scaled_curve(*s.own.curves[e.curve], e.value) : *s.own.curves[e.curve];
    } else if (static_cast<std::size_t>(e.offset) + e.size <= s.want.image.size()) {
        write_field(s.want.image, e, e.touched ? e.value : e.stock);
    }
    s.write_due = true;
}
void remember(const Entry &e) {
    auto &s = state();
    if (e.touched || e.frozen) s.saved[e.key] = {e.value, e.frozen};
    else s.saved.erase(e.key);
}
void set_entry(Entry &e, double value) {
    e.value = sane(e, value);
    e.touched = e.value != e.stock;
    apply_entry(e);
    remember(e);
    // A linked value scales its graphs by the same ratio (the graphs are what the game reads).
    if (!e.drives.empty() && e.stock != 0) {
        const auto ratio = e.value / e.stock;
        const auto targets = e.drives; // set_entry may not touch `e` again, but keep the list stable
        auto &s = state();
        for (const auto index : targets)
            if (index < s.entries.size() && !s.entries[index].frozen) {
                auto &target = s.entries[index];
                const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
                set_entry(target, multiplier ? ratio : target.stock * ratio);
            }
    }
}
// A value a locked linked value drives (its graphs, the push class's speeds) is locked with it:
// resetting it alone would leave the lock showing a number that no longer does anything.
bool is_locked(const Entry &e) {
    if (e.frozen) return true;
    auto &s = state();
    const auto index = static_cast<std::size_t>(&e - s.entries.data());
    return std::ranges::any_of(s.entries, [&](const Entry &other) {
        return other.frozen && std::ranges::find(other.drives, index) != other.drives.end();
    });
}
Entry *find_entry(std::string_view id) {
    auto &s = state();
    const auto found = s.index.find(lower(id));
    return found == s.index.end() ? nullptr : &s.entries[found->second];
}
void build_entries() {
    auto &s = state();
    s.entries.clear();
    s.index.clear();
    s.class_entries.clear();
    const auto &m = *s.model;
    const auto add = [&](std::string name, Kind kind, std::uint16_t offset, std::uint16_t size, std::size_t curve) {
        if (name.empty()) name = std::format("Unnamed.0x{:x}", offset);
        Entry e;
        e.id = kind == Kind::curve || kind == Kind::graph ? name + " x" : name;
        e.key = lower(e.id);
        if (s.index.contains(e.key)) { // arrays of structures repeat names
            e.id += std::format(" @0x{:x}", offset);
            e.key = lower(e.id);
        }
        const auto dot = name.find('.');
        auto group = dot == std::string::npos ? std::string("General") : name.substr(0, dot);
        for (const std::string_view prefix : {"SkatePhysics", "Physics"})
            if (group.size() > prefix.size() && group.starts_with(prefix)) {
                group.erase(0, prefix.size());
                break;
            }
        e.group = spaced(group);
        e.label = spaced(dot == std::string::npos ? name : name.substr(dot + 1));
        if (kind == Kind::curve || kind == Kind::graph) e.label += " (x)";
        e.kind = kind;
        e.offset = offset;
        e.size = size;
        e.curve = curve;
        e.used = value_used(offset);
        s.index.emplace(e.key, s.entries.size());
        s.entries.push_back(std::move(e));
    };
    for (std::size_t i = 0; i < m.fields.size(); ++i) {
        const auto &f = m.fields[i];
        add(i < m.field_names.size() ? m.field_names[i] : std::string{}, f.real ? Kind::real : f.flag ? Kind::flag : Kind::integer,
            f.offset, f.size, 0);
    }
    for (std::size_t i = 0; i < m.curve_slots.size(); ++i)
        add(i < m.curve_names.size() ? m.curve_names[i] : std::string{}, Kind::curve, m.curve_slots[i], 8, i);
    // Graphs stored in the asset: sixteen X and Y values, a point count and bounds. Each
    // becomes one multiplier row; its points stay editable but out of the way.
    const auto plain = s.entries.size();
    for (std::size_t i = 0; i < plain; ++i) {
        if (!s.entries[i].id.ends_with(".Y0") || s.entries[i].kind != Kind::real) continue;
        const auto parent = s.entries[i].id.substr(0, s.entries[i].id.size() - 3);
        const auto offset = s.entries[i].offset;
        std::vector<std::size_t> outputs;
        for (std::size_t j = 0; j < plain; ++j) {
            auto &other = s.entries[j];
            if (other.id.size() <= parent.size() + 1 || !other.id.starts_with(parent) || other.id[parent.size()] != '.') continue;
            const std::string_view leaf(other.id.c_str() + parent.size() + 1);
            if (leaf.find('.') != std::string_view::npos) continue;
            other.detail = true;
            const bool output = (leaf.starts_with("Y") && leaf.size() <= 3) || leaf == "Min_Y" || leaf == "Max_Y";
            if (output && other.kind == Kind::real) outputs.push_back(j);
        }
        add(parent, Kind::graph, offset, 0, 0);
        s.entries.back().members = std::move(outputs);
    }
    // The game's data-defined classes: one row per number field, under the class's name.
    for (std::size_t c = 0; c < class_count; ++c)
        for (std::size_t i = 0; i < class_specs[c].count; ++i) {
            const auto &field = class_fields[class_specs[c].first + i];
            Entry e;
            e.id = std::format("{}.{}", class_specs[c].key, field.name);
            e.key = lower(e.id);
            if (s.index.contains(e.key)) continue;
            e.group = class_specs[c].group;
            e.label = spaced(field.name);
            e.kind = Kind::real;
            e.used = field.seen;
            e.class_field = static_cast<int>(class_specs[c].first + i);
            e.stock = e.value = field.stock;
            s.index.emplace(e.key, s.entries.size());
            s.class_entries.push_back(s.entries.size());
            s.entries.push_back(std::move(e));
        }
    for (const auto &link : value_links()) {
        const auto from = s.index.find(link.value), to = s.index.find(link.drives);
        if (from == s.index.end() || to == s.index.end()) continue;
        s.entries[from->second].drives.push_back(to->second);
        s.entries[from->second].used = true;
    }
    // A host's values may have arrived before there were entries to hold them.
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    s.extras_due = true;
    say(logging::Level::info, std::format("Trainer: {} tuning values and {} curves are editable.", m.fields.size(), m.curve_slots.size()));
}
// A new copy of the asset (first sight, or a level load): its values are the stock ones.
void adopt(const tuning::Values &live) {
    auto &s = state();
    s.own = live;
    s.want = live;
    for (auto &e : s.entries) {
        e.stock = e.class_field >= 0 ? class_fields[e.class_field].stock : e.kind == Kind::curve || e.kind == Kind::graph ? 1.0 : read_field(s.own.image, e);
        const auto found = s.saved.find(e.key);
        if (found != s.saved.end()) {
            e.frozen = found->second.frozen;
            e.value = sane(e, found->second.value);
            e.touched = e.value != e.stock;
        } else if (!e.touched) {
            e.value = e.stock;
        }
        if (e.touched) apply_entry(e);
    }
    // A linked value saved by itself (before links existed, or edited in the file) still
    // drives its graphs.
    for (auto &e : s.entries) {
        if (e.drives.empty() || !e.touched || e.stock == 0) continue;
        for (const auto index : e.drives) {
            auto &target = s.entries[index];
            if (target.touched || target.frozen) continue;
            const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
            target.value = sane(target, multiplier ? e.value / e.stock : target.stock * e.value / e.stock);
            target.touched = target.value != target.stock;
            if (target.touched) {
                apply_entry(target);
                remember(target);
            }
        }
    }
    s.write_due = true;
    changed(false);
}
std::string apply_preset(std::string_view name);
bool editable(std::string *why = nullptr) {
    // A guest's physics are the host's while the session enforces them: the tuning
    // (physics_tuning::enforce), and what the host shares beyond it (sync_session). Writing
    // over either would only fight the session, so the player's own stand down.
    if (session_tuning_enforced()) {
        if (why) *why = "You are in a multiplayer game where the host controls physics. This menu will not work in this multiplayer game.";
        return false;
    }
    return true;
}
// Auto push is the trainer's doing: the game's flag reaches only its animation.
float own_cruise() {
    const auto *auto_push = find_entry("physicsmode.autopushenabled"), *auto_speed = find_entry("physicspush.maxspeedforautopush");
    return auto_push && auto_speed && auto_push->touched && auto_push->value != 0 ? static_cast<float>(auto_speed->value) : 0.0f;
}
// The host's where a session's host sets everyone's physics, whatever the boosts switch says
// (that switch is about a guest's own); otherwise the player's, and the game's own in a
// session that has turned boosts off.
// A session where the player's own trick extras do not count: its host sets the physics, or has turned boosts off.
bool extras_blocked() { return state().enforced || (multiplayer_session_active() && !session_boosts_allowed()); }
// With the catch set by the air time, the flip itself has to be slow enough for that limit to be
// what ends it (the game's own flip is over in a third of a second).
constexpr float catch_flip_speed = 0.25f;
Boosts boosts_in_force() {
    auto &s = state();
    if (s.enforced)
        return {s.host.flip_speed, s.host.hippy_height, s.host.nocomply_height, s.host.boneless_height, s.host.offboard_height, s.host.cruise};
    if (multiplayer_session_active() && !session_boosts_allowed()) return {};
    return {s.catch_at ? std::min(s.flip_speed, catch_flip_speed) : s.flip_speed, s.hippy_height, s.nocomply_height, s.boneless_height, s.offboard_height, own_cruise()};
}
// The player's own, for a session they host to share with its guests.
void publish_extras() {
    auto &s = state();
    s.extras_due = false;
    SessionExtras own;
    own.flip_speed = s.flip_speed;
    own.hippy_height = s.hippy_height;
    own.nocomply_height = s.nocomply_height;
    own.boneless_height = s.boneless_height;
    own.offboard_height = s.offboard_height;
    own.cruise = own_cruise();
    for (const auto index : s.class_entries)
        if (const auto &e = s.entries[index]; e.touched)
            own.classes.emplace_back(static_cast<std::uint16_t>(e.class_field), static_cast<float>(e.value));
    set_local_physics_extras(encode_session_extras(own));
}
// Follows a session into and out of its host's physics, and the host's changes while there.
void sync_session() {
    auto &s = state();
    const bool enforced = session_tuning_enforced();
    std::vector<std::uint8_t> bytes;
    bool fresh = false;
    if (enforced) fresh = host_physics_extras(s.host_revision, bytes);
    else s.host_revision = 0; // read again the next time a host sets the physics
    if (enforced == s.enforced && !fresh) return;
    if (fresh) {
        auto decoded = decode_session_extras(bytes);
        if (!decoded) say(logging::Level::warning, "Trainer: what the host shares beyond its tuning could not be read; the game's own is used for it.");
        s.host = decoded ? std::move(*decoded) : SessionExtras{};
        for (std::size_t i = 0; i < class_field_count; ++i) s.host_classes[i] = class_fields[i].stock;
        for (const auto &[index, value] : s.host.classes) s.host_classes[index] = value;
        if (!s.host.stock())
            say(logging::Level::info, std::format("Trainer: skating with the host's trick multipliers and {} of its class values.", s.host.classes.size()));
    }
    s.enforced = enforced;
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    changed(false);
}
void hold(std::uint64_t now) {
    auto &s = state();
    if (!s.base) return;
    if (!s.model) {
        tuning::prepare();
        std::string error;
        s.model = tuning::game_tuning(&error);
        if (!s.model) {
            const auto next = error.empty() ? std::string("Reading the game's physics tuning...") : "Physics tuning is unavailable: " + error + ".";
            if (next != s.status) { s.status = next; changed(false); }
            return;
        }
        build_entries();
    }
    if (!s.write_due && now < s.next_hold) return;
    s.next_hold = now + 500;
    tuning::Values live;
    std::uintptr_t asset{};
    if (!tuning::read_live(s.base, live, &asset)) {
        if (s.ready) { s.ready = false; s.status = "Waiting for a level."; changed(false); }
        return;
    }
    if (asset != s.asset || !s.ready) {
        s.asset = asset;
        adopt(live);
        s.ready = true;
        s.status = "Ready.";
    }
    if (!editable()) return;
    if (s.profile_due) {
        s.profile_due = false;
        if (const auto found = s.maps.find(s.map); found != s.maps.end() && !found->second.preset.empty())
            say(logging::Level::info, "Trainer: this map's preset - " + apply_preset(found->second.preset));
    }
    const bool pending = s.write_due || std::ranges::any_of(s.entries, [](const Entry &e) { return e.touched; });
    s.write_due = false;
    if (!pending) return;
    const auto written = tuning::write_live(s.base, s.entity, s.want);
    if (written.failed) {
        s.status = "The physics tuning could not be written.";
        changed(false);
    }
    // The skater skates from its own copy of part of the tuning: keep asking until it took it.
    if (written.values || written.curves) {
        s.refresh_due = !written.refreshed;
        say(logging::Level::info, std::format("Trainer: wrote {} values and {} curves; skater copy refreshed: {}.", written.values, written.curves,
                                              written.refreshed ? "yes" : "not yet"));
    } else if (s.refresh_due && tuning::refresh_skater(s.base, s.entity)) {
        s.refresh_due = false;
        say(logging::Level::info, "Trainer: skater copy refreshed.");
    }
}

// ---- presets ---------------------------------------------------------------------------
bool matches(const std::string &key, std::string_view pattern) {
    std::size_t start = 0;
    while (start < pattern.size()) {
        auto end = pattern.find(' ', start);
        if (end == std::string_view::npos) end = pattern.size();
        const auto token = pattern.substr(start, end - start);
        if (!token.empty()) {
            const bool exclude = token.front() == '!';
            const auto word = exclude ? token.substr(1) : token;
            if ((key.find(word) != std::string::npos) == exclude) return false;
        }
        start = end + 1;
    }
    return true;
}
// What each built-in preset's rules reach in the running game: (entry, rule) pairs, worked out
// once per table of entries.
// Whether a preset rule is about this value (`scale`: it is a curve or graph multiplier).
bool rule_matches(const Entry &e, bool scale, const PresetRule &rule) {
    if (rule.exact) return !scale && e.key == rule.pattern;
    return !e.detail && scale == rule.curves && matches(e.key, rule.pattern);
}
const std::vector<std::vector<std::pair<std::size_t, std::size_t>>> &preset_targets() {
    static std::vector<std::vector<std::pair<std::size_t, std::size_t>>> cache;
    static const Entry *cached_for{};
    static std::size_t cached_count{};
    auto &s = state();
    if (cached_for == s.entries.data() && cached_count == s.entries.size() && !cache.empty()) return cache;
    cached_for = s.entries.data();
    cached_count = s.entries.size();
    const auto &presets = builtin_presets();
    cache.assign(presets.size(), {});
    for (std::size_t p = 0; p < presets.size(); ++p)
        for (std::size_t r = 0; r < presets[p].rules.size(); ++r) {
            const auto &rule = presets[p].rules[r];
            for (std::size_t i = 0; i < s.entries.size(); ++i) {
                const auto &e = s.entries[i];
                const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                if (!rule_matches(e, scale, rule)) continue;
                if (e.kind == Kind::flag && rule.multiply) continue;
                cache[p].emplace_back(i, r);
            }
        }
    return cache;
}
// A preset as a dial: `factor` is where its first value goes (x of stock); the others follow in
// proportion, so 1 is the game's own and the first rule's amount is the preset as it ships.
std::string apply_dial(std::string_view name, double factor) {
    auto &s = state();
    const auto &presets = builtin_presets();
    const auto wanted = lower(name);
    std::size_t p = 0;
    while (p < presets.size() && lower(presets[p].name) != wanted) ++p;
    if (p == presets.size()) return "error: no built-in preset is called \"" + std::string(name) + "\".";
    const auto &rules = presets[p].rules;
    if (rules.empty() || !rules[0].multiply || rules[0].amount <= 0 || rules[0].amount == 1)
        return "error: " + std::string(presets[p].name) + " is a switch, not a dial.";
    if (!(factor > 0) || !std::isfinite(factor)) return "error: a dial needs a multiplier above 0; 1 is the game's own.";
    const double t = std::log(factor) / std::log(rules[0].amount);
    std::size_t count{}, locked{};
    for (const auto &[index, r] : preset_targets()[p]) {
        auto &e = s.entries[index];
        const auto &rule = rules[r];
        if (is_locked(e)) {
            ++locked;
            continue;
        }
        if (rule.multiply) set_entry(e, rule.amount > 0 ? e.stock * std::pow(rule.amount, t) : e.stock * (1 + (rule.amount - 1) * t));
        else set_entry(e, t > 0.01 ? rule.amount : e.stock); // a switch that goes with the preset's direction
        ++count;
    }
    std::erase(s.active, std::string(presets[p].name));
    if (std::abs(t - 1) < 1e-3) s.active.emplace_back(presets[p].name);
    changed();
    return std::format("{} x{:.2f}: {} values set{}.", presets[p].name, factor, count, locked ? std::format("; {} locked values left alone", locked) : "");
}
// A preset of the player's own also carries the trick sliders that are off 1, under these keys.
constexpr std::string_view trick_prefix = "trick.";
constexpr std::string_view trick_sliders[]{"flip_speed", "hippy_height", "nocomply_height", "boneless_height", "offboard_height", "revert_boost", "pump_power"};
// Those and the speed of each flip trick.
constexpr auto trick_names = [] {
    std::array<std::string_view, std::size(trick_sliders) + flip_tricks.size()> names{};
    std::size_t at{};
    for (const auto name : trick_sliders) names[at++] = name;
    for (const auto &trick : flip_tricks) names[at++] = trick.key;
    return names;
}();
constexpr std::string_view flip_gate_name = "flip_gate", catch_at_name = "catch_at", catch_percent_name = "catch_percent", flip_advanced_name = "flip_advanced";
bool trick_switch(std::string_view name) { return name == flip_gate_name || name == catch_at_name || name == catch_percent_name || name == flip_advanced_name; }
// What a trick slider or switch is when the game is left alone: the multipliers 1, the revert boost and the switches off.
constexpr float trick_stock(std::string_view name) {
    return name == "revert_boost" || name == catch_at_name || name == flip_advanced_name ? 0.0f : name == catch_percent_name ? 70.0f : 1.0f;
}
float *trick_option(std::string_view name) {
    auto &s = state();
    for (std::size_t i = 0; i < flip_tricks.size(); ++i)
        if (name == flip_tricks[i].key) return &s.flip_trick[i];
    return name == "flip_speed" ? &s.flip_speed : name == "hippy_height" ? &s.hippy_height : name == "nocomply_height" ? &s.nocomply_height
         : name == "boneless_height" ? &s.boneless_height : name == "offboard_height" ? &s.offboard_height
         : name == "revert_boost" ? &s.revert_boost : name == "pump_power" ? &s.pump_power : nullptr;
}
// Sets a trick slider named by a preset key; false when the key is not one.
bool set_trick_key(std::string_view key, double value) {
    if (!key.starts_with(trick_prefix)) return false;
    const auto name = key.substr(trick_prefix.size());
    // (flip_gate, a switch of earlier versions, still comes in old presets: a slow flip stays slow by itself now.)
    if (name == catch_at_name) state().catch_at = value != 0;
    if (name == flip_advanced_name) state().flip_advanced = value != 0;
    if (name == catch_percent_name) state().catch_percent = std::clamp(static_cast<float>(value), catch_low, catch_high);
    if (auto *option = trick_option(name))
        *option = name == "flip_speed" || name.starts_with("flip.") ? std::clamp(static_cast<float>(value), flip_low, flip_high)
                : name == "revert_boost" ? std::clamp(static_cast<float>(value), 0.0f, revert_boost_high)
                : name == "pump_power" ? std::clamp(static_cast<float>(value), 0.0f, pump_high)
                : std::clamp(static_cast<float>(value), height_low, height_high);
    return true;
}
std::string remove_preset(std::string_view name) {
    auto &s = state();
    const auto wanted = lower(name);
    std::size_t count{};
    std::string removed;
    const auto put_back = [&](Entry &e) {
        if (is_locked(e)) return;
        if (e.touched) {
            set_entry(e, e.stock);
            ++count;
            return;
        }
        // A linked value at stock whose graphs were set directly (by an older version's preset).
        for (const auto index : e.drives)
            if (auto &target = s.entries[index]; target.touched && !is_locked(target)) {
                set_entry(target, target.stock);
                ++count;
            }
    };
    for (const auto &[user_name, values] : s.user) {
        if (lower(user_name) != wanted) continue;
        for (const auto &saved : values) {
            if (saved.first.starts_with(trick_prefix)) {
                (void)set_trick_key(saved.first, trick_stock(std::string_view(saved.first).substr(trick_prefix.size())));
                ++count;
            }
            else if (auto *e = find_entry(saved.first)) put_back(*e);
        }
        removed = user_name;
        break;
    }
    if (removed.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (!rule_matches(e, scale, rule)) continue;
                    put_back(e);
                }
            removed = preset.name;
            break;
        }
    if (removed.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    std::erase(s.active, removed);
    // Presets that are still on may share values with the one switched off: set theirs again.
    const auto still_on = s.active;
    for (const auto &other : still_on) (void)apply_preset(other);
    changed();
    return std::format("{} off: {} values put back.", removed, count);
}
std::size_t reset_all() {
    auto &s = state();
    std::size_t count{};
    for (auto &e : s.entries) {
        if (!e.touched || is_locked(e)) continue;
        set_entry(e, e.stock);
        ++count;
    }
    s.active.clear();
    return count;
}
std::string apply_preset(std::string_view name) {
    auto &s = state();
    const auto wanted = lower(name);
    if (wanted == "stock") {
        const auto count = reset_all();
        changed();
        return std::format("Stock: {} values put back (frozen values kept).", count);
    }
    std::size_t count{}, locked{};
    std::string applied;
    if (wanted == "map") {
        if (s.map_file.preset.empty()) return "error: this map ships no trainer preset.";
        for (const auto &[id, value] : s.map_file.preset)
            if (auto *e = find_entry(id); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
        applied = s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    }
    if (applied.empty())
        for (const auto &[user_name, values] : s.user) {
            if (lower(user_name) != wanted) continue;
            for (const auto &[key, value] : values) {
                if (set_trick_key(key, value)) ++count;
                else if (auto *e = find_entry(key); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
            }
            applied = user_name;
            break;
        }
    if (applied.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (!rule_matches(e, scale, rule)) continue;
                    if (e.kind == Kind::flag && rule.multiply) continue;
                    if (is_locked(e)) {
                        ++locked;
                        continue;
                    }
                    set_entry(e, rule.multiply ? e.stock * rule.amount : rule.amount);
                    ++count;
                }
            applied = preset.name;
            break;
        }
    if (applied.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    if (std::ranges::find(s.active, applied) == s.active.end()) s.active.push_back(applied);
    changed();
    return std::format("{}: {} values set{}.", applied, count,
                       locked ? std::format("; {} locked values left alone (untick their boxes in Settings to let presets change them)", locked) : "");
}

// ---- maps ------------------------------------------------------------------------------
MapFile read_map_file(const std::string &level) {
    MapFile result;
    const auto mods = game_directory() / L"Mods";
    std::error_code error;
    for (std::filesystem::directory_iterator it(mods, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        const auto json = read_json(it->path() / L"trainer.json");
        if (!json || !json->is_object()) continue;
        try {
            // The file names its level, or it stands for every level its mod adds.
            bool mine = false;
            if (json->contains("level") && json->at("level").is_string()) mine = lower(json->at("level").string()) == level;
            else if (const auto levels = read_json(it->path() / L"reskate-levels.json"); levels && levels->contains("levels"))
                for (const auto &row : levels->at("levels"))
                    if (row.is_object() && lower(row.value("asset", "")) == level) mine = true;
            if (!mine) continue;
            result.note = json->value("note", "");
            if (json->contains("preset") && json->at("preset").is_object()) {
                const auto &preset = json->at("preset");
                result.preset_name = preset.value("name", "");
                if (preset.contains("values") && preset.at("values").is_object())
                    for (const auto &[id, value] : preset.at("values").items())
                        if (value.is_number() && result.preset.size() < 512) result.preset.emplace_back(id, value.get<double>());
            }
            if (json->contains("spots") && json->at("spots").is_array())
                for (const auto &row : json->at("spots")) {
                    if (!row.is_object() || !row.contains("position") || result.spots.size() >= 64) continue;
                    if (const auto position = read_position(row.at("position")))
                        result.spots.push_back({row.value("name", "Spot"), *position});
                }
            return result;
        } catch (...) {}
    }
    return result;
}
void clear_live_tricks() {
    auto &s = state();
    if (s.flip_live || s.pump_live || s.flip_drive.active || s.flip_trick_seen) s.view_due = true;
    s.flip_drive = {};
    s.flip_live = s.pump_live = false;
    s.flip_trick_seen = 0;
    s.catch_seconds = flip_gate_stock;
}
void enter_map(const std::string &level) {
    auto &s = state();
    clear_live_tricks();
    s.entity = 0;
    s.telemetry.skater = false;
    s.map = level;
    s.map_file = level.empty() ? MapFile{} : read_map_file(level);
    s.motion = {};
    s.telemetry.last = {};
    s.telemetry.best = {};
    s.telemetry.top_speed = 0;
    s.return_at = 0;
    s.profile_due = false;
    // A level brings fresh copies of the game's tuning classes: look for them once it has settled.
    s.class_search_at = level.empty() ? 0 : GetTickCount64() + 10000;
    s.class_search_tries = 0;
    s.class_search_seen = class_searches();
    s.class_skater_wait = class_skater_wait_low;
    s.class_skater_after = 0;
    s.class_skater_due = false;
    s.class_skater_judged = true;
    s.class_list_wanted = false;
    if (!level.empty()) {
        const auto found = s.maps.find(level);
        s.profile_due = found != s.maps.end() && !found->second.preset.empty();
        if (!s.map_file.spots.empty() || !s.map_file.preset.empty())
            say(logging::Level::info, std::format("Trainer: the map ships {} spots and {} preset values.", s.map_file.spots.size(),
                                                    s.map_file.preset.size()));
    }
    changed(false);
}
MapData &current_map() { return state().maps[state().map]; }
bool teleport_allowed(std::string &why) {
    if (multiplayer_session_active() && !session_noclip_allowed()) {
        why = "The host has turned off teleporting in this session.";
        return false;
    }
    return true;
}
std::string go_to(const Vec3 &position, const std::string &what) {
    std::string why;
    if (!teleport_allowed(why)) return "error: " + why;
    if (!teleport_local_skater(position)) return "error: teleporting is unavailable right now.";
    state().motion.valid = false;
    return "Teleporting to " + what + ".";
}

// ---- telemetry -------------------------------------------------------------------------
std::int64_t clock_now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
double clock_seconds(std::int64_t ticks) {
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<double>(ticks) / frequency;
}
float horizontal(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[2] * v[2]); }
void finish_jump(const Vec3 &landing, float landing_speed, std::int64_t landed_at) {
    auto &s = state();
    auto &m = s.motion;
    const auto air = static_cast<float>(clock_seconds(landed_at - m.takeoff_stamp));
    m.airborne = false;
    s.telemetry.airborne = false;
    if (air < 0.25f) return;
    Jump jump;
    jump.serial = s.telemetry.last.serial + 1;
    jump.air_time = air;
    jump.takeoff = m.takeoff;
    jump.landing = landing;
    jump.takeoff_speed = std::sqrt(m.takeoff_velocity[0] * m.takeoff_velocity[0] + m.takeoff_velocity[1] * m.takeoff_velocity[1] +
                                   m.takeoff_velocity[2] * m.takeoff_velocity[2]);
    jump.takeoff_angle = std::atan2(m.takeoff_velocity[1], std::max(horizontal(m.takeoff_velocity), 0.01f)) * 180.0f / std::numbers::pi_v<float>;
    jump.height = m.apex - m.takeoff[1];
    jump.distance = horizontal({landing[0] - m.takeoff[0], 0, landing[2] - m.takeoff[2]});
    jump.drop = m.takeoff[1] - landing[1];
    jump.landing_speed = landing_speed;
    jump.spin = std::abs(m.spin);
    jump.spin_rate = m.spin_rate;
    jump.flip = m.flip;
    jump.board_turn = m.board_turn;
    jump.state = m.air_state;
    s.telemetry.last = jump;
    if (jump.distance > s.telemetry.best.distance) s.telemetry.best = jump;
    // In the log for a player who is measuring: the read-out is on, or telemetry is recorded.
    if (!s.hud_jump && !s.logging) return;
    say(logging::Level::info,
        std::format("Trainer jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, "
                    "landing {:.1f} km/h at ({:.1f}, {:.1f}, {:.1f}); spin {:.0f} deg (peak {:.0f} deg/s), flip {:.0f} deg, "
                    "board {:.0f} deg/s, states {}>{}.",
                    jump.serial, jump.takeoff_speed * 3.6f, jump.takeoff_angle, jump.air_time, jump.height, jump.distance, jump.drop,
                    jump.landing_speed * 3.6f, landing[0], landing[1], landing[2], jump.spin, jump.spin_rate, jump.flip, jump.board_turn, m.ground_state,
                    m.air_state));
}
// Board flip tricks turn at the game's speed times the player's. The trick scripts set
// Float.Anim.TrickFlipSpeed when the board is flicked; while the skater is in the air the
// trainer keeps it at that number times the flip speed (and, with "Advanced trick speed", times
// the speed of the trick that is turning). On the ground the game's own number is put back.
void drive_flip_speed(std::uintptr_t base, bool airborne) {
    auto &s = state();
    auto &drive = s.flip_drive;
    const auto now = read_flip_state(base, static_cast<std::uintptr_t>(s.entity));
    if (s.flip_live != static_cast<bool>(now)) {
        s.flip_live = static_cast<bool>(now);
        s.view_due = true;
    }
    // A new skater, state page, handle generation or trick must never inherit the last
    // animation's baseline, even when its speed happens to equal what we wrote.
    if (drive.active && (!now || !physics_policy::same_flip_target(drive.observed, now.observation()))) drive = {};
    if (!now || !airborne) {
        if (now && drive.active && drive.written != drive.game &&
            physics_policy::can_write_flip({drive.observed.owner, drive.observed.trick, drive.written}, now.observation(), drive.game))
            (void)write_flip_speed(now, drive.game);
        drive = {};
        return;
    }
    if (now.trick) s.flip_trick_seen = now.trick;
    const auto index = flip_trick_index(now.trick);
    const float individual = index >= 0 ? s.flip_trick[static_cast<std::size_t>(index)] : 1.0f;
    const float factor = physics_policy::effective_flip_factor(s.boosts.flip, individual, s.flip_advanced, s.catch_at, extras_blocked());
    // A number that is not the trainer's own is the game's: the flight's first, or a new trick's.
    if (!drive.active || now.speed != drive.written) drive.game = now.speed;
    drive.active = true;
    drive.observed = now.observation();
    drive.factor = factor;
    drive.written = std::clamp(drive.game * factor, 0.0f, 1000.0f);
    if (now.speed != drive.written && !write_flip_speed(now, drive.written)) {
        drive = {}; // an unsuccessful write is never a baseline to restore later
        s.flip_live = false;
        s.view_due = true;
    }
}
void observe(std::uintptr_t base, std::uintptr_t client, std::uint64_t now) {
    auto &s = state();
    auto &t = s.telemetry;
    auto &m = s.motion;
    overlay::DebugModel skater;
    std::array<float, 16> matrix{};
    bool found = false;
    if (now >= s.next_skater) {
        try {
            matrix = client_source::detail::debug_skater(base, client, skater);
            found = skater.skater_position_valid;
        } catch (...) {
            s.next_skater = now + 250; // no skater: menus, loading, a respawn
        }
    }
    if (!found) {
        clear_live_tricks();
        t.skater = false;
        m.valid = false;
        s.entity = 0;
        return;
    }
    if (s.entity != skater.skater_identity) {
        clear_live_tricks();
        m = {};
        s.last_state = 0;
        s.offboard_grounded = false;
        s.boost_factor = 0;
    }
    s.entity = skater.skater_identity;
    watch_physics_state(client, s.entity);
    const auto watch = watched_physics_state();
    const auto stamp = clock_now();
    const Vec3 position = skater.skater_position;
    t.skater = true;
    t.position = position;
    // Rows: right, up, forward, position.
    t.heading = std::atan2(matrix[8], matrix[10]) * 180.0f / std::numbers::pi_v<float>;
    if (t.heading < 0) t.heading += 360.0f;
    t.physics_state = watch.valid ? watch.state : 0;
    if (watch.valid) ++s.states[watch.state];
    // A hippy jump leaves the board (504) straight from riding (100): measured. A plain
    // dismount does the same but is not rising, so it is left alone.
    if (watch.valid && watch.state != s.last_state) {
        if (watch.state == addr::no_bail::offboard_physics_state && s.last_state == 100 && s.boosts.hippy != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.hippy); // height goes with the square of the speed
            s.boost_until = now + 250;
            s.boost_name = "hippy jump";
        }
        s.last_state = watch.state;
    }
    if (s.boost_factor > 0) {
        const auto result = take_jump_scale_result();
        if (result.outcome > 0) {
            say(logging::Level::info, std::format("Trainer: {} boosted: up speed {:.2f} -> {:.2f} m/s.", s.boost_name, result.up_speed,
                                                  result.up_speed * s.boost_factor));
            s.boost_factor = 0;
        } else if (now >= s.boost_until || result.outcome == -2) {
            s.boost_factor = 0;
        } else {
            // The factor comes from the multipliers in force, so a session's rules are already in it.
            (void)queue_jump_scale(client, s.entity, s.boost_factor); // again each tick until the skater is rising
        }
    }
    const auto seconds = m.valid ? clock_seconds(stamp - m.stamp) : 0.0;
    const Vec3 step{position[0] - m.position[0], position[1] - m.position[1], position[2] - m.position[2]};
    const bool jumped = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]) > 25.0f;
    if (!m.valid || seconds <= 0.0005 || seconds > 0.25 || jumped) {
        // First sample, a hitch, or a teleport: nothing can be measured across it.
        if (seconds > 0.25 || jumped) clear_live_tricks();
        m = {};
        m.valid = true;
        m.position = position;
        m.stamp = stamp;
        t.airborne = false;
        t.air_time = t.height = 0;
        return;
    }
    const auto dt = static_cast<float>(seconds);
    const Vec3 raw{step[0] / dt, step[1] / dt, step[2] / dt};
    const Vec3 velocity{m.velocity[0] * 0.5f + raw[0] * 0.5f, m.velocity[1] * 0.5f + raw[1] * 0.5f, m.velocity[2] * 0.5f + raw[2] * 0.5f};
    // An on-foot jump: off the board, level at the last tick and now rising faster than stairs
    // or a slope can lift a runner.
    if (watch.valid && watch.state == addr::no_bail::offboard_physics_state) {
        if (s.offboard_grounded && raw[1] > 2.2f && s.boost_factor == 0 && s.boosts.offboard != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.offboard);
            s.boost_until = now + 250;
            s.boost_name = "off-board jump";
        }
        s.offboard_grounded = std::abs(raw[1]) < 0.6f;
    } else {
        s.offboard_grounded = false;
    }
    // The skater's physics says when it is in the air: its states 200-299 are the pop and
    // the flight (100s ride the ground, 300 is a wipeout, 500s are off the board).
    const bool flying = watch.valid && watch.state >= air_states_first && watch.state < air_states_end;
    // Rotation in the air: about the vertical (spins) and of the up axis (flips and rolls).
    const Vec3 forward{matrix[8], matrix[9], matrix[10]}, up{matrix[4], matrix[5], matrix[6]};
    if (m.airborne && m.axes_valid) {
        const float yaw = std::atan2(m.forward[2] * forward[0] - m.forward[0] * forward[2], m.forward[0] * forward[0] + m.forward[2] * forward[2]);
        const float degrees = yaw * 180.0f / std::numbers::pi_v<float>;
        // A ragdoll or a pose snap turns further in one tick than any trick does: leave those out.
        if (std::abs(degrees) < 45.0f) {
            m.spin += degrees;
            m.spin_rate = std::max(m.spin_rate, std::abs(degrees) / dt);
        }
        const float cosine = std::clamp(m.up[0] * up[0] + m.up[1] * up[1] + m.up[2] * up[2], -1.0f, 1.0f);
        if (const float tumble = std::acos(cosine) * 180.0f / std::numbers::pi_v<float>; tumble < 45.0f) m.flip += tumble;
    }
    m.forward = forward;
    m.up = up;
    m.axes_valid = true;
    if (!flying && watch.valid) m.ground_state = watch.state;
    if (flying && !m.airborne) {
        m.spin = m.spin_rate = m.flip = m.board_turn = 0;
        m.air_state = watch.state;
        m.airborne = true;
        m.takeoff = m.position;
        m.takeoff_velocity = velocity;
        m.takeoff_stamp = m.stamp;
        m.apex = std::max(m.position[1], position[1]);
    }
    // On the ground the limit is the game's own: the number left by the last flight means nothing for the next.
    if (!m.airborne) state().catch_seconds = flip_gate_stock;
    drive_flip_speed(base, m.airborne);
    if (m.airborne) {
        m.apex = std::max(m.apex, position[1]);
        t.air_time = static_cast<float>(clock_seconds(stamp - m.takeoff_stamp));
        if (auto &own = state(); own.catch_at && flying) {
            // The whole flight, guessed from where the skater is and how fast they rise: the time
            // so far plus the time back down to the height they left from. Right over flat ground,
            // short over a drop. The game has a flip round `seconds` before the landing it
            // predicts, so the catch falls at the asked part of the flight.
            const auto *gravity = find_entry("onboard_speedmodel.gravityacceleration");
            const float g = std::max(1.0f, gravity ? static_cast<float>(gravity->touched ? gravity->value : gravity->stock) : 9.81f);
            const float rise = velocity[1], above = std::max(0.0f, position[1] - m.takeoff[1]);
            const float whole = t.air_time + (rise + std::sqrt(rise * rise + 2 * g * above)) / g;
            // A catch asked for earlier than a board can come round (a flick late in a short
            // jump) would make the limit negative, which the game reads as no limit at all and
            // the flip crawls (measured at 10%: big pops turned at 450 deg/s to the ground). Such a
            // flip is asked to come round as fast as the game turns one, about a fifth of a second.
            constexpr float fastest_flip = 0.2f;
            if (whole > 0.05f && whole < 60.0f)
                own.catch_seconds = std::min((1 - own.catch_percent / 100) * whole, whole - t.air_time - fastest_flip);
        }
        t.height = m.apex - m.takeoff[1];
        if (!flying)
            finish_jump(position, std::sqrt(m.velocity[0] * m.velocity[0] + m.velocity[1] * m.velocity[1] + m.velocity[2] * m.velocity[2]),
                        stamp);
    }
    t.airborne = m.airborne;
    t.speed = horizontal(velocity);
    t.vertical = velocity[1];
    t.top_speed = std::max(t.top_speed, t.speed);
    m.velocity = velocity;
    m.position = position;
    m.stamp = stamp;
    // How fast the board turns (flip tricks, shuvits): body 0's angular velocity (+0x90), read while a jump
    // is measured or telemetry is recorded.
    float board_turn = 0;
    bool turning = m.airborne || (s.logging && s.log);
    if (turning) {
        try {
            const auto bodies = client_source::detail::debug_noclip_bodies(base, client, s.entity);
            std::array<float, 3> spin{};
            if (!bodies.parts.empty() && memory::peek(bodies.parts[0] + 0x90, spin)) {
                board_turn = std::sqrt(spin[0] * spin[0] + spin[1] * spin[1] + spin[2] * spin[2]) * 180.0f / std::numbers::pi_v<float>;
                if (!std::isfinite(board_turn) || board_turn > 1.0e5f) board_turn = 0;
            }
        } catch (...) {}
        if (m.airborne) m.board_turn = std::max(m.board_turn, board_turn);
    }
    if (s.logging && s.log) {
        s.log << std::format("{:.4f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.1f},{},{},{:.0f}\n", clock_seconds(stamp - s.log_start), position[0],
                             position[1], position[2], t.speed, t.vertical, t.heading, t.airborne ? 1 : 0, t.physics_state, board_turn);
    }
    // A bail: back to the selected marker once the skater has had time to fall.
    if (watch.valid && (!s.wipeouts_known || watch.wipeouts != s.seen_wipeouts)) {
        if (s.wipeouts_known && s.auto_return) {
            const bool marked = current_map().markers[static_cast<std::size_t>(s.slot)].set;
            if (marked) s.return_at = now + static_cast<std::uint64_t>(s.return_delay * 1000.0f);
            say(logging::Level::info, marked ? std::format("Trainer: bail seen; back to marker {} in {:.1f} s.", s.slot + 1, s.return_delay)
                                             : std::format("Trainer: bail seen, but marker {} is empty.", s.slot + 1));
        }
        s.wipeouts_known = true;
        s.seen_wipeouts = watch.wipeouts;
    }
}
void set_logging(bool on) {
    auto &s = state();
    if (s.log.is_open()) s.log.close();
    s.logging = false;
    if (!on) return;
    const auto directory = data_directory() / L"telemetry";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::string name = s.map.empty() ? "unknown" : s.map.substr(s.map.find_last_of('/') + 1);
    for (auto &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    SYSTEMTIME time;
    GetLocalTime(&time);
    const auto file = directory / std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv", name, time.wYear, time.wMonth, time.wDay, time.wHour,
                                              time.wMinute, time.wSecond);
    s.log.open(file, std::ios::binary | std::ios::trunc);
    if (!s.log) return;
    s.log << "seconds,x,y,z,speed_mps,vertical_mps,heading_deg,airborne,physics_state,board_turn_dps\n";
    s.log_start = clock_now();
    s.logging = true;
}
void shortcuts() {
    auto &s = state();
    ControllerInput input;
    DingoSDKOverlayReadControllerInput(&input);
    const auto buttons = input.available ? input.buttons : 0;
    const auto pressed = buttons & ~s.pad_previous;
    s.pad_previous = buttons;
    if (!s.pad_shortcuts || (buttons & (xinput_lb | xinput_rb)) != (xinput_lb | xinput_rb)) return;
    std::string result;
    if (pressed & xinput_up) result = command("marker", {"save"});
    else if (pressed & xinput_down) result = command("marker", {"go"});
    else if (pressed & xinput_left) result = command("slot", {std::to_string((s.slot + static_cast<int>(marker_slots) - 1) % static_cast<int>(marker_slots) + 1)});
    else if (pressed & xinput_right) result = command("slot", {std::to_string((s.slot + 1) % static_cast<int>(marker_slots) + 1)});
    if (!result.empty()) overlay::notify(overlay::NoticeLevel::info, "TRAINER", result);
}

// ---- self test -------------------------------------------------------------------------
void test_line(bool ok, const std::string &text) {
    auto &s = state();
    if (!ok) ++s.test.failures;
    say(ok ? logging::Level::info : logging::Level::error, std::format("trainer selftest: {} {}", ok ? "OK" : "FAIL", text));
}
void run_selftest(std::uint64_t now) {
    auto &s = state();
    auto &t = s.test;
    if (!t.step || now < t.until) return;
    tuning::Values live;
    switch (t.step) {
    case 1: {
        test_line(s.model != nullptr, std::format("game tuning read ({} values, {} curves)", s.model ? s.model->fields.size() : 0,
                                                  s.model ? s.model->curve_slots.size() : 0));
        std::size_t named{};
        if (s.model) named = static_cast<std::size_t>(std::ranges::count_if(s.model->field_names, [](const std::string &n) { return !n.empty(); }));
        test_line(s.model && named == s.model->fields.size(), std::format("field names ({} of {})", named, s.model ? s.model->fields.size() : 0));
        test_line(s.ready && s.asset, "running tuning asset found");
        test_line(s.telemetry.skater, "local skater found");
        // The first plain real that is not zero and that the player has not touched.
        t.entry = s.entries.size();
        for (std::size_t i = 0; i < s.entries.size(); ++i)
            if (s.entries[i].kind == Kind::real && !s.entries[i].detail && !s.entries[i].touched && std::abs(s.entries[i].stock) > 0.01) {
                t.entry = i;
                break;
            }
        if (!s.ready || t.entry == s.entries.size() || !editable()) {
            test_line(false, "no value to write (tuning not ready or locked by a session)");
            t.step = 4;
            break;
        }
        auto &e = s.entries[t.entry];
        t.before = e.stock;
        t.target = e.stock * 1.01;
        set_entry(e, t.target);
        t.step = 2;
        t.until = now + 1200;
        break;
    }
    case 2: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && std::abs(read_field(live.image, e) - static_cast<float>(t.target)) < 1e-4 * std::max(1.0, std::abs(t.target)),
                  std::format("write and hold {} = {:.5g}", e.id, t.target));
        set_entry(e, t.before);
        t.step = 3;
        t.until = now + 1200;
        break;
    }
    case 3: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && read_field(live.image, e) == static_cast<float>(t.before) && !e.touched, std::format("restore {} = {:.5g}", e.id, t.before));
        t.step = 4;
        break;
    }
    case 4: {
        if (!s.telemetry.skater) {
            test_line(false, "teleport (no skater)");
            t.step = 6;
            break;
        }
        t.origin = s.telemetry.position;
        const Vec3 target{t.origin[0] + 3.0f, t.origin[1] + 1.0f, t.origin[2]};
        const auto result = go_to(target, "a test point");
        test_line(!result.starts_with("error"), "teleport request: " + result);
        t.origin = target;
        t.step = result.starts_with("error") ? 6 : 5;
        t.until = now + 700;
        break;
    }
    case 5: {
        const auto &p = s.telemetry.position;
        // The skater may already be rolling away from where it was put.
        const float away = horizontal({p[0] - t.origin[0], 0, p[2] - t.origin[2]});
        test_line(s.telemetry.skater && away < 25.0f, std::format("teleport put the skater {:.2f} m from the target", away));
        t.step = 6;
        t.until = now + 1500;
        break;
    }
    default:
        say(t.failures ? logging::Level::error : logging::Level::info,
            std::format("trainer selftest: DONE {} failures", t.failures));
        t = {};
        changed(false);
        break;
    }
}

void build_view() {
    auto &s = state();
    s.view_due = false;
    auto next = std::make_shared<View>();
    next->revision = ++s.revision;
    next->ready = s.ready;
    next->status = s.status;
    next->last = s.last;
    next->editable = editable(&next->blocked);
    next->session_enforced = s.enforced;
    next->boosts_blocked = !s.enforced && multiplayer_session_active() && !session_boosts_allowed();
    next->rows.reserve(s.entries.size());
    for (const auto &e : s.entries) {
        int rank{};
        std::uint8_t modes{};
        const auto friendly = essential_name(e.key, &rank, &modes);
        next->rows.push_back({e.id, e.label, e.group, e.kind, e.value, e.stock, e.touched, e.frozen, e.detail, std::string(friendly), rank, modes,
                              e.used});
        next->rows.back().help = essential_help(e.key);
        next->rows.back().preset_locked = is_locked(e);
        if (e.touched) ++next->touched;
        if (std::ranges::find(next->groups, e.group) == next->groups.end()) next->groups.push_back(e.group);
    }
    const auto is_active = [&](std::string_view name) { return std::ranges::find(s.active, name) != s.active.end(); };
    next->presets.push_back({"Stock", "Everything back to how this map loaded (frozen values stay).", true, s.active.empty() && !next->touched});
    {
        // A built-in preset is on when the values say so, whatever switched them: a slider, a
        // dial or another preset.
        const auto &presets = builtin_presets();
        const auto &targets = preset_targets();
        for (std::size_t p = 0; p < presets.size(); ++p) {
            const auto &preset = presets[p];
            PresetRow row{std::string(preset.name), std::string(preset.note), true, false};
            bool any = false, on = true;
            const Entry *headline{};
            for (const auto &[index, r] : targets[p]) {
                const auto &e = s.entries[index];
                const auto &rule = preset.rules[r];
                if (!headline && r == 0) headline = &e;
                const double expected = rule.multiply ? e.stock * rule.amount : rule.amount;
                any = true;
                // To the two decimals a dial shows: dragging one to the preset's number switches it on.
                if (std::abs(e.value - sane(e, expected)) > 4e-3 * std::max(1e-3, std::abs(expected))) on = false;
            }
            row.active = any && on;
            const auto dial = preset_dial(preset.name);
            row.title = std::string(dial.title);
            row.modes = dial.modes;
            if (!preset.rules.empty() && preset.rules[0].multiply && headline && headline->stock != 0 && !dial.title.empty()) {
                row.dial = true;
                row.amount = preset.rules[0].amount;
                row.factor = headline->value / headline->stock;
            }
            next->presets.push_back(std::move(row));
        }
    }
    for (const auto &[name, values] : s.user) {
        PresetRow row{name, std::format("{} values", values.size()), false, is_active(name)};
        row.values.assign(values.begin(), values.end());
        next->presets.push_back(std::move(row));
    }
    next->slot = s.slot;
    next->auto_return = s.auto_return;
    next->hippy_height = s.hippy_height;
    next->nocomply_height = s.nocomply_height;
    next->offboard_height = s.offboard_height;
    next->flip_speed = s.flip_speed;
    next->flip_advanced = s.flip_advanced;
    next->flip_trick = s.flip_trick;
    next->flip_advanced_blocked = s.flip_advanced && extras_blocked();
    next->flip_live = s.flip_live;
    next->flip_gate_found = flip_gates() != 0;
    next->flip_gate_blocked = s.catch_at && extras_blocked();
    next->catch_at = s.catch_at;
    next->catch_percent = s.catch_percent;
    next->boneless_height = s.boneless_height;
    next->revert_boost = s.revert_boost;
    next->pump_power = s.pump_power;
    next->pump_live = s.pump_live;
    next->pump_blocked = s.pump_power != 1.0f && extras_blocked();
    next->revert = s.revert;
    next->return_delay = s.return_delay;
    next->pad_shortcuts = s.pad_shortcuts;
    next->pad_menu = s.pad_menu;
    next->hud = s.hud;
    next->hud_jump = s.hud_jump;
    next->logging = s.logging;
    next->map = s.map;
    next->map_note = s.map_file.note;
    next->map_preset = s.map_file.preset.empty() ? std::string{} : s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    next->spots = s.map_file.spots;
    next->open_serial = s.open_serial;
    next->stock = s.revert == RevertTuning{} && !next->touched && s.active.empty() && std::ranges::none_of(s.entries, &Entry::frozen) &&
                  std::ranges::all_of(trick_names, [](std::string_view name) { return *trick_option(name) == trick_stock(name); }) && !s.catch_at && !s.flip_advanced;
    next->capture_changed = next->touched || s.catch_at || s.flip_advanced || std::ranges::any_of(trick_names, [](std::string_view name) { return *trick_option(name) != trick_stock(name); });
    next->share_text = s.share_text;
    next->share_serial = s.share_serial;
    next->open_tab = s.open_tab;
    if (const auto found = s.maps.find(s.map); found != s.maps.end()) {
        next->markers = found->second.markers;
        next->profile_preset = found->second.preset;
    }
    publish(std::move(next));
}
// ---- sharing presets ---------------------------------------------------------------------------
// One line of text, short enough to paste in a chat: "RST1:" and the preset's JSON in base64.
constexpr std::string_view share_tag = "RST1:";
constexpr std::string_view base64_digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(std::string_view bytes) {
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t chunk{};
        const auto count = std::min<std::size_t>(3, bytes.size() - i);
        for (std::size_t k = 0; k < 3; ++k) chunk = chunk << 8 | (k < count ? static_cast<unsigned char>(bytes[i + k]) : 0u);
        for (std::size_t k = 0; k < 4; ++k) out += k <= count ? base64_digits[chunk >> (18 - 6 * k) & 63] : '=';
    }
    return out;
}
std::optional<std::string> unbase64(std::string_view text) {
    std::string out;
    std::uint32_t chunk{};
    int bits{};
    for (const char c : text) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
        const auto digit = base64_digits.find(c);
        if (digit == std::string_view::npos) return std::nullopt;
        chunk = chunk << 6 | static_cast<std::uint32_t>(digit);
        if ((bits += 6) >= 8) out += static_cast<char>(chunk >> (bits -= 8) & 0xff);
    }
    return out;
}
std::filesystem::path shared_directory() {
    const auto directory = data_directory();
    return directory.empty() ? directory : directory / L"shared";
}
// File names from preset names: letters, digits, space, '-' and '_' only.
std::string file_stem(std::string_view name) {
    std::string stem;
    for (const char c : name) stem += std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' ? c : '_';
    return stem.empty() ? "preset" : stem;
}
// `preset export <name>`: one of the player's presets, or "current" for what is changed right now.
std::string export_preset(const std::string &name) {
    auto &s = state();
    Json values = Json::object();
    std::string title;
    if (lower(name) == "current") {
        for (const auto &e : s.entries)
            if (e.touched) values[e.key] = e.value;
        for (const auto trick : trick_names)
            if (const auto *option = trick_option(trick); *option != trick_stock(trick)) values[std::string(trick_prefix) + std::string(trick)] = *option;
        if (s.flip_advanced) values[std::string(trick_prefix) + std::string(flip_advanced_name)] = 1;
        if (s.catch_at) {
            values[std::string(trick_prefix) + std::string(catch_at_name)] = 1;
            values[std::string(trick_prefix) + std::string(catch_percent_name)] = s.catch_percent;
        }
        title = "My setup";
    } else {
        for (const auto &[user_name, saved] : s.user)
            if (lower(user_name) == lower(name)) {
                for (const auto &[key, value] : saved) values[key] = value;
                title = user_name;
            }
        if (title.empty()) return "error: no preset of yours is called \"" + name + "\". Use \"current\" for what is changed right now.";
    }
    if (values.empty()) return "error: nothing is changed, so there is nothing to share.";
    const auto count = values.size();
    Json json = Json::object();
    json["reskate_trainer"] = 1;
    json["name"] = title;
    json["values"] = std::move(values);
    s.share_text = std::string(share_tag) + base64(json.dump());
    ++s.share_serial;
    std::string where;
    if (const auto directory = shared_directory(); !directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / (file_stem(title) + ".json"), std::ios::binary | std::ios::trunc);
        file << json.dump(2);
        if (file) where = std::format(" Also saved as shared\\{}.json.", file_stem(title));
    }
    changed();
    return std::format("\"{}\" copied to the clipboard as one line ({} values): paste it anywhere.{}", title, count, where);
}
// `preset import [file]`: the text the menu put in shared\clipboard.txt, or a file in shared\.
// Either the one-line form or the JSON itself. The preset joins the player's own; nothing is applied.
std::string import_preset(const std::string &file_name) {
    auto &s = state();
    const auto directory = shared_directory();
    if (directory.empty()) return "error: the trainer's folder could not be found.";
    const auto path = directory / (file_name.empty() ? "clipboard.txt" : file_stem(std::filesystem::path(file_name).stem().string()) + ".json");
    std::string text;
    const auto bounded = storage::read_bounded(path, storage::shared_bytes_limit);
    if (!bounded) return "error: preset file is missing, unreadable or exceeds 2 MiB. Nothing was imported.";
    text = *bounded;
    if (const auto tag = text.find(share_tag); tag != std::string::npos) {
        const auto start = tag + share_tag.size();
        const auto end = text.find_first_of(" \t\r\n`\"'", start);
        const auto decoded = unbase64(std::string_view(text).substr(start, end == std::string::npos ? end : end - start));
        if (!decoded) return "error: that is not a whole preset line: copy all of it, from RST1: to the end.";
        text = *decoded;
    }
    try {
        const auto json = Json::parse(text, {.bytes = storage::shared_bytes_limit});
        if (!json.is_object() || !json.contains("values") || !json.at("values").is_object()) return "error: that is not a ReSkate Trainer preset.";
        const auto decoded_values = storage::decode_values(json.at("values"));
        if (!decoded_values) return "error: preset values are invalid or exceed 8192 entries. Nothing was imported.";
        std::map<std::string, double, std::less<>> values;
        std::size_t unknown{};
        for (const auto &[key, value] : *decoded_values) {
            const auto id = lower(key);
            const bool trick = id.starts_with(trick_prefix) && (trick_option(std::string_view(id).substr(trick_prefix.size())) ||
                                                                trick_switch(std::string_view(id).substr(trick_prefix.size())));
            if (!trick && !find_entry(id)) ++unknown; // kept: another build of the game may have it
            if (!values.emplace(id, value).second)
                return "error: preset contains duplicate value ids with different letter casing. Nothing was imported.";
        }
        if (values.empty()) return "error: that preset holds no values.";
        auto name = json.value("name", "Imported");
        if (name.empty() || name.size() > 40) name = "Imported";
        const auto taken = [&](const std::string &candidate) {
            if (lower(candidate) == "stock" || lower(candidate) == "map" || std::ranges::any_of(s.user, [&](const auto &item) { return lower(item.first) == lower(candidate); })) return true;
            return std::ranges::any_of(builtin_presets(), [&](const auto &preset) { return lower(preset.name) == lower(candidate); });
        };
        auto unique = name;
        for (int n = 2; taken(unique); ++n) unique = std::format("{} {}", name, n);
        const auto count = values.size();
        s.user[unique] = std::move(values);
        changed();
        return std::format("Imported \"{}\" ({} values{}). Turn it on under Your presets.", unique, count,
                           unknown ? std::format(", {} of them unknown to this version", unknown) : "");
    } catch (...) {
        return "error: that is not a ReSkate Trainer preset.";
    }
}
bool flag(const std::string &text, bool &out) {
    const auto value = lower(text);
    if (value == "1" || value == "on" || value == "true") { out = true; return true; }
    if (value == "0" || value == "off" || value == "false") { out = false; return true; }
    return false;
}
std::optional<int> slot_argument(const std::vector<std::string> &arguments, std::size_t index) {
    auto &s = state();
    if (arguments.size() <= index) return s.slot;
    const auto value = number(arguments[index]);
    if (!value || *value < 1 || *value > static_cast<double>(marker_slots) || *value != std::floor(*value)) return std::nullopt;
    return static_cast<int>(*value) - 1;
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string &level) noexcept {
    try {
        auto &s = state();
        const auto now = GetTickCount64();
        if (!s.loaded) load_store();
        s.base = base;
        sync_session();
        s.boosts = boosts_in_force();
        if (const auto key = lower(playing ? level : std::string{}); key != s.map) enter_map(key);
        if (playing) {
            observe(base, client, now);
            shortcuts();
            if (s.return_at && now >= s.return_at) {
                s.return_at = 0;
                const auto &marker = current_map().markers[static_cast<std::size_t>(s.slot)];
                if (marker.set) (void)go_to(marker.position, "your marker");
            }
        } else {
            clear_live_tricks();
            s.telemetry.skater = false;
            s.motion.valid = false;
            s.entity = 0;
        }
        (void)start_trick_heights(base);
        if (take_class_list_shown()) s.class_list_wanted = true;
        // The search reads all of the game's writable memory, so it runs only for a player with
        // a use for it: one of the classes' values or the flip speed is not the game's own
        // (theirs, or a host's they skate with), or they have the list of every value open.
        if (s.class_search_at && now >= s.class_search_at && playing && s.telemetry.skater && !finding_classes() &&
            (s.class_list_wanted || classes_wanted())) {
            // A level brings fresh copies, so only a search finished since it loaded counts. The
            // skater's own classes arrive a little after the skater does: a search that came up
            // empty is repeated, twice.
            const bool have = class_searches() > s.class_search_seen && class_copies(0) != 0;
            if (have || s.class_search_tries >= 3) {
                s.class_search_at = 0;
            } else {
                ++s.class_search_tries;
                s.class_search_at = now + 15000;
                find_classes();
            }
        }
        flip_speed_in_use(s.boosts.flip != 1.0f || s.flip_advanced);
        // The speed itself goes straight to the trick in the air (drive_flip_speed); the flip curves
        // are only scaled where that number cannot be reached.
        want_flip_speed(s.flip_live ? 1.0f : s.boosts.flip);
        // The player's own choice, and only where their own trick settings count: not under a
        // host's enforced physics or in a session that has turned boosts off.
        // A flip asked to be slow is slow from the start: the game's rule would hurry it round before
        // the landing, so it is held off for as long as the speed in force is under the game's own.
        const bool slow = (s.flip_drive.active ? s.flip_drive.factor : s.boosts.flip) < 1.0f;
        want_flip_gate(extras_blocked() ? flip_gate_stock : s.catch_at ? s.catch_seconds : slow ? flip_gate_off : flip_gate_stock);
        if (s.boosts.flip != s.flip_applied) {
            // The catch times follow the flip speed (want_class).
            s.flip_applied = s.boosts.flip;
            for (const auto &e : s.entries)
                if (e.class_field >= 0 && slowed_catch_time(e.key)) want_class(e);
        }
        (void)apply_classes();
        // The game builds its tuning objects again with a new skater (a respawn, a teleport, a
        // session change) and they start at its own numbers: look again, but not in a flurry.
        {
            const bool lost = refresh_classes(now);
            const auto entity = static_cast<std::uint64_t>(s.entity);
            bool new_skater = entity && s.class_entity && entity != s.class_entity;
            if (entity) s.class_entity = entity;
            // A respawn can keep the entity and still fill new copies (the push speeds' native
            // one among them): the old ones go on holding what was written, so nothing looks lost.
            if (const auto creations = skater_creations(); creations != s.class_creations) {
                new_skater = new_skater || s.class_creations != 0;
                s.class_creations = creations;
            }
            // A bail builds a new skater too, and most of those bring nothing new: a search that a
            // new skater asked for and that found only what was known doubles the wait before the
            // next one (a search reads all of the game's writable memory, for seconds).
            if (!s.class_skater_judged && !s.class_search_at && !finding_classes()) {
                s.class_skater_judged = true;
                s.class_skater_wait = class_search_found() == s.class_found ? std::min(s.class_skater_wait * 2, class_skater_wait_high) : class_skater_wait_low;
                s.class_skater_after = now + s.class_skater_wait;
            }
            s.class_research_due = s.class_research_due || lost;
            s.class_skater_due = s.class_skater_due || new_skater;
            if (!classes_wanted()) s.class_research_due = s.class_skater_due = false;
            const bool for_lost = s.class_research_due && now >= s.class_research_after;
            const bool for_skater = s.class_skater_due && now >= s.class_skater_after && now >= s.class_research_after;
            if ((for_lost || for_skater) && !s.class_search_at && !finding_classes()) {
                s.class_research_due = s.class_skater_due = false;
                s.class_search_seen = class_searches();
                s.class_search_tries = 0;
                s.class_search_at = now + 3000;
                s.class_research_after = now + 20000;
                if (!for_lost) {
                    s.class_skater_judged = false;
                    s.class_found = class_search_found();
                    s.class_skater_after = now + s.class_skater_wait;
                }
            }
        }
        set_trick_heights(s.boosts.nocomply, s.boosts.boneless);
        // The speeds pushes aim for are the push class's (value_links ties them to the tuning's top
        // pushing speed, which only gates whether a push may start).
        const auto *top_speed = find_entry("physicspush.maxpushablespeed");
        // Past the game's own ceiling the push is carried by the trainer (see trainer_push_speed):
        // the player's own setting only, like the other speed the trainer adds.
        const float push_factor = top_speed && top_speed->touched && top_speed->stock > 0 && !extras_blocked()
                                      ? std::max(1.0f, static_cast<float>(top_speed->value / top_speed->stock)) : 1.0f;
        set_push_speed(client, s.entity, push_factor, top_speed ? static_cast<float>(top_speed->stock) : 0.0f, s.boosts.cruise);
        // Pumping: the game says when (its pumping state), the trainer sizes the gain on the physics
        // step (trainer_pump_power). The player's own setting only, like the push speed.
        {
            const auto pump = playing && s.telemetry.skater ? read_pump_state(base, static_cast<std::uintptr_t>(s.entity)) : PumpState{};
            if (pump.found != s.pump_live) {
                s.pump_live = pump.found;
                s.view_due = true;
            }
            bool measure = false;
            states_in_use(s.pump_power != 1.0f || measure);
            set_pump_power(client, s.entity, extras_blocked() ? 1.0f : s.pump_power, pump.found && pump.pumping, measure);
        }
        // The revert boost adds speed, so it keeps to the session's rule for boosts and to a host's physics.
        set_revert_boost(client, s.entity, s.enforced || (multiplayer_session_active() && !session_boosts_allowed()) ? 0.0f : s.revert_boost, s.revert);
        if (const auto report = revert_boost_report(); report.sequence != s.revert_boosts_seen) {
            s.revert_boosts_seen = report.sequence;
            // Every landing with some spin in it, boosted or not, so a report can say why not.
            if ((s.hud_jump || s.logging) && report.spin >= 30.0f)
                say(logging::Level::info,
                    std::format("Trainer revert: landed after {} ms with {:.0f} deg of spin, board {} off the body and {:.0f} deg off the travel, board turned {:.0f} deg past the body, state {}, "
                                "{:.1f} m/s: {}{}.",
                                report.air_ms, report.spin, report.board_read ? std::format("{:.0f} deg", report.board_offset) : std::string("unread"), report.slip,
                                report.board_rotation, report.landed_state, report.speed, report.outcome,
                                report.added > 0 ? std::format(" +{:.1f} m/s", report.added) : std::string()));
        }
        for (TrickLaunch launch; take_trick_launch(launch);)
            if (launch.factor != 1.0f)
                say(logging::Level::info, std::format("Trainer trick: {} launched at {:.2f} m/s up, x{:.2f}.",
                                                      launch.trick == Trick::boneless ? "boneless" : "no comply", launch.up_speed, launch.factor));
        hold(now);
        if (s.extras_due) publish_extras();
        run_selftest(now);
        publish(s.telemetry);
        if (s.view_due) build_view();
        if (s.save_due && now >= s.next_save) save_store();
    } catch (...) {}
}

namespace {
// ---- tricklining -------------------------------------------------------------------------------
// trainer revert <field> <number> | reset: what counts as a revert and what it is worth.
std::string revert_command(const std::vector<std::string> &a) {
    auto &s = state();
    const auto name = a.empty() ? std::string{} : lower(a[0]);
    std::string why;
    if (!editable(&why)) return "error: " + why;
    if (name == "reset") {
        s.revert = {};
        changed();
        return "Bends are judged the way they shipped: 90 degrees of spin and the board 40 degrees off the body (the off-the-travel rule is off).";
    }
    const auto found = std::ranges::find(revert_fields, name, &RevertField::name);
    const auto value = a.size() > 1 ? number(a[1]) : std::nullopt;
    if (found == std::end(revert_fields) || !value) return "error: usage: trainer revert spin|slip|twist|bend|full|auto|max_speed|cooldown|min_air <number>, or revert reset";
    s.revert.*found->member = static_cast<float>(*value);
    s.revert = sane_revert(s.revert);
    changed();
    return std::format("Revert {} = {:.4g}", name, s.revert.*found->member);
}
// The values that make a trick line: one command sets them the way Skate 3 had them, or puts the
// game's own back. Skate 3's are its tuning for what this game still shares with it (the "Skate 3"
// preset) and speed back out of reverts; its heavier revert and powerslide friction is a first
// guess (x3 and x1.5), to be tuned by playing.
constexpr std::pair<std::string_view, double> trickline_friction[]{
    {"onboard_powerslide.frictionscalar_revert", 3.0}, {"onboard_powerslide.frictionscalar_autorevert", 3.0}, {"onboard_powerslide.frictionscalar_slide", 1.5}};
// trainer feel stock|easy|normal|hardcore: which game the skating plays like. The three Skate 3
// choices are its difficulty settings (the built-in presets of those names); only one is on.
constexpr std::pair<std::string_view, std::string_view> feels[]{{"easy", "Skate 3 Easy"}, {"normal", "Skate 3"}, {"hardcore", "Skate 3 Hardcore"}};
std::string feel_command(const std::vector<std::string> &a) {
    const auto which = a.empty() ? std::string{} : lower(a[0]);
    std::string why;
    if (!editable(&why)) return "error: " + why;
    const auto found = std::ranges::find(feels, which, [](const auto &feel) { return feel.first; });
    if (found == std::end(feels) && which != "stock" && which != "skate") return "error: usage: trainer feel stock|easy|normal|hardcore";
    // Only a feel that is on is taken off: taking off one that is not would put back values the
    // player set themselves that it happens to share (the push speed dial, for one).
    for (const auto &feel : feels)
        if (std::ranges::find(state().active, feel.second) != state().active.end()) (void)remove_preset(feel.second);
    if (found != std::end(feels)) (void)apply_preset(found->second);
    changed();
    return found == std::end(feels) ? "Plays like skate.: the game's own tuning." : std::format("Plays like {}: its tuning for everything this game shares with it.", found->second);
}
// trainer trickline extras on|off: what Skate 3's trick lines had beyond its tuning. Speed back
// out of reverts, and heavier revert and powerslide friction (x3 and x1.5: a first guess).
std::string trickline_command(const std::vector<std::string> &a) {
    auto &s = state();
    std::string why;
    if (!editable(&why)) return "error: " + why;
    bool on{};
    if (a.size() < 2 || lower(a[0]) != "extras" || !flag(a[1], on)) return "error: usage: trainer trickline extras on|off";
    for (const auto &[key, factor] : trickline_friction)
        if (auto *e = find_entry(key); e && !is_locked(*e)) set_entry(*e, on ? e->stock * factor : e->stock);
    s.revert_boost = on ? std::max(s.revert_boost, 1.0f) : 0.0f;
    changed();
    return on ? "Tricklining extras on: the board bending boost, heavier revert and powerslide friction." : "Tricklining extras off.";
}
// Curated workshop profiles coexist with the historical Skate 3 feel command.
std::string workshop_command(const std::vector<std::string> &arguments, bool assistance = false) {
    auto &s = state();
    std::string why;
    if (!s.ready) return "error: load a level before changing physics.";
    if (!editable(&why)) return "error: " + why;
    if (arguments.size() != 1) return "error: supply one feel name or mixer value.";
    auto value = assistance ? number(arguments[0]) : workshop::amount(arguments[0]);
    if (!value) value = number(arguments[0]);
    if (!value || (assistance ? *value < .1 || *value > 5 : *value < -1 || *value > 1))
        return assistance ? "error: capture assistance must be between 0.1 and 5x stock." : "error: feel must be Hardcore, Authentic, Stock, Accessible, Arcade or -1..1.";
    std::vector<Row> rows;
    rows.reserve(s.entries.size());
    for (const auto &entry : s.entries) {
        Row row; row.id = entry.id; row.value = entry.value; row.stock = entry.stock; row.kind = entry.kind;
        row.used = entry.used; row.detail = entry.detail; row.preset_locked = is_locked(entry);
        rows.push_back(std::move(row));
    }
    auto plan = workshop::preview(rows, *value);
    if (assistance) {
        plan.clear();
        for (const auto &row : rows) {
            const auto key = lower(row.id);
            if (!row.used || row.detail || (key != "physicsmode.grindlockdist" && key != "physicsgrindsair.maxdistboardslide" && key != "physicsgrindsair.maxdisttipslide")) continue;
            plan.push_back({row.id, row.value, static_cast<float>(row.stock * *value), row.preset_locked});
        }
    }
    std::size_t applied{}, locked{};
    for (const auto &change : plan) {
        if (auto *entry = find_entry(change.id)) {
            if (is_locked(*entry)) { ++locked; continue; }
            set_entry(*entry, change.after); ++applied;
        }
    }
    changed();
    return std::format("{}: {} supported values applied, {} locks kept. Other edits stay.", assistance ? "Grind capture assistance" : "Workshop feel", applied, locked);
}
std::string run(std::string_view verb, const std::vector<std::string> &a) {
    auto &s = state();
    const auto v = lower(verb);
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string{}; };
    if (v == "status") {
        std::size_t touched{}, frozen{};
        for (const auto &e : s.entries) { touched += e.touched; frozen += e.frozen; }
        return std::format("{} {} values ({} changed, {} frozen). Map: {}. Presets: {}.", s.status, s.entries.size(), touched, frozen,
                           s.map.empty() ? "none" : s.map, s.active.empty() ? "stock" : std::to_string(s.active.size()) + " applied");
    }
    if (v == "set" || v == "freeze" || v == "reset") {
        if (v == "reset" && lower(arg(0)) == "all") {
            const auto count = reset_all();
            std::size_t locked{};
            for (const auto &e : s.entries) locked += e.touched;
            changed();
            return locked ? std::format("{} values put back; {} locked values kept (Reset everything clears those too).", count, locked)
                          : std::format("{} values put back.", count);
        }
        if (v == "reset" && lower(arg(0)) == "tricks") {
            s.hippy_height = s.nocomply_height = s.boneless_height = s.offboard_height = s.flip_speed = s.pump_power = 1.0f;
            s.catch_at = false;
            s.catch_percent = trick_stock(catch_percent_name);
            s.catch_seconds = flip_gate_stock;
            s.flip_advanced = false;
            s.flip_trick.fill(1.0f);
            s.revert_boost = 0.0f;
            changed();
            return "Trick heights and flip trick speed are the game's own again.";
        }
        if (v == "reset" && lower(arg(0)) == "presets") {
            const auto on = s.active;
            for (const auto &name : on) (void)remove_preset(name);
            s.active.clear();
            changed();
            return std::format("{} presets switched off.", on.size());
        }
        if (v == "reset" && lower(arg(0)) == "everything") {
            // The game as it shipped: no lock survives, no preset stays on, no trick multiplier.
            std::size_t count{};
            for (auto &e : s.entries) e.frozen = false;
            for (auto &e : s.entries) {
                if (e.touched) {
                    set_entry(e, e.stock);
                    ++count;
                } else {
                    remember(e);
                }
            }
            s.saved.clear();
            s.active.clear();
            s.hippy_height = s.nocomply_height = s.boneless_height = s.offboard_height = s.flip_speed = s.pump_power = 1.0f;
            s.catch_at = false;
            s.catch_percent = trick_stock(catch_percent_name);
            s.catch_seconds = flip_gate_stock;
            s.flip_advanced = false;
            s.flip_trick.fill(1.0f);
            s.revert_boost = 0.0f;
            s.revert = {};
            changed();
            return std::format("Everything is the game's own again: {} values put back, locks cleared, presets off, trick sliders at 1.", count);
        }
        auto *e = find_entry(arg(0));
        if (!e) return "error: no tuning value is called \"" + arg(0) + "\". Try: trainer find <text>";
        std::string why;
        if (v != "freeze" && !editable(&why)) return "error: " + why;
        if (v == "set") {
            const auto value = number(arg(1));
            if (!value) return "error: " + e->id + " needs a number.";
            set_entry(*e, *value);
        } else if (v == "freeze") {
            if (!flag(arg(1), e->frozen)) return "error: freeze needs 0 or 1.";
            remember(*e);
        } else {
            set_entry(*e, e->stock);
        }
        changed();
        return std::format("{} = {:.6g}{}{}", e->id, e->value, e->touched ? std::format(" (stock {:.6g})", e->stock) : "", e->frozen ? ", frozen" : "");
    }
    if (v == "find") {
        const auto pattern = lower(arg(0));
        std::string result;
        std::size_t count{};
        for (const auto &e : s.entries)
            if (matches(e.key, pattern) && ++count <= 12) result += std::format("{}{} = {:.6g}", result.empty() ? "" : "; ", e.id, e.value);
        return count ? std::format("{} match: {}{}", count, result, count > 12 ? "; ..." : "") : "Nothing matches.";
    }
    if (v == "dial") {
        // trainer dial <multiplier> <built-in preset>
        const auto factor = number(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (!factor || name.empty()) return "error: usage: trainer dial <multiplier> <preset name>";
        std::string why;
        if (!editable(&why)) return "error: " + why;
        return apply_dial(name, *factor);
    }
    if (v == "preset") {
        const auto action = lower(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (action == "import") return import_preset(name);
        if (name.empty()) return "error: usage: trainer preset apply|remove|save|delete|export <name>, or preset import [file]";
        if (action == "export") return export_preset(name);
        if (action == "remove") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return remove_preset(name);
        }
        if (action == "apply") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return apply_preset(name);
        }
        if (action == "save") {
            if (name.size() > 48) return "error: that name is too long.";
            for (const auto &preset : builtin_presets())
                if (lower(preset.name) == lower(name)) return "error: that name belongs to a built-in preset.";
            if (lower(name) == "stock" || lower(name) == "map") return "error: that name is reserved.";
            std::map<std::string, double, std::less<>> values;
            for (const auto &e : s.entries)
                if (e.touched) values[e.key] = e.value;
            for (const auto trick : trick_names)
                if (const auto *option = trick_option(trick); *option != trick_stock(trick)) values[std::string(trick_prefix) + std::string(trick)] = *option;
            if (s.flip_advanced) values[std::string(trick_prefix) + std::string(flip_advanced_name)] = 1;
            if (s.catch_at) {
                values[std::string(trick_prefix) + std::string(catch_at_name)] = 1;
                values[std::string(trick_prefix) + std::string(catch_percent_name)] = s.catch_percent;
            }
            if (values.empty()) return "error: nothing is changed, so there is nothing to save.";
            const auto count = values.size();
            // Commands resolve names without case; saving must replace that same entry.
            const auto existing = std::ranges::find_if(s.user, [&](const auto &item) { return lower(item.first) == lower(name); });
            if (existing != s.user.end()) name = existing->first;
            s.user[name] = std::move(values);
            changed();
            return std::format("Saved \"{}\" ({} values).", name, count);
        }
        if (action == "delete") {
            if (!s.user.erase(name)) return "error: no preset of yours is called \"" + name + "\".";
            std::erase(s.active, name);
            changed();
            return "Deleted \"" + name + "\".";
        }
        return "error: usage: trainer preset apply|save|delete <name>";
    }
    if (v == "revert") return revert_command(a);
    if (v == "trickline") return trickline_command(a);
    if (v == "feel") return feel_command(a);
    if (v == "workshop") return workshop_command(a);
    if (v == "assist") return workshop_command(a, true);
    if (v == "slot") {
        const auto slot = slot_argument(a, 0);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        s.slot = *slot;
        changed();
        return std::format("Marker slot {}{}.", s.slot + 1, current_map().markers[static_cast<std::size_t>(s.slot)].set ? "" : " (empty)");
    }
    if (v == "marker") {
        const auto action = lower(arg(0));
        const auto slot = slot_argument(a, 1);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        if (s.map.empty()) return "error: load a level first.";
        auto &marker = current_map().markers[static_cast<std::size_t>(*slot)];
        if (action == "save") {
            if (!s.telemetry.skater) return "error: no skater to mark.";
            marker = {true, s.telemetry.position};
            changed();
            return std::format("Marker {} saved at ({:.1f}, {:.1f}, {:.1f}).", *slot + 1, marker.position[0], marker.position[1], marker.position[2]);
        }
        if (action == "go") {
            if (!marker.set) return std::format("error: marker {} is empty.", *slot + 1);
            return go_to(marker.position, std::format("marker {}", *slot + 1));
        }
        if (action == "clear") {
            marker = {};
            changed();
            return std::format("Marker {} cleared.", *slot + 1);
        }
        return "error: usage: trainer marker save|go|clear [slot]";
    }
    if (v == "tp") {
        const auto x = number(arg(0)), y = number(arg(1)), z = number(arg(2));
        if (!x || !y || !z) return "error: usage: trainer tp <x> <y> <z>";
        return go_to({static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)}, "that point");
    }
    if (v == "spot") {
        const auto index = number(arg(0));
        if (!index || *index < 1 || *index > static_cast<double>(s.map_file.spots.size())) return "error: this map has no such spot.";
        const auto &spot = s.map_file.spots[static_cast<std::size_t>(*index) - 1];
        return go_to(spot.position, spot.name);
    }
    if (v == "option") {
        const auto name = lower(arg(0));
        bool on{};
        std::string why;
        if (name == "flip_speed" || name.starts_with("flip.")) {
            const auto value = number(arg(1));
            auto *option = trick_option(name);
            if (!option) return "error: no such flip trick. flip_speed is all of them; flip.kickflip, flip.360flip ... are one each.";
            if (!value) return "error: " + name + " needs a multiplier, 1 = the game's own speed.";
            if (!editable(&why)) return "error: " + why;
            *option = std::clamp(static_cast<float>(*value), flip_low, flip_high);
        } else if (name == "hippy_height" || name == "nocomply_height" || name == "boneless_height" || name == "offboard_height") {
            const auto value = number(arg(1));
            if (!value) return "error: " + name + " needs a multiplier, 1 = the game's own height.";
            if (!editable(&why)) return "error: " + why;
            (name == "hippy_height" ? s.hippy_height : name == "nocomply_height" ? s.nocomply_height : name == "offboard_height" ? s.offboard_height : s.boneless_height) =
                std::clamp(static_cast<float>(*value), height_low, height_high);
        } else if (name == "pump_power") {
            const auto value = number(arg(1));
            if (!value) return "error: pump_power needs a multiplier, 1 = what the game's own pump gains.";
            if (!editable(&why)) return "error: " + why;
            s.pump_power = std::clamp(static_cast<float>(*value), 0.0f, pump_high);
        } else if (name == "revert_boost") {
            const auto value = number(arg(1));
            if (!value) return "error: revert_boost needs a strength: 0 is off, 1 gives back about 2 m/s on an auto revert.";
            if (!editable(&why)) return "error: " + why;
            s.revert_boost = std::clamp(static_cast<float>(*value), 0.0f, revert_boost_high);
        } else if (name == "catch_percent") {
            const auto value = number(arg(1));
            if (!value) return "error: catch_percent needs a percent of the air time.";
            if (!editable(&why)) return "error: " + why;
            s.catch_percent = std::clamp(static_cast<float>(*value), catch_low, catch_high);
        } else if (name == "return_delay") {
            const auto value = number(arg(1));
            if (!value) return "error: return_delay needs seconds.";
            s.return_delay = std::clamp(static_cast<float>(*value), 0.0f, 10.0f);
        } else if (!flag(arg(1), on)) {
            return "error: usage: trainer option hud|hud_jump|auto_return|pad|pad_menu|flip_advanced|catch_at|log 0|1";
        } else if (name == "hud") s.hud = on;
        else if (name == "hud_jump") s.hud_jump = on;
        else if (name == "auto_return") s.auto_return = on;
        else if (name == "pad") s.pad_shortcuts = on;
        else if (name == "pad_menu") s.pad_menu = on;
        else if (name == "flip_gate") return "That switch is gone: a flip set slower than the game's own stays slow by itself.";
        else if (name == "catch_at") {
            if (on && !editable(&why)) return "error: " + why;
            s.catch_at = on;
        } else if (name == "flip_advanced") {
            if (on && !editable(&why)) return "error: " + why;
            s.flip_advanced = on;
        }
        else if (name == "log") {
            set_logging(on);
            if (on && !s.logging) return "error: the telemetry file could not be created.";
        } else return "error: no option is called \"" + name + "\".";
        changed();
        return name + " = " + arg(1);
    }
    if (v == "profile") {
        const auto action = lower(arg(0));
        if (s.map.empty()) return "error: load a level first.";
        if (action == "clear") {
            current_map().preset.clear();
            changed();
            return "This map no longer applies a preset on load.";
        }
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (action != "set" || name.empty()) return "error: usage: trainer profile set <preset> | clear";
        current_map().preset = name;
        changed();
        return "\"" + name + "\" will be applied whenever this map loads.";
    }
    if (v == "open") {
        const auto tab = lower(arg(0));
        // The last three are the Tune tab on one of its lists.
        const std::array<std::string_view, 11> tabs{"tune", "presets", "practice", "map", "realistic", "fun", "everything", "camera", "trickline", "feel", "settings"};
        const auto found = std::ranges::find(tabs, tab);
        if (!tab.empty() && found == tabs.end()) return "error: usage: trainer open [feel|tune|trickline|practice|map|realistic|fun|everything|settings|presets]";
        s.open_tab = tab.empty() ? 9 : static_cast<int>(found - tabs.begin());
        ++s.open_serial;
        s.view_due = true;
        return "Opening the trainer.";
    }
    if (v == "jumps") {
        const auto &j = s.telemetry.last;
        if (!j.serial) return "No jump measured yet.";
        return std::format("Jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, landing {:.1f} km/h.",
                           j.serial, j.takeoff_speed * 3.6f, j.takeoff_angle, j.air_time, j.height, j.distance, j.drop, j.landing_speed * 3.6f);
    }
    if (v == "where") {
        if (!s.telemetry.skater) return "error: no skater.";
        const auto &p = s.telemetry.position;
        return std::format("({:.2f}, {:.2f}, {:.2f}) heading {:.0f}, {:.1f} km/h. Blender: ({:.2f}, {:.2f}, {:.2f})", p[0], p[1], p[2],
                           s.telemetry.heading, s.telemetry.speed * 3.6f, p[0], -p[2], p[1]);
    }
    if (v == "classes") {
        if (lower(arg(0)) == "find") {
            find_classes();
            return "Looking for the game's tuning classes.";
        }
        return "Copies found: " + classes_summary();
    }
    if (v == "refresh") return tuning::refresh_skater(s.base, s.entity) ? "Skater copy refreshed." : "error: the skater has no cached copy right now.";
    if (v == "states") {
        std::string result;
        for (const auto &[id, ticks] : s.states) result += std::format("{}{}: {}", result.empty() ? "" : ", ", id, ticks);
        return result.empty() ? "No physics state seen yet." : "Physics states seen (id: ticks): " + result;
    }
    if (v == "dump") {
        const auto directory = data_directory();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / L"tuning-values.txt", std::ios::binary | std::ios::trunc);
        if (!file) return "error: could not write the list.";
        for (const auto &e : s.entries)
            file << std::format("{}\t0x{:x}\t{}\t{:.6g}\t{:.6g}\n", e.id, e.offset,
                                e.kind == Kind::real ? "real" : e.kind == Kind::flag ? "flag" : e.kind == Kind::curve ? "curve" :
                                e.kind == Kind::graph ? "graph" : "int", e.stock, e.value);
        return std::format("Wrote {} values to {}.", s.entries.size(), path_utf8(directory / L"tuning-values.txt"));
    }
    if (v == "selftest") {
        if (s.test.step) return "The self test is already running.";
        s.test = {};
        s.test.step = 1;
        s.test.until = GetTickCount64() + 500;
        return "Self test started; see the log for \"trainer selftest:\" lines.";
    }
    return "error: trainer status|open|set|freeze|reset|find|preset|slot|marker|tp|spot|option|profile|jumps|where|states|dump|selftest";
}
} // namespace

bool stage_import(std::string_view text) noexcept {
    try {
        const auto directory = shared_directory();
        if (directory.empty() || text.empty() || text.size() > storage::shared_bytes_limit) return false;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / "clipboard.txt", std::ios::binary | std::ios::trunc);
        file << text;
        return static_cast<bool>(file);
    } catch (...) {
        return false;
    }
}
std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto result = run(verb, arguments);
    // The menu shows the last answer, except for the ones a dragged slider sends every frame.
    if (verb != "set" || result.starts_with("error")) {
        auto &s = state();
        s.last = result;
        s.view_due = true;
    }
    return result;
}
} // namespace dingosdk::trainer
