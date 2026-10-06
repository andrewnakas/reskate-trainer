#include "face_scan.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "qrcodegen.hpp"
#include <Windows.h>
#include <ShlObj.h>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>

namespace dingosdk::face_scan {
namespace {
constexpr auto poll_every = std::chrono::seconds(3);
constexpr auto give_up_after = std::chrono::minutes(10);
constexpr std::uint64_t max_meta_bytes = 64 * 1024, max_texture_bytes = 8ULL * 1024 * 1024;
constexpr const wchar_t *agent = L"ReSkate-FaceScan/1";

struct State {
    std::mutex mutex; // the worker writes the pairing fields; everything else is the game thread's
    std::uintptr_t base{}, client{};
    bool playing{};
    View view;
    std::shared_ptr<std::atomic<bool>> cancel; // the running worker's
    std::atomic<bool> received{};              // the worker has a new scan for the game thread
    bool loaded{};
};
State &state() {
    static auto *s = new State; // the worker may outlive static destruction at exit
    return *s;
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::customization, "Face scan: " + text); }

// Under the lock.
void republish(State &s) {
    ++s.view.revision;
    publish(std::make_shared<const View>(s.view));
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}
bool write_file(const std::filesystem::path &path, std::string_view text) {
    const auto temporary = std::filesystem::path(path).concat(L".part");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size()))) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

// 10 characters of the site's newId alphabet: about 50 bits, from the OS's generator.
std::string new_code() {
    constexpr std::string_view alphabet = "abcdefghijkmnopqrstuvwxyz23456789";
    std::random_device random;
    std::string code;
    while (code.size() < 10) {
        const auto value = random() & 0xff;
        if (value < 33u * 7u) code += alphabet[value % 33]; // no modulo bias
    }
    return code;
}

std::string text_field(const Json &json, std::string_view key) {
    return json.contains(key) && json.at(key).is_string() ? json.at(key).string() : std::string{};
}
bool valid_hex(std::string_view value) {
    return value.size() == 7 && value[0] == '#' &&
           value.substr(1).find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
}
bool valid_word(std::string_view value) {
    return value.size() <= 32 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string_view::npos;
}
// The site's meta (apps/api/src/face.ts, FaceMeta), checked again here: it decides what the game does.
std::optional<Scan> parse_scan(const std::string &text) {
    try {
        const auto json = Json::parse(text);
        Scan scan{text_field(json, "skin"), text_field(json, "hair"), text_field(json, "hairStyle"), text_field(json, "facialHair")};
        scan.face = json.contains("texture") && json.at("texture").is_object();
        if (!valid_hex(scan.skin) || !valid_hex(scan.hair) || !valid_word(scan.hair_style) || !valid_word(scan.facial_hair))
            return std::nullopt;
        return scan;
    } catch (...) {
        return std::nullopt;
    }
}
std::string scan_json(const Scan &scan, bool enabled) {
    auto json = Json::object();
    json["skin"] = scan.skin;
    json["hair"] = scan.hair;
    json["hairStyle"] = scan.hair_style;
    json["facialHair"] = scan.facial_hair;
    json["face"] = scan.face;
    json["enabled"] = enabled;
    return json.dump(2);
}

void load_saved(State &s) {
    s.loaded = true;
    const auto path = data_directory() / L"scan.json";
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return;
    const auto text = read_file(path);
    if (auto scan = parse_scan(text)) {
        try {
            const auto json = Json::parse(text);
            scan->face = json.contains("face") && json.at("face").is_boolean() && json.at("face").get<bool>() &&
                         std::filesystem::exists(data_directory() / L"face.png", error);
            s.view.enabled = !json.contains("enabled") || !json.at("enabled").is_boolean() || json.at("enabled").get<bool>();
        } catch (...) {}
        s.view.saved = true;
        s.view.scan = *scan;
    }
}

