// Only the workshop and trainer are exercised here. Unrelated pages are blank
// seams so the real menu shell can link without game-facing UI dependencies.
// This source belongs exclusively to the headless native UI test executable.
#include "Engine/Core/Platform/launcher_support.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include "Extension/UI/Overlay/modding_menu.h"
namespace dingosdk::launcher {
OverlayKeys overlay_keys() noexcept { return {}; }
std::string key_cap(unsigned) { return "INS"; }
}
namespace dingosdk::overlay {
void draw_modding_menu(SkateMenu &, int) { ImGui::TextUnformatted("Unrelated page fixture"); }
namespace menu {
void map_page(SkateMenu &, const Model &, const CallbacksV3 &) { ImGui::TextUnformatted("Unrelated page fixture"); }
void world_page(SkateMenu &, const Model &, const CallbacksV3 &) { ImGui::TextUnformatted("Unrelated page fixture"); }
void build_page(SkateMenu &, const Model &, const CallbacksV3 &) { ImGui::TextUnformatted("Unrelated page fixture"); }
void skater_page(SkateMenu &, const Model &, const CallbacksV3 &) { ImGui::TextUnformatted("Unrelated page fixture"); }
void multiplayer_page(SkateMenu &, const Model &, const CallbacksV3 &) { ImGui::TextUnformatted("Unrelated page fixture"); }
void progression_page(SkateMenu &, const Model &, const CallbacksV3 &) { ImGui::TextUnformatted("Unrelated page fixture"); }
void settings_page(SkateMenu &, const Model &, const CallbacksV3 &) { ImGui::TextUnformatted("Unrelated page fixture"); }
void developer_page(SkateMenu &, const Model &, const CallbacksV3 &) { ImGui::TextUnformatted("Unrelated page fixture"); }
}
}

namespace dingosdk::trainer {
// Clipboard staging is a game-facing seam, not exercised by these layout tests.
bool stage_import(std::string_view) noexcept { return false; }
}

namespace dingosdk::overlay::menu { void special_page(SkateMenu &, const Model &, const CallbacksV3 &) {} }
extern "C" void DingoSDKOverlayReadControllerInput(dingosdk::ControllerInput *input, bool) { if(input) *input = {}; }
