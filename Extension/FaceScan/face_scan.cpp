#include "face_scan.h"
#include <Windows.h>
#include <ShlObj.h>

namespace dingosdk::face_scan {
namespace {
struct State {
    std::uintptr_t base{}, client{};
    bool playing{};
};
State &state() {
    static State s;
    return s;
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
    s.base = base, s.client = client, s.playing = playing;
}

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto &s = state();
    if (verb == "dump") {
        if (!s.playing) return "error: load into the world with your skater first.";
        try {
            return dump(s.base, s.client, !arguments.empty() && arguments[0] == "raw");
        } catch (const std::exception &error) {
            return std::string("error: ") + error.what();
        }
    }
    if (verb == "status") return s.playing ? "In the world: face dump is available." : "Not in the world yet.";
    return "error: face dump [raw]|status";
}
} // namespace dingosdk::face_scan