// The worker: polls until the phone has posted, then picks the scan up. It owns nothing but
// its copies; the state it reports goes through the lock, and only while not cancelled.
void worker(std::string code, std::shared_ptr<std::atomic<bool>> cancel) {
    const auto update = [&](View::Phase phase, std::string status) {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        if (*cancel) return false;
        s.view.phase = phase;
        s.view.status = std::move(status);
        republish(s);
        return true;
    };
    const auto api = std::wstring(site.begin(), site.end()) + L"/api/face/" + std::wstring(code.begin(), code.end());
    const auto until = std::chrono::steady_clock::now() + give_up_after;
    std::optional<std::string> meta;
    unsigned failures{};
    while (!*cancel && std::chrono::steady_clock::now() < until) {
        https::Download result;
        meta = https::get_text(api, max_meta_bytes, 10, agent, &result);
        if (meta) break;
        // 404 is "not scanned yet". Anything else is the network: say so, keep trying.
        if (result.http_status != 404 && ++failures % 5 == 0)
            update(View::Phase::waiting, std::format("Can't reach skatemods.com (HTTP {}, error {}). Still trying.", result.http_status, result.error));
        for (auto slept = std::chrono::milliseconds(0); slept < poll_every && !*cancel; slept += std::chrono::milliseconds(250))
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (*cancel) return;
    if (!meta) { update(View::Phase::failed, "No scan arrived within 10 minutes. Start a new one."); return; }
    const auto scan = parse_scan(*meta);
    if (!scan) { update(View::Phase::failed, "The scan that arrived could not be read."); return; }
    if (!update(View::Phase::downloading, "Scan received. Downloading...")) return;

    const auto directory = data_directory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto face = directory / L"face.png", part = directory / L"face.png.part";
    bool got_face{};
    if (scan->face) {
        std::filesystem::remove(part, error);
        const auto download = https::get(api + L"/texture", part, max_texture_bytes, 30, agent);
        got_face = download.ok;
        if (got_face) std::filesystem::rename(part, face, error), got_face = !error;
        if (!got_face) say(logging::Level::warning, std::format("face texture download failed (HTTP {}, error {})", download.http_status, download.error));
    } else {
        https::get_text(api + L"/done", 1024, 10, agent); // picked up: the site deletes it
    }
    if (*cancel) return;
    auto kept = *scan;
    kept.face = got_face;
    if (!got_face) std::filesystem::remove(face, error);
    if (!write_file(directory / L"scan.json", scan_json(kept, true))) {
        update(View::Phase::failed, "The scan could not be saved in " + directory.string());
        return;
    }
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (*cancel) return;
    s.view.phase = View::Phase::received;
    s.view.code.clear(), s.view.url.clear(), s.view.qr.clear(), s.view.qr_size = 0;
    s.view.saved = true;
    s.view.scan = kept;
    s.view.enabled = true;
    s.view.status = scan->face && !got_face ? "Colours and hair received; the face texture did not download." : "Scan received.";
    s.received = true;
    republish(s);
    say(logging::Level::info, std::format("received skin {} hair {} style '{}' facial hair '{}' face {}", kept.skin, kept.hair,
                                          kept.hair_style, kept.facial_hair, kept.face));
}

std::string start(State &s) {
    std::lock_guard lock(s.mutex);
    if (s.cancel) *s.cancel = true;
    const auto code = new_code();
    const auto url = std::string(site) + "/face/?c=" + code;
    const auto qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    s.view.phase = View::Phase::waiting;
    s.view.code = code;
    s.view.url = url;
    s.view.qr_size = qr.getSize();
    s.view.qr.assign(static_cast<std::size_t>(qr.getSize() * qr.getSize()), 0);
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x) s.view.qr[static_cast<std::size_t>(y * qr.getSize() + x)] = qr.getModule(x, y);
    s.view.status = "Scan the code with your phone's camera.";
    s.cancel = std::make_shared<std::atomic<bool>>(false);
    republish(s);
    std::thread(worker, code, s.cancel).detach();
    return "Showing a QR code for " + url;
}

// What applying the kept scan to the skater does. Finding the skater's skin, hair and face
// parameters is the job of `face dump`; until those are known this says what will change.
std::string apply(State &s) {
    if (!s.view.saved) return "No scan saved yet.";
    if (!s.view.enabled) return "Off: the skater shows their own look.";
    return std::format("Skin {}, hair {}{}{}{}: waiting for the skater's skin and hair parameters (run face dump and send the file).",
                       s.view.scan.skin, s.view.scan.hair,
                       s.view.scan.hair_style.empty() ? "" : ", " + s.view.scan.hair_style,
                       s.view.scan.facial_hair.empty() ? "" : ", " + s.view.scan.facial_hair,
                       s.view.scan.face ? ", face texture" : "");
}
} // namespace

std::filesystem::path data_directory() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"face";
    if (raw) CoTaskMemFree(raw);
    return result;
}

void tick(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept {
    auto &s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (!s.loaded) { load_saved(s); s.view.applied = apply(s); republish(s); }
        const bool entered = playing && !s.playing;
        s.base = base, s.client = client, s.playing = playing;
        if (s.received.exchange(false) || entered) {
            s.view.applied = apply(s);
            republish(s);
        }
    } catch (...) {}
}

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto &s = state();
    if (verb == "dump") {
        bool playing{};
        std::uintptr_t base{}, client{};
        {
            std::lock_guard lock(s.mutex);
            playing = s.playing, base = s.base, client = s.client;
        }
        if (!playing) return "error: load into the world with your skater first.";
        try {
            return dump(base, client, !arguments.empty() && arguments[0] == "raw");
        } catch (const std::exception &error) {
            return std::string("error: ") + error.what();
        }
    }
    if (verb == "scan") return start(s);
    std::lock_guard lock(s.mutex);
    if (verb == "cancel") {
        if (s.cancel) *s.cancel = true;
        s.cancel.reset();
        s.view.phase = View::Phase::idle;
        s.view.code.clear(), s.view.url.clear(), s.view.qr.clear(), s.view.qr_size = 0;
        s.view.status = "Cancelled.";
        republish(s);
        return "Face scan cancelled.";
    }
    if (verb == "on" || verb == "off") {
        if (!s.view.saved) return "error: no scan saved yet. Use face scan first.";
        s.view.enabled = verb == "on";
        write_file(data_directory() / L"scan.json", scan_json(s.view.scan, s.view.enabled));
        s.view.applied = apply(s);
        republish(s);
        return s.view.applied;
    }
    if (verb == "forget") {
        std::error_code error;
        std::filesystem::remove(data_directory() / L"scan.json", error);
        std::filesystem::remove(data_directory() / L"face.png", error);
        s.view.saved = false;
        s.view.scan = {};
        s.view.applied = "Saved scan deleted.";
        republish(s);
        return "Deleted the saved scan from " + data_directory().string();
    }
    if (verb == "status") {
        const auto phase = s.view.phase == View::Phase::waiting ? "waiting for the phone (" + s.view.code + ")" :
                           s.view.phase == View::Phase::downloading ? std::string("downloading") : s.view.status;
        return std::format("{}{}. {}", phase.empty() ? "Idle" : phase,
                           s.view.saved ? std::format("; saved scan: skin {} hair {}", s.view.scan.skin, s.view.scan.hair) : "",
                           s.view.applied);
    }
    return "error: face scan|cancel|on|off|forget|status|dump [raw]";
}
} // namespace dingosdk::face_scan
