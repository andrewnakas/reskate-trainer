#include "face_scan.h"
#include <mutex>

// The snapshot the menu reads. Kept apart from face_scan.cpp so the overlay library links
// without the game-facing half.
namespace dingosdk::face_scan {
namespace {
struct Feed {
    std::mutex mutex;
    std::shared_ptr<const View> view = std::make_shared<const View>();
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
void publish(std::shared_ptr<const View> next) noexcept {
    if (!next) return;
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    f.view = std::move(next);
}
} // namespace dingosdk::face_scan
