#pragma once
// Face scan: a phone scan (skatemods.com/face/) turned into the local skater's skin tone,
// hair and face.
//
// The game thread owns this state. Every change is a `face ...` console command, so the menu
// page only queues commands and reads the snapshot below.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::face_scan {
// Verified client update thread only (Runtime/client_tick.cpp).
void tick(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept;
// Console verbs (face_commands.cpp); runs on the game update thread.
std::string command(std::string_view verb, const std::vector<std::string> &arguments);
// %LOCALAPPDATA%\ReSkate\face
std::filesystem::path data_directory();

// Discovery (face_dump.cpp): everything the skater wears and its materials' parameters, written
// under data_directory()/dump. Returns a one-line summary or "error: ...".
std::string dump(std::uintptr_t base, std::uintptr_t client, bool raw);
} // namespace dingosdk::face_scan
