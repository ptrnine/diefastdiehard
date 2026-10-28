#include <iostream>
#include <string>
#include <vector>

/* Parses the sf::Event subtypes (struct Closed, struct Resized, ...) from the preprocessed SFML/Window/Event.hpp */
int main() {
    std::vector<std::string> event_types;
    std::string line;

    bool on_class = false;

    /* The input is the preprocessed header */
    while (std::getline(std::cin, line)) {
        std::string_view l = line;

        if (!on_class) {
            if (l.starts_with("class ") && l.ends_with(" Event"))
                on_class = true;
            continue;
        }

        if (l == "};")
            break;

        /* The subtypes are the structs declared directly in class Event */
        if (!l.starts_with("    struct "))
            continue;

        l = l.substr(std::string_view("    struct ").size());
        if (l.empty() || l.find_first_of(" ;{") != std::string_view::npos)
            continue;
        event_types.emplace_back(l);
    }

    std::cout <<
        "#pragma once\n"
        "\n"
        "#include <optional>\n"
        "#include <string>\n"
        "#include <map>\n"
        "#include <SFML/Window/Event.hpp>\n"
        "#include \"base/log.hpp\"\n"
        "\n"
        "namespace dfdh {\n"
        "\n"
        "inline std::string sfml_event_type_to_str(const sf::Event& event) {\n";
    for (auto& event : event_types)
        std::cout <<
            "    if (event.is<sf::Event::" << event << ">()) return \"" << event << "\";\n";
    std::cout <<
        "    return {};\n"
        "}\n"
        "/* An event of the given type with the default values */\n"
        "inline std::optional<sf::Event> sfml_str_to_event(std::string_view str) {\n"
        "    static auto map = []{\n"
        "        return std::map<std::string_view, sf::Event>{\n";
    for (auto& event: event_types)
        std::cout <<
        "        {\"" << event << "\", sf::Event::" << event << "{}},\n";
        std::cout << "    };\n"
            "    }();\n"
            "    auto found = map.find(str);\n"
            "    if (found != map.end())\n"
            "        return found->second;\n"
            "    glog().error(\"invalid sfml event type: {}\", str);\n"
            "    return {};\n"
            "}\n"
            "} // namespace dfdh\n";
}
