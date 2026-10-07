#include "Extension/Trainer/trainer_preset_io.h"
#include <iostream>
#include <chrono>
#include <stdexcept>

void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        using namespace dingosdk; using namespace trainer::storage;
        auto values = Json::object();
        for (int i = 0; i < 5274; ++i) values["old-workshop-value-" + std::to_string(i)] = i + .125;
        const auto decoded = decode_values(values);
        require(decoded && decoded->size() == 5274, "Full previous workshop capture must not silently lose values beyond 512");
        values["bad"] = "not a number";
        require(!decode_values(values), "Invalid values refuse the complete import");
        values = Json::object(); for (std::size_t i = 0; i <= preset_values_limit; ++i) values[std::to_string(i)] = 1;
        require(!decode_values(values), "Oversized imports refuse instead of truncating");
        const auto directory = std::filesystem::temp_directory_path() / ("trainer-bounded-read-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory); const auto file = directory / "fixture.json";
        { std::ofstream out(file, std::ios::binary); out << "123456789"; }
        require(read_bounded(file, 9).value() == "123456789" && !read_bounded(file, 8), "Read validates byte limit before allocating or parsing a whole file");
        std::filesystem::remove(file); std::filesystem::remove(directory);
        std::cout << "Complete preset imports and bounded file reads passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
