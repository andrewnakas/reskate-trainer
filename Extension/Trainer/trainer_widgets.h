#pragma once
#include <imgui_internal.h>

namespace dingosdk::overlay::menu {
struct ScalarInputFrame { const char *format; bool cancelled; };
// ImGui seeds its temporary editor and Escape baseline from the widget format.
// Keep idle labels compact, but use round-trip precision on every input activation
// path and throughout text editing.
inline ScalarInputFrame scalar_input(const char *label, const char *idle, const char *precise = "%.17g") {
    const auto id = ImGui::GetID(label);
    const auto &g = *GImGui;
    const bool editing = ImGui::TempInputIsActive(id);
    const auto origin = ImGui::GetCursorScreenPos();
    const bool hovered = ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + ImGui::CalcItemWidth(), origin.y + ImGui::GetFrameHeight()));
    const bool mouse_input = hovered && ((g.IO.KeyCtrl && g.IO.MouseClicked[0]) || g.IO.MouseClickedCount[0] == 2);
    const bool activate = mouse_input || g.NavActivateId == id ||
        (g.IO.ConfigDragClickToInputText && g.ActiveId == id && g.IO.MouseReleased[0]);
    return {editing || activate ? precise : idle, editing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)};
}
// A width/scale change can alter rows above a focused editor. Keep text entry
// within its actual child viewport without moving an ordinary drag control.
inline void keep_scalar_visible() {
    const auto &g = *GImGui;
    const auto id = g.LastItemData.ID;
    if (!id || (!ImGui::TempInputIsActive(id) && !(g.InputTextState.ID == id && g.ActiveId == id))) return;
    const auto *window = ImGui::GetCurrentWindow();
    const auto &rect = g.LastItemData.Rect;
    if (rect.Min.y < window->InnerClipRect.Min.y || rect.Max.y > window->InnerClipRect.Max.y) ImGui::SetScrollHereY(.5f);
}
} // namespace dingosdk::overlay::menu
