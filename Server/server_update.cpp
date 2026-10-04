#include "server_update.h"
#include "Engine/Core/Platform/launcher_support.h"
#ifdef _WIN32
#include "Launcher/updater.h"
#include <Windows.h>
#else
#include <unistd.h>
#endif
#include <mutex>
#include <optional>
#include <stdexcept>

namespace dingosdk::server {
#ifdef _WIN32
namespace {
std::mutex release_mutex;
std::optional<launcher_update::Config> release; // from the last check

std::filesystem::path self() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("Cannot find ReSkateServer.exe");
    path.resize(length);
    return path;
}
} // namespace

bool updates_enabled() noexcept { return launcher_update::binary_updates_enabled(); }

UpdateCheck check_for_update() {
    UpdateCheck check;
    try {
        auto config = launcher_update::fetch_config();
        if (!config) {
            check.problem = "GitHub could not be reached";
            return check;
        }
        if (config->server.url.empty()) {
            check.problem = "the latest release has no server";
            return check;
        }
        check.version = config->server.version;
        check.available = launcher::sha256_file(self()) != config->server_exe_sha256;
        std::lock_guard lock(release_mutex);
        release = std::move(config);
    } catch (const std::exception &e) {
        check.problem = e.what();
    }
    return check;
}

void install_update(const std::filesystem::path &folder) {
    launcher_update::RemoteFile file;
    std::string exe_sha256;
    {
        std::lock_guard lock(release_mutex);
        if (!release || release->server.url.empty()) throw std::runtime_error("no update has been found");
        file = release->server;
        exe_sha256 = release->server_exe_sha256;
    }
    const auto archive = launcher_update::download_verified(folder / L"ReSkateServer-update.zip", file);
    struct Remove {
        std::filesystem::path path;
        ~Remove() { DeleteFileW(path.c_str()); }
    } remove{archive};
    // Otherwise the new copy would find itself out of date and update forever.
    if (launcher_update::archive_entry_sha256(archive, "ReSkateServer.exe") != exe_sha256)
        throw std::runtime_error("the release's ReSkateServer.exe doesn't match launcher.json");
    launcher_update::install_archive(archive, folder);
}

bool relaunch() {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = GetCommandLineW();
    const auto started = CreateProcessW(self().c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
    if (!started) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void remove_previous_update(const std::filesystem::path &folder) noexcept {
    launcher_update::remove_replaced_files(folder);
    DeleteFileW((folder / L"ReSkateServer-update.zip.new").c_str());
}
#else // _WIN32

// Linux V1: self-update is disabled. Operators update by replacing the binary
// (package manager / re-download). This keeps the server free of WinHTTP and
// Windows file-replacement semantics. A curl-based updater can be added later.
bool updates_enabled() noexcept { return false; }

UpdateCheck check_for_update() {
    UpdateCheck check;
    check.problem = "self-update is not supported on Linux; update by replacing the binary";
    return check;
}

void install_update(const std::filesystem::path &) {
    throw std::runtime_error("self-update is not supported on Linux");
}

bool relaunch() { return false; }

void remove_previous_update(const std::filesystem::path &) noexcept {}

#endif // _WIN32
} // namespace dingosdk::server
