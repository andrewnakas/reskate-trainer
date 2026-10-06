#include "face_scan.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Multiplayer/Remote/native_cosmetics.h"
#include "Extension/Multiplayer/Remote/native_cosmetics_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>

// `face dump`: what the face scan has to find before it can change anything. Nothing in ReSkate
// knows where the skater's skin, hair and face live, so this writes down everything the local
// skater is made of: the recipe template's slots by name, the recipe it wears, and each item
// controller with every parameter of every material instance. Run it, change the skin tone or
// hair in the game's creator, run it again, and diff the two files.
//
// Read-only. Every read is bounded and checked (memory::peek_bytes); a part that cannot be read
// is noted in the file and the walk goes on.
namespace dingosdk::face_scan {
namespace {
bool read(std::uintptr_t address, void *out, std::size_t size) { return memory::peek_bytes(address, out, size); }
template <class T> bool peek(std::uintptr_t address, T &value) { return address && read(address, &value, sizeof(T)); }
std::uintptr_t pointer(std::uintptr_t address) {
    std::uintptr_t value{};
    return peek(address, value) ? value : 0;
}
// A printable C string at `address` (up to 160 characters), or empty.
std::string text_at(std::uintptr_t address) {
    if (address < 0x10000 || address > 0x00007fffffffffffULL) return {};
    std::array<char, 161> chunk{};
    if (!read(address, chunk.data(), chunk.size() - 1)) {
        // A short string at the end of a page: byte by byte.
        for (std::size_t i = 0; i + 1 < chunk.size(); ++i)
            if (!peek(address + i, chunk[i]) || !chunk[i]) break;
    }
    std::string out;
    for (auto c : chunk) {
        if (!c) break;
        if (c < 32 || c == 127) return {};
        out += c;
    }
    return out.size() >= 3 ? out : std::string{};
}
std::string hex(std::uintptr_t address, std::size_t size) {
    std::vector<unsigned char> bytes(size);
    if (!read(address, bytes.data(), size)) return "<unreadable>";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) out += std::format("{}{:02x}", i && i % 8 == 0 ? " " : "", bytes[i]);
    return out;
}
// Every pointer in the `size` bytes at `address` that leads to a string, one or two steps away:
// type names, asset paths and texture names show up this way.
std::string names_near(std::uintptr_t address, std::size_t size) {
    std::string out;
    for (std::size_t at = 0; at + 8 <= size; at += 8) {
        const auto p = pointer(address + at);
        if (auto name = text_at(p); !name.empty()) { out += std::format(" +{:x}=\"{}\"", at, name); continue; }
        for (std::size_t inner = 0; inner < 0x40; inner += 8)
            if (auto name = text_at(pointer(p + inner)); !name.empty()) {
                out += std::format(" +{:x}->+{:x}=\"{}\"", at, inner, name);
                break;
            }
    }
    return out;
}
void hexdump(std::ofstream &file, std::string_view indent, std::uintptr_t address, std::size_t size) {
    for (std::size_t at = 0; at < size; at += 32) file << std::format("{}+{:03x}: {}\n", indent, at, hex(address + at, 32));
}
// The recipe template's slots (RecipeTemplate.Slots, 24 bytes each: port name, slot asset, save
// flag; local_customization_runtime.cpp, cosmetic_slot_categories), by name.
void write_slots(std::ofstream &file, std::uintptr_t resource) {
    file << std::format("== recipe template slots (resource {:#x}) ==\n", resource);
    const auto slots = pointer(resource + 0x28);
    std::uint32_t count{};
    if (!slots || !peek(slots - 4, count) || (count &= 0x7fffffff) > 256) { file << "  <unreadable>\n"; return; }
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto port = text_at(pointer(slots + i * 24ULL));
        const auto slot = pointer(slots + i * 24ULL + 8) & ~std::uintptr_t{4};
        std::uint32_t hash{}, category_hash{};
        peek(slot + 0x4c, hash);
        const auto category = pointer(slot + 0x30) & ~std::uintptr_t{4};
        peek(category + 0x38, category_hash);
        file << std::format("  [{:3}] slot {:08x} {:<44} category {:08x} {:<32} port {}\n", i, hash,
                            text_at(pointer(slot + 0x20)), category_hash, text_at(pointer(category + 0x28)), port);
    }
}
void write_recipe(std::ofstream &file, const char *title, const multiplayer::CosmeticRecipe &recipe) {
    file << std::format("== {} recipe (template {} v{}) ==\n  scalars:", title, recipe.key, recipe.version);
    for (const auto scalar : recipe.scalars) {
        float as_float{};
        std::memcpy(&as_float, &scalar, 4);
        file << std::format(" {:08x}({:.4g})", scalar, as_float);
    }
    file << "\n";
    for (const auto &item : recipe.items) {
        file << std::format("  slot {:08x} {}", item.slot, item.asset);
        if (!item.parameters.empty()) {
            file << "  params";
            for (const auto word : item.parameters) file << std::format(" {:08x}", word);
        }
        file << "\n";
    }
}
// Every parameter node of one material instance (developer_hoodie_material.h, parameter_map:
// a hash map of typed nodes at +0x268, chained at +0x40).
void write_material(std::ofstream &file, std::uintptr_t base, std::uintptr_t material, bool raw) {
    const auto buckets = pointer(material + 0x268);
    std::uint32_t size{}, total{};
    peek(material + 0x270, size);
    peek(material + 0x274, total);
    file << std::format("      parameters: {} in {} buckets{}\n", total, size, names_near(material, 0x80));
    if (raw) hexdump(file, "        ", material, 0x300);
    if (!buckets || !size || size > 1024 || total > 512) return;
    const auto stop = pointer(buckets + size * 8ULL);
    std::uint32_t visited{};
    for (std::uint32_t bucket = 0; bucket < size && visited < total; ++bucket)
        for (auto node = pointer(buckets + bucket * 8ULL); node && node != stop && visited < total; node = pointer(node + 0x40)) {
            ++visited;
            struct { std::uint16_t identity, size, type, count; std::uint64_t hash; } key{};
            if (!peek(node, key)) break;
            const auto type = pointer(node + 0x20);
            const auto info = pointer(type);
            std::uint32_t type_hash{}, flags{};
            peek(info, type_hash);
            peek(node + 0x28, flags);
            const auto in_game = type >= base && type < base + 0x09144000;
            file << std::format("        key {:016x} id {:04x} size {:3} type {:4} count {} flags {:x} typeinfo {:08x}{} value {}",
                                key.hash, key.identity, key.size, key.type, key.count, flags, type_hash,
                                in_game ? "" : " (type outside image)", hex(node + 0x10, std::min<std::size_t>(key.size ? key.size : 16, 48)));
            if (key.size == 12) {
                std::array<float, 3> color{};
                if (peek(node + 0x10, color)) file << std::format(" rgb({:.3f},{:.3f},{:.3f})", color[0], color[1], color[2]);
            }
            // A texture or other resource parameter: what its pointers lead to.
            if (key.size == 8 || key.size == 16) file << names_near(node + 0x10, key.size);
            file << names_near(info, 0x30) << "\n";
        }
}
// Every controller on the skater's appearance component, whatever its slot, with each named
// material and its variants (developer_hoodie_material.h, materials()).
void write_controllers(std::ofstream &file, std::uintptr_t base, std::uintptr_t appearance, bool raw) {
    const auto begin = pointer(appearance + 0x90), end = pointer(appearance + 0x98);
    file << std::format("== controllers (appearance {:#x}, {} entries) ==\n", appearance, end >= begin ? (end - begin) / 8 : 0);
    if (!begin || end < begin || (end - begin) / 8 > 64) { file << "  <unreadable>\n"; return; }
    for (auto entry = begin; entry < end; entry += 8) {
        const auto item = pointer(entry);
        if (!item) continue;
        std::uint32_t flags{}, size{}, total{};
        peek(item + 0xb0, flags);
        peek(item + 0x1f8, size);
        peek(item + 0x1fc, total);
        file << std::format("  [{}] item {:#x} \"{}\" flags {:x} owner {} materials {}\n", (entry - begin) / 8, item,
                            text_at(pointer(item + 0x30)), flags, pointer(item + 0x48) == appearance + 0x40 ? "this" : "other",
                            total);
        file << "      near:" << names_near(item, 0x200) << "\n";
        if (raw) hexdump(file, "      ", item, 0x200);
        const auto buckets = pointer(item + 0x1f0);
        if (!buckets || !size || size > 256 || total > 128) continue;
        std::uint32_t visited{};
        for (std::uint32_t bucket = 0; bucket < size && visited < total; ++bucket)
            for (auto node = pointer(buckets + bucket * 8ULL); node && visited < total; node = pointer(node + 0x28)) {
                std::uint32_t name_hash{};
                if (!peek(node, name_hash) || name_hash % size != bucket) break;
                ++visited;
                std::array<std::uintptr_t, 4> seen{};
                for (std::size_t variant = 0; variant < seen.size(); ++variant) {
                    const auto material = pointer(node + 8 + variant * 8);
                    if (!material || std::find(seen.begin(), seen.begin() + variant, material) != seen.begin() + variant) continue;
                    seen[variant] = material;
                    file << std::format("    material {:08x} variant {} @ {:#x}\n", name_hash, variant, material);
                    write_material(file, base, material, raw);
                }
            }
    }
}
} // namespace

