#pragma once

// A character bundle-reference table (characters/customization/configs/
// cas_main_bundlereftable and its kind): which bundle to load for each preset,
// looked up by the FNV-1a hash of the preset's full path and of its leaf name.
// A cosmetic mod registers each preset it adds with a pair of rows pointing at
// a bundle the base already lists, so two mods' tables differ from the base
// only by the presets each added and can be combined exactly.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::frostbite::bundle_ref {

// Resources named *_bundlereftable.
[[nodiscard]] bool is_table(std::string_view assetName);

struct Table {
    std::span<const std::byte> resource;
    std::span<const std::byte> resourceMeta;
};

struct MergedTable {
    std::vector<std::byte> resource;
    std::vector<std::byte> resourceMeta;
    std::size_t added{};     // presets taken from the edits
    std::size_t conflicts{}; // presets two edits put in different bundles; the first won
    // Presets added under their full path only, because another preset already
    // answers to the same leaf name (cust_tops/1_ap and cust_bottoms/1_ap).
    struct Shadowed {
        std::size_t edit{};  // index into `edits`
        std::string path;    // the preset left without a leaf lookup
        std::string holder;  // the preset its leaf name finds instead
    };
    std::vector<Shadowed> shadowed;
};

// Throws when a copy is not a table of this kind, or an edit changes the base's
// bundle list or existing rows (anything beyond added presets).
[[nodiscard]] MergedTable merge(const Table& base, std::span<const Table> edits);

} // namespace dingosdk::frostbite::bundle_ref
