#include "Extension/Trainer/trainer_atomic_file.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using dingosdk::trainer::storage::write_atomic;
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
std::string read(const std::filesystem::path &file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"reskate-atomic-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto target = directory / L"trainer.json", blocked = directory / L"directory.json";
    std::filesystem::path preserved_path, second_preserved_path;
    try {
        require(std::filesystem::create_directory(directory), "Create a private test fixture");
        require(!write_atomic(target, "original\n"), "Create an initial store");
        const std::string large(2 * 1024 * 1024 + 31, 'x');
        require(!write_atomic(target, large) && read(target) == large, "Write all chunks and replace the complete store");
        require(!write_atomic(target, "new\n") && read(target) == "new\n", "Replacement truncates only the new temporary file");
        std::filesystem::create_directory(blocked);
        require(static_cast<bool>(write_atomic(blocked, "cannot replace directory")) && std::filesystem::is_directory(blocked), "Failed replacement preserves an existing directory");
        require(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY), "Prepare a read-only store");
        require(static_cast<bool>(write_atomic(target, "must fail")) && read(target) == "new\n", "A refused replacement preserves the last good store");
        require(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL), "Restore fixture permissions");
        require(static_cast<bool>(write_atomic(directory / L"missing" / L"trainer.json", "must fail")), "Absent parent fails without creating directories");
        std::size_t files{};
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            require(entry.path() == target || entry.path() == blocked, "Successful and failed saves leave no temporary debris"); ++files;
        }
        require(files == 2, "Only our complete target and blocked directory remain");
        require(!write_atomic(target, "") && read(target).empty(), "Empty payload still creates a complete replacement");
        require(!write_atomic(target, "{unreadable-original"), "Prepare a damaged profile fixture");
        const auto held = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(held != INVALID_HANDLE_VALUE, "Hold a profile without rename sharing");
        const auto denied = dingosdk::trainer::storage::preserve_unreadable(target);
        CloseHandle(held);
        require(denied.error && denied.backup.empty() && read(target) == "{unreadable-original", "Failed recovery keeps the original bytes at their original path");
        const auto preserved = dingosdk::trainer::storage::preserve_unreadable(target); preserved_path = preserved.backup;
        require(!preserved.error && !preserved.backup.empty() && preserved.backup.parent_path() == directory && !std::filesystem::exists(target) && read(preserved.backup) == "{unreadable-original", "Recovery atomically preserves a regular profile beside its original path");
        require(!write_atomic(target, "fresh-profile") && read(target) == "fresh-profile" && read(preserved.backup) == "{unreadable-original", "A fresh save never replaces the recovery backup");
        const auto second_preserved = dingosdk::trainer::storage::preserve_unreadable(target); second_preserved_path = second_preserved.backup;
        require(!second_preserved.error && second_preserved.backup != preserved.backup && read(second_preserved.backup) == "fresh-profile" && read(preserved.backup) == "{unreadable-original", "Repeated recovery produces distinct backups and retains both generations");
        require(!write_atomic(target, "second-fresh-profile"), "New saves can follow repeated preservation");
        require(dingosdk::trainer::storage::preserve_unreadable(blocked).error && std::filesystem::is_directory(blocked), "Recovery refuses to move directories");
        const auto absent = dingosdk::trainer::storage::preserve_unreadable(directory / L"absent.json");
        require(!absent.error && absent.backup.empty(), "An already absent file needs no invented backup");
        std::filesystem::remove(second_preserved_path); std::filesystem::remove(preserved_path); std::filesystem::remove(target); std::filesystem::remove(blocked); std::filesystem::remove(directory);
        std::cout << "Atomic trainer store replacement regressions passed\n";
    } catch (const std::exception &error) {
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::error_code ignored;
        if (!second_preserved_path.empty()) std::filesystem::remove(second_preserved_path, ignored);
        if (!preserved_path.empty()) std::filesystem::remove(preserved_path, ignored);
        std::filesystem::remove(target, ignored); std::filesystem::remove(blocked, ignored); std::filesystem::remove(directory, ignored);
        std::cerr << error.what() << '\n'; return 1;
    }
}