std::string dump(std::uintptr_t base, std::uintptr_t client, bool raw) {
    if (!base || !client) return "error: no game session yet. Load into the world first.";
    const auto local = multiplayer::capture_local(base, client, false);
    if (!local.ready || !local.entity) return "error: the local skater is not ready: " + local.detail;
    const auto directory = data_directory() / L"dump";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const auto name = std::format(L"face-dump-{:04}{:02}{:02}-{:02}{:02}{:02}.txt", now.wYear, now.wMonth, now.wDay,
                                  now.wHour, now.wMinute, now.wSecond);
    std::ofstream file(directory / name, std::ios::binary | std::ios::trunc);
    if (!file) return "error: could not write " + (directory / name).string();
    file << std::format("ReSkate face dump. base {:#x} client {:#x} skater {:#x}\n\n", base, client, local.entity);
    std::size_t controllers{};
    try {
        auto reader = [](std::uintptr_t address, void *out, std::size_t size) { return memory::peek_bytes(address, out, size); };
        multiplayer::CosmeticMemory<decltype(reader) &> m{reader, base};
        const auto component = m.component(local.entity);
        write_slots(file, m.resource(component));
        std::string detail;
        if (const auto appearance = multiplayer::capture_cosmetics(base, local, detail)) {
            write_recipe(file, "skater", appearance->skater);
            write_recipe(file, "board", appearance->board);
        } else file << "== recipe ==\n  <not captured: " << detail << ">\n";
        const auto appearance = multiplayer::read_native_component(reader, local.entity, base + addr::engine::skater_appearance_vtable);
        controllers = (pointer(appearance + 0x98) - pointer(appearance + 0x90)) / 8;
        write_controllers(file, base, appearance, raw);
    } catch (const std::exception &failure) {
        file << "\n<stopped: " << failure.what() << ">\n";
    }
    file.flush();
    return std::format("Wrote the skater's slots, recipe and {} controllers to {}.", controllers, (directory / name).string());
}
} // namespace dingosdk::face_scan
