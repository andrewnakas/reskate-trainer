#pragma once
namespace dingosdk::backtrace {
// Internal launcher mode, entered before game launch or logging initialization.
int run_reporter(int argc, wchar_t** argv) noexcept;
}
