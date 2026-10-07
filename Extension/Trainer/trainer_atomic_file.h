#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace dingosdk::trainer::storage {
struct PreservedProfile { std::filesystem::path backup; std::error_code error; };
// Rename only a regular profile into a unique adjacent backup. Never replace a
// backup or delete the original bytes. Refuse directory/reparse attributes before rename.
inline PreservedProfile preserve_unreadable(const std::filesystem::path &target) {
    const auto attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        return {{}, {static_cast<int>(error), std::system_category()}};
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return {{}, {ERROR_DIRECTORY, std::system_category()}};
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return {{}, {ERROR_NOT_SUPPORTED, std::system_category()}};
    static std::atomic<unsigned long> sequence{};
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto backup = std::filesystem::path(target.wstring() + L".recover-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".json");
        if (MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) return {std::move(backup), {}};
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return {{}, {static_cast<int>(error), std::system_category()}};
    }
    return {{}, {ERROR_FILE_EXISTS, std::system_category()}};
}
// Create a new private temporary file beside the target. Never truncate a predictable
// .tmp path (which could already be a link). Flush and close before replacing the target.
// An error leaves the previous target intact and removes only our own temporary file.
inline std::error_code write_atomic(const std::filesystem::path &target, std::string_view bytes) {
    static std::atomic<unsigned long> sequence{};
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8; ++attempt) {
        temporary = target.wstring() + L".write-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".tmp";
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return {static_cast<int>(error), std::system_category()};
    }
    if (file == INVALID_HANDLE_VALUE) return {ERROR_FILE_EXISTS, std::system_category()};
    DWORD error = ERROR_SUCCESS;
    for (std::size_t done = 0; done < bytes.size();) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1024 * 1024));
        DWORD written{};
        if (!WriteFile(file, bytes.data() + done, chunk, &written, nullptr)) { error = GetLastError(); break; }
        if (!written) { error = ERROR_WRITE_FAULT; break; }
        done += written;
    }
    if (error == ERROR_SUCCESS && !FlushFileBuffers(file)) error = GetLastError();
    if (!CloseHandle(file) && error == ERROR_SUCCESS) error = GetLastError();
    if (error == ERROR_SUCCESS && !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) error = GetLastError();
    if (error != ERROR_SUCCESS) DeleteFileW(temporary.c_str());
    return {static_cast<int>(error), std::system_category()};
}
}
