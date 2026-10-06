#include "face_scan.h"
#include "Extension/Console/commands.h"

// The `face` console commands: the only way anything reaches the face scan, the menu included.
namespace dingosdk::console {
void register_face_commands(Commands &registry) {
    struct Verb {
        const char *name, *description, *usage;
    };
    const Verb verbs[]{
        {"status", "What the face scan is doing", nullptr},
        {"dump", "Write the skater's slots, recipe and material parameters to a file (discovery)", "[raw]"},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("face ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            out(face_scan::command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
