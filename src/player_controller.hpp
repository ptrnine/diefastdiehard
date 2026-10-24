#pragma once

#include "base/types.hpp"
#include "base/cfg.hpp"
#include "base/fixed_string.hpp"
#include "key_generated.hpp"

namespace dfdh {

struct key_str_map_t {
    key_str_map_t() {
        for (int i = sf::Keyboard::A; i < sf::Keyboard::KeyCount; ++i)
            str_to_key[sfml_key_to_str(sf::Keyboard::Key(i))] = i;
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
    player_controller(u32 id): _id(id) {

        auto sect_name = cfg_section_name{"player_controller_" + std::to_string(_id)};
        auto path      = "data/settings/players_settings.cfg"s;
        auto conf      = cfg(path, cfg_mode::create_if_not_exists | cfg_mode::commit_at_destroy);
        auto sect      = conf.get_or_create(sect_name);

        up        = key_str_map().to_key(sect.value_or_default_and_set("key_up", "Up"s));
        down      = key_str_map().to_key(sect.value_or_default_and_set("key_down", "Down"s));
        left      = key_str_map().to_key(sect.value_or_default_and_set("key_left", "Left"s));
        right     = key_str_map().to_key(sect.value_or_default_and_set("key_right", "Right"s));
        shot      = key_str_map().to_key(sect.value_or_default_and_set("key_shot", "Comma"s));
        adjust_up = key_str_map().to_key(sect.value_or_default_and_set("key_adjust_up", "M"s));
        grenade   = key_str_map().to_key(sect.value_or_default_and_set("key_grenade", "Period"s));
    }

    /*
    control_keys(bool, int id): _id(id) {
        up      = sf::Keyboard::Up;
        down    = sf::Keyboard::Down;
        left    = sf::Keyboard::Left;
        right   = sf::Keyboard::Right;
        shot    = sf::Keyboard::Comma;
        grenade = sf::Keyboard::Dash;

        up      = sf::Keyboard::W;
        down    = sf::Keyboard::S;
        left    = sf::Keyboard::A;
        right   = sf::Keyboard::D;
        shot    = sf::Keyboard::Y;
        grenade = sf::Keyboard::U;
    }
    */

    void save() {
        // XXX: wtf
        //cfg().try_refresh_file("player_controller_" + std::to_string(_id));
    }

    virtual ~player_controller() = default;

    int up, down, left, right, shot, adjust_up, grenade;
    u32 _id;
    player_name_t player_name;
};

}
