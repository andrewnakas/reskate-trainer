// Scans the installed game's level data (first argument) and checks the
// world-layer catalog it produces, including a round trip through the cache
// file. Skips when no game folder is configured.
#include "Engine/Vfs/world_layer_scan.h"
#include <algorithm>
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main(int argc, char** argv) {
    using namespace dingosdk;
    if (argc < 2 || !std::filesystem::exists(std::filesystem::path(argv[1]) / "Skate.exe")) {
        std::cout << "No game folder given; skipped.\n";
        return 0;
    }
    const std::filesystem::path root = argv[1];
    const auto catalog = world_layer_scan::scan(root);
    check(catalog.anchors.size() == 6, "one anchor per map");
    check(catalog.nodes.size() >= 200 && catalog.layers.size() < catalog.nodes.size() &&
        catalog.layers.size() + 8 > catalog.nodes.size(), "a row per layer but the essential ones");
    for (std::size_t i = 0; i < catalog.nodes.size(); ++i) {
        const auto parent = catalog.nodes[i].parent;
        check(parent >= -1 && parent < static_cast<int>(i), "parents come first");
    }
    const auto has = [&](std::string_view key) {
        return std::any_of(catalog.layers.begin(), catalog.layers.end(), [&](const WorldLayer& layer) { return layer.key == key; });
    };
    for (const auto* key : {"bam_tod_1_morning", "bam_tod_7_weathernight", "grom_tod_5_night", "bam_debug_tests"})
        check(has(key), key);
    // No row for what every map is built out of: a player cannot switch off
    // the world and then wonder which row did it.
    check(std::none_of(catalog.layers.begin(), catalog.layers.end(), [](const WorldLayer& layer) {
        return layer.key.find("coregameassets") != std::string::npos;
    }), "CoreGameAssets has no row to switch");
    check(std::any_of(catalog.nodes.begin(), catalog.nodes.end(), [](const WorldLayerNode& node) {
        std::string bundle = node.bundle;
        std::transform(bundle.begin(), bundle.end(), bundle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return bundle.find("coregameassets") != std::string::npos;
    }), "but it keeps its node, so its children are still reachable");
    check(std::none_of(catalog.nodes.begin(), catalog.nodes.end(), [](const WorldLayerNode& node) {
        std::string bundle = node.bundle;
        std::transform(bundle.begin(), bundle.end(), bundle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return bundle.find("activitypresentation") != std::string::npos || bundle.find("commlot") != std::string::npos;
    }), "activity presentations and park lots stay with the game");

    const auto file = std::filesystem::temp_directory_path() / "dingosdk-world-layers-test.json";
    std::filesystem::remove(file);
    const auto written = world_layer_scan::load_or_scan(root, file);
    const auto cached = world_layer_scan::load_or_scan(root, file);
    check(std::filesystem::exists(file) && cached.layers.size() == written.layers.size() &&
        cached.nodes.size() == written.nodes.size() && cached.anchors == written.anchors, "cache round trip");
    std::filesystem::remove(file);
    std::cout << catalog.layers.size() << " world layers across " << catalog.anchors.size() << " maps.\n";
    return failures ? 1 : 0;
}
