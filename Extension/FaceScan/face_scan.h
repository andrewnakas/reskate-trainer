#pragma once
// Face scan: a phone scan (skatemods.com/face/) turned into the local skater's skin tone,
// hair and face.
//
// The game shows a QR code for skatemods.com/face/?c=CODE with a code it made. The phone page
// works out a skin colour, a hair colour, a hairstyle, facial hair and a face texture on the
// device and posts them under that code; a worker thread here polls skatemods.com/api/face/CODE,
// downloads the result once (which deletes it there) and keeps it in %LOCALAPPDATA%\ReSkate\face.
//
// The game thread owns the state. Every change is a `face ...` console command, so the menu
// page (presentation thread) only queues commands and reads the snapshot below.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::face_scan {
inline constexpr std::string_view site = "https://skatemods.com";

// What the phone sent, as kept in scan.json.
struct Scan {
    std::string skin, hair;            // "#rrggbb", sRGB
    std::string hair_style, facial_hair; // the page's categories ("short", "beard", ...); empty: keep
    bool face{};                       // face.png came with it
};

// Changes rarely: rebuilt when a command or the worker changes something.
struct View {
    std::uint64_t revision{};
    enum class Phase : std::uint8_t { idle, waiting, downloading, received, failed } phase{};
    std::string code, url;  // while waiting: what the QR code says
    int qr_size{};          // modules a side; qr_size * qr_size entries, 1 = dark
    std::vector<std::uint8_t> qr;
    std::string status;     // one line for the page
    bool saved{};           // a scan is kept on disk
    Scan scan;              // the kept scan
    bool enabled{};         // apply the kept scan to the skater
    std::string applied;    // what applying it did, or why it could not
};

// Verified client update thread only (Runtime/client_tick.cpp).
void tick(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept;
// Console verbs (face_commands.cpp); runs on the game update thread.
std::string command(std::string_view verb, const std::vector<std::string> &arguments);
// %LOCALAPPDATA%\ReSkate\face
std::filesystem::path data_directory();

// The menu's snapshot (face_view.cpp: linked into the overlay without the game-facing half).
std::shared_ptr<const View> view() noexcept;
void publish(std::shared_ptr<const View> next) noexcept;

// Discovery (face_dump.cpp): everything the skater wears and its materials' parameters, written
// under data_directory()/dump. Returns a one-line summary or "error: ...".
std::string dump(std::uintptr_t base, std::uintptr_t client, bool raw);
} // namespace dingosdk::face_scan
