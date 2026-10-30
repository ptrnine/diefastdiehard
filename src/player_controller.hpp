#pragma once

#include "base/types.hpp"
#include "base/cfg.hpp"
#include "base/fixed_string.hpp"
#include "key_generated.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace dfdh {

struct key_str_map_t {
    key_str_map_t() {
        for (int i = 0; i < int(sf::Keyboard::KeyCount); ++i)
            str_to_key[sfml_key_to_str(sf::Keyboard::Key(i))] = i;

        /* SFML 2 names of the renamed keys (the saved settings) */
        static constexpr std::pair<const char*, sf::Keyboard::Key> sfml2_names[] = {
            {"Return", sf::Keyboard::Key::Enter},
            {"BackSpace", sf::Keyboard::Key::Backspace},
            {"BackSlash", sf::Keyboard::Key::Backslash},
            {"SemiColon", sf::Keyboard::Key::Semicolon},
            {"Dash", sf::Keyboard::Key::Hyphen},
            {"Quote", sf::Keyboard::Key::Apostrophe},
            {"Tilde", sf::Keyboard::Key::Grave},
        };
        for (auto [name, key] : sfml2_names)
            str_to_key.emplace(name, int(key));
    }

    [[nodiscard]]
    std::string to_str(int key) const {
        return sfml_key_to_str(sf::Keyboard::Key(key));
    }

    [[nodiscard]]
    int to_key(const std::string& str) const {
        auto found = str_to_key.find(str);
        if (found != str_to_key.end())
            return found->second;
        return -1;
    }

    std::map<std::string, int> str_to_key;
};

static inline key_str_map_t& key_str_map() {
    static key_str_map_t inst;
    return inst;
}

struct player_controller {
    /* An unset binding (also sf::Keyboard::Key::Unknown: the input ignores unknown keys) */
    static constexpr int unset = -1;
    static constexpr auto unset_name = "none";
    static constexpr auto section_prefix = "player_controller_";

    /* Declared before the bindings table, which points at them */
    int up = unset, down = unset, left = unset, right = unset, shot = unset, adjust_up = unset, grenade = unset, drop = unset;

    struct binding_t {
        const char*            cfg_key;
        const char*            label;
        int player_controller::*key;
        const char*            default_key; /* controller 0 only: the others start unset */
    };

    static constexpr binding_t bindings[] = {
        {"key_up", "jump", &player_controller::up, "Up"},
        {"key_down", "jump down", &player_controller::down, "Down"},
        {"key_left", "left", &player_controller::left, "Left"},
        {"key_right", "right", &player_controller::right, "Right"},
        {"key_shot", "shot", &player_controller::shot, "Comma"},
        {"key_adjust_up", "long shot", &player_controller::adjust_up, "M"},
        {"key_grenade", "grenade", &player_controller::grenade, "Period"},
        {"key_drop", "drop gun", &player_controller::drop, "G"},
    };

    player_controller(u32 id): _id(id) {
        load();
    }

    /* (Re)read the bindings of the controller id */
    void load(u32 id) {
        _id = id;
        load();
    }

    void load() {
        auto conf = settings_cfg();
        auto sect = conf.get_or_create(section_name(_id));
        for (auto& b : bindings)
            this->*b.key = key_str_map().to_key(sect.value_or_default_and_set(b.cfg_key, std::string(_id == 0 ? b.default_key : unset_name)));
    }

    [[nodiscard]]
    u32 id() const {
        return _id;
    }

    /* The [player_controller_<id>] sections of data/settings/players_settings.cfg */
    static cfg settings_cfg() {
        return cfg("data/settings/players_settings.cfg"s,
                   cfg_mode::create_if_not_exists | cfg_mode::commit_at_destroy | cfg_mode::autocreate_dir);
    }

    static cfg_section_name section_name(u32 id) {
        return cfg_section_name{std::string(section_prefix) + std::to_string(id)};
    }

    /* The configured controllers, sorted. Controller 0 always exists */
    static std::vector<u32> ids() {
        std::vector<u32> res{0};
        auto conf = settings_cfg();
        for (auto& name : conf.list_sections()) {
            std::string_view sv = name;
            if (!sv.starts_with(section_prefix))
                continue;
            sv.remove_prefix(std::strlen(section_prefix));
            if (sv.empty() || !std::ranges::all_of(sv, [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            if (auto id = u32(std::stoul(std::string(sv))); id != 0)
                res.push_back(id);
        }
        std::ranges::sort(res);
        return res;
    }

    /* A new controller: the smallest free id, no keys set */
    static u32 create() {
        auto used = ids();
        u32  id   = 1;
        while (std::ranges::find(used, id) != used.end())
            ++id;
        [[maybe_unused]] player_controller created{id}; /* writes the unset keys */
        return id;
    }

    /* Delete the controller's section (not controller 0) */
    static void remove(u32 id) {
        if (id == 0)
            return;
        auto conf = settings_cfg();
        conf.try_remove_section(section_name(id));
    }

    /* Bind the key (unset: clear) to the binding_t::cfg_key of the controller */
    static void set_key(u32 id, const char* cfg_key, int key) {
        auto conf = settings_cfg();
        conf.get_or_create(section_name(id)).set(cfg_key, key == unset ? std::string(unset_name) : key_str_map().to_str(key));
    }

    virtual ~player_controller() = default;

    u32 _id;
    player_name_t player_name;
};

}
