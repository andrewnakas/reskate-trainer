#pragma once
#include <cstdint>

namespace dingosdk::game::build::v20260929::local_placements {
// Placed-object manager vtable; its id -> entity hash table starts at +0x10.
inline constexpr std::uintptr_t placement_manager_vtable = 0x60e0380;
// uint32 registration offset of the server player's component holding the native
// simulation owner (+0x98). 0x7b3470/0x7befd0 activate rendering and collision
// only for that owner.
inline constexpr std::uintptr_t player_component_offset = 0x6f7cf60;
// uint32 offset of the client context record whose +0x100 byte is 1 once ready.
inline constexpr std::uintptr_t client_ready_offset = 0x7215038;
// Pointer to the teleport manager; its first uint32 is 0 while idle.
inline constexpr std::uintptr_t teleport_manager = 0x71ee790;
// Reference only (PlacementCreateRecipe mirrors these, nothing calls them): the
// create-message handler 0x87da20; the free-roam drop 0x7af5b0, whose park
// context byte (127) comes from 0x6f7c750; the source enum codec 0x58f96c0/0x58f8c50.
}
