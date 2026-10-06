#include "face_scan.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

// SKATER > FACE SCAN: the QR code for the phone, and the kept scan. The page only queues
// `face ...` console commands and reads face_scan::view().
namespace dingosdk::overlay::menu {
namespace {
ImU32 colour(const std::string &hex) {
    if (hex.size() != 7) return IM_COL32(128, 128, 128, 255);
    const auto part = [&](std::size_t at) { return static_cast<int>(std::strtoul(hex.substr(at, 2).c_str(), nullptr, 16)); };
    return IM_COL32(part(1), part(3), part(5), 255);
}
void swatch(SkateMenu &menu, const char *label, const std::string &hex) {
    field(menu, label);
    const auto at = ImGui::GetCursorScreenPos();
    const float side = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + side * 2, at.y + side), colour(hex));
    ImGui::Dummy(ImVec2(side * 2, side));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(hex.c_str());
}
// White quiet zone, dark modules: drawn as rectangles, so it needs no texture of its own.
void qr(const face_scan::View &view) {
    const int quiet = 4, modules = view.qr_size + quiet * 2;
    const float side = std::min(ImGui::GetContentRegionAvail().x, px(330));
    const float module = std::floor(side / static_cast<float>(modules));
    if (module < 1) return;
    const auto at = ImGui::GetCursorScreenPos();
    auto *draw = ImGui::GetWindowDrawList();
    const float total = module * static_cast<float>(modules);
    draw->AddRectFilled(at, ImVec2(at.x + total, at.y + total), IM_COL32(255, 255, 255, 255));
    for (int y = 0; y < view.qr_size; ++y)
        for (int x = 0; x < view.qr_size; ++x)
            if (view.qr[static_cast<std::size_t>(y * view.qr_size + x)]) {
                const ImVec2 corner(at.x + module * static_cast<float>(x + quiet), at.y + module * static_cast<float>(y + quiet));
                draw->AddRectFilled(corner, ImVec2(corner.x + module, corner.y + module), IM_COL32(0, 0, 0, 255));
            }
    ImGui::Dummy(ImVec2(total, total));
}
} // namespace

void face_scan_page(SkateMenu &menu, const Model &, const CallbacksV3 &callbacks) {
    const auto view = face_scan::view();
    using Phase = face_scan::View::Phase;
    const bool can = callbacks.queue_console_command != nullptr;

    begin_card(menu, "face-scan", "FACE SCAN", "Your face, skin tone and hair from your phone.");
    if (view->phase == Phase::waiting && view->qr_size) {
        qr(*view);
        info(menu, "Or open", view->url);
        info(menu, "Code", view->code);
        note(view->status.c_str());
        if (ImGui::Button("CANCEL")) send_console(menu, callbacks, "face cancel");
    } else {
        note("Scan the QR code with your phone's camera. The skatemods.com page scans your face on the phone and "
             "sends your game a skin colour, hair colour and style, and a face texture. It is deleted from the site "
             "once your game downloads it.");
        if (view->phase == Phase::downloading || view->phase == Phase::failed || view->phase == Phase::received)
            info(menu, "Status", view->status);
        if (primary_button(menu, view->saved ? "SCAN AGAIN" : "SCAN WITH PHONE", can)) send_console(menu, callbacks, "face scan");
    }
    end_card();

    if (view->saved) {
        begin_card(menu, "face-saved", "YOUR SCAN");
        swatch(menu, "Skin", view->scan.skin);
        swatch(menu, "Hair", view->scan.hair);
        info(menu, "Hairstyle", view->scan.hair_style.empty() ? "keep mine" : view->scan.hair_style);
        info(menu, "Facial hair", view->scan.facial_hair.empty() ? "keep mine" : view->scan.facial_hair);
        info(menu, "Face texture", view->scan.face ? "yes" : "no");
        bool enabled = view->enabled;
        if (toggle_row(menu, "Use my scan", "Put the scan on your skater. Off shows their own look.", enabled, can))
            send_console(menu, callbacks, enabled ? "face on" : "face off");
        if (!view->applied.empty()) note(view->applied.c_str());
        if (ImGui::Button("DELETE SCAN")) send_console(menu, callbacks, "face forget");
        end_card();
    }
}
} // namespace dingosdk::overlay::menu
