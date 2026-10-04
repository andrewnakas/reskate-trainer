#include "trainer.h"
#include <mutex>

// The snapshots the menu and HUD read. Kept apart from trainer.cpp so the overlay library
// links without the game-facing half.
namespace dingosdk::trainer {
namespace {
struct Feed {
    std::mutex mutex;
    std::shared_ptr<const View> view = std::make_shared<const View>();
    Telemetry telemetry;
};
Feed &feed() {
    static auto *value = new Feed;
    return *value;
}
} // namespace
std::shared_ptr<const View> view() noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    return f.view;
}
Telemetry telemetry() noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    return f.telemetry;
}
void publish(std::shared_ptr<const View> next) noexcept {
    if (!next) return;
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    f.view = std::move(next);
}
void publish(const Telemetry &next) noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    f.telemetry = next;
}
} // namespace dingosdk::trainer
