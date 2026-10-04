#pragma once

#ifdef _WIN32
#include <Windows.h>
#endif

#include <stdexcept>
#include <string>

// Shared by launcher_support.cpp, launcher_pe.cpp and launcher_injection.cpp.
namespace dingosdk::launcher::detail {

#ifdef _WIN32
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    HANDLE release() noexcept { const auto value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_{};
};

[[noreturn]] inline void fail_windows(const char* message, DWORD error = GetLastError()) {
    throw std::runtime_error(std::string(message) + " (Windows error " +
        std::to_string(error) + ")");
}
#endif

[[noreturn]] inline void fail(const char* message) { throw std::runtime_error(message); }

} // namespace dingosdk::launcher::detail
