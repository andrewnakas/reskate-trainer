#include "item_thumbnails.h"
#include "game_bundles.h"
#include "Engine/Core/Platform/path_text.h"
#include <algorithm>
#include <stdexcept>

namespace dingosdk::vfs {
namespace fb = frostbite;
namespace {
// Where the game keeps its item thumbnails: one texture per owned item.
constexpr std::string_view bundle_name = "win32/characters/customization/configs/cas_main_sharedbundle";
constexpr std::string_view prefix = "thumbnail/tool/";

bool build_kit_item(std::string_view item) {
    return item.starts_with("own_bk") && !item.ends_with("_lrg") &&
        std::all_of(item.begin(), item.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}
}

ThumbnailRead read_build_kit_thumbnails(const std::filesystem::path& gameRoot, std::uint32_t side) {
    const GameData data(gameRoot);
    // The bundle's superbundle is looked up rather than assumed, so a game
    // update that moves it only costs a scan of the TOCs.
    fb::TocDocument toc;
    std::optional<GameBundle> bundle;
    std::error_code error;
    std::string problems;
    for (std::filesystem::recursive_directory_iterator it(gameRoot / L"Data" / L"Win32", error), end;
         it != end && !error && !bundle; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != L".toc") continue;
        try {
            toc = data.read_toc(std::filesystem::relative(it->path(), gameRoot / L"Data").generic_string());
            bundle = data.read_bundle(toc, bundle_name);
        } catch (const std::exception& failure) {
            if (problems.size() < 600)
                problems += "; " + path_utf8(it->path().filename()) + ": " + failure.what();
        }
    }
    if (!bundle) throw std::runtime_error("The game's thumbnail bundle was not found" + problems);
    ThumbnailRead result;
    const auto& resources = bundle->manifest.resources;
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        if (resource.resourceType != fb::texture_resource_type || !resource.name.starts_with(prefix)) continue;
        const auto item = std::string_view(resource.name).substr(prefix.size());
        if (!build_kit_item(item)) continue;
        try {
            const auto* payload = bundle->payload(fb::AssetKind::resource, index);
            if (!payload) throw std::runtime_error("missing payload");
            const auto header = fb::read_texture_header(data.read(*payload), resource.resourceMeta);
            // Pixels are usually in the same bundle; otherwise the TOC's own chunks.
            std::vector<std::byte> pixels;
            std::size_t chunk{};
            if (bundle->find_chunk(header.chunk, &chunk)) {
                pixels = data.read(*bundle->payload(fb::AssetKind::chunk, chunk));
            } else {
                const auto found = std::find_if(toc.chunks.begin(), toc.chunks.end(),
                    [&](const fb::TocChunk& entry) { return entry.guid == header.chunk; });
                if (found == toc.chunks.end()) throw std::runtime_error("missing pixel chunk");
                pixels = data.read({found->location, found->offset, found->size});
            }
            auto image = fb::resize(fb::decode_texture(header, pixels, side), side, side);
            result.thumbnails.push_back({std::string(item), std::move(image)});
        } catch (...) {
            ++result.failed;
        }
    }
    return result;
}
}
