// Lists every value of Gameplay/SkatePhysicsTuning the trainer can edit, as the game's own
// data names it.
//   dingosdk_trainer_tuning_dump <Skate folder>
#include "Extension/Skater/physics_tuning_model.h"
#include <cstring>
#include <iostream>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: dingosdk_trainer_tuning_dump <Skate folder>\n";
        return 2;
    }
    try {
        const auto model = dingosdk::physics_tuning::read_game_tuning(argv[1]);
        std::size_t unnamed{};
        for (std::size_t i = 0; i < model.fields.size(); ++i) {
            const auto &f = model.fields[i];
            const auto &name = model.field_names[i];
            if (name.empty()) ++unnamed;
            std::cout << (name.empty() ? "?" : name) << '\t' << std::hex << "0x" << f.offset << std::dec << '\t';
            if (f.real) {
                float value;
                std::memcpy(&value, model.image.data() + f.offset, 4);
                std::cout << "real\t" << value;
            } else if (f.flag) {
                std::cout << "flag\t" << static_cast<int>(model.image[f.offset]);
            } else {
                std::int64_t value{};
                std::memcpy(&value, model.image.data() + f.offset, f.size);
                std::cout << "int" << f.size * 8 << '\t' << value;
            }
            std::cout << '\n';
        }
        for (std::size_t i = 0; i < model.curve_slots.size(); ++i) {
            const auto &curve = model.curves[i];
            std::cout << (model.curve_names[i].empty() ? "?" : model.curve_names[i]) << '\t' << std::hex << "0x" << model.curve_slots[i]
                      << std::dec << "\tcurve\t" << (curve ? curve->points.size() / 0x1c : 0) << " points";
            if (curve)
                for (std::size_t p = 0; p + 0x1c <= curve->points.size(); p += 0x1c) {
                    float x, y;
                    std::memcpy(&x, curve->points.data() + p + 0xc, 4);
                    std::memcpy(&y, curve->points.data() + p + 0x14, 4);
                    std::cout << " (" << x << ", " << y << ")";
                }
            std::cout << '\n';
        }
        std::cerr << model.fields.size() << " values (" << unnamed << " unnamed), " << model.curve_slots.size() << " curves\n";
        return unnamed ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
