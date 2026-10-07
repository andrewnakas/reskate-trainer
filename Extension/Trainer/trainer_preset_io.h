#pragma once
#include "Engine/Core/Json/json.h"
#include <cmath>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>

namespace dingosdk::trainer::storage {
inline constexpr std::size_t shared_bytes_limit = 2 * 1024 * 1024, preset_values_limit = 8192;
inline std::optional<std::string> read_bounded(const std::filesystem::path &path, std::size_t limit) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string result(limit + 1, '\0');
    file.read(result.data(), static_cast<std::streamsize>(result.size()));
    const auto count = static_cast<std::size_t>(file.gcount());
    if (count > limit || file.bad()) return std::nullopt;
    result.resize(count); return result;
}
using PresetValues = std::map<std::string, double, std::less<>>;
inline std::optional<PresetValues> decode_values(const Json &json) {
    if (!json.is_object() || json.empty() || json.size() > preset_values_limit) return std::nullopt;
    PresetValues result;
    for (const auto &[key, value] : json.items()) {
        if (key.empty() || key.size() > 256 || !value.is_number()) return std::nullopt;
        const auto number = value.get<double>();
        if (!std::isfinite(number)) return std::nullopt;
        result[key] = number;
    }
    return result;
}
}
