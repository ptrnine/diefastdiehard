#pragma once

#include <SFML/Graphics/Color.hpp>
#include <filesystem>

#include "base/fixed_string.hpp"
#include "base/log.hpp"
#include "base/cfg.hpp"
#include "base/color.hpp"
#include "weapon.hpp"

namespace dfdh {
class player_configurator {
public:
    player_configurator(const player_name_t& player_name): name(player_name) {
        read();
    }

    ~player_configurator() {
        if (write_on_delete)
            write();
    }

    void read() {
        auto cfgpath = "data/settings/players_settings.cfg"s;
        auto conf = cfg(cfgpath, cfg_mode::create_if_not_exists | cfg_mode::commit_at_destroy | cfg_mode::autocreate_dir);

        auto  sect_name = cfg_section_name{"player_settings_" + std::string(name)};
        auto& sect      = conf.get_or_create(sect_name);

        pistol = sect.value_or_default_and_set("pistol", std::string(weapon::default_secondary));
        /* Only a secondary gun can be configured */
        if (!weapon::is_secondary_section(pistol)) {
            glog().warn("player {}: {} is not a secondary weapon, replaced by {}", name, pistol, weapon::default_secondary);
            pistol = std::string(weapon::default_secondary);
            sect.set("pistol", pistol);
        }

        face_id    = sect.value_or_default_and_set<u32>("face_id", 0);
        body_id    = sect.value_or_default_and_set<u32>("body_id", 0);
        /* XXX: write better color serialization */
        body_color = rgba_t::from(
            sect.value_or_default_and_set<std::string>("body_color", rgba_t{sf::Color(255, 255, 255)}.to_string()));
        tracer_color = rgba_t::from(
            sect.value_or_default_and_set<std::string>("tracer_color", rgba_t{sf::Color(255, 255, 255)}.to_string()));
    }

    void write() {
        auto  cfgpath   = "data/settings/players_settings.cfg"s;
        auto  sect_name = "player_settings_" + std::string(name);
        auto  conf      = cfg(cfgpath, cfg_mode::create_if_not_exists | cfg_mode::commit_at_destroy | cfg_mode::autocreate_dir);
        auto& sect      = conf.get_or_create(sect_name);

        sect.set("pistol", pistol);
        sect.set("face_id", face_id);
        sect.set("body_id", body_id);
        sect.set("body_color", dfdh::format("{}", body_color));
        sect.set("tracer_color", dfdh::format("{}", tracer_color));
    }

    [[nodiscard]]
    std::string face_texture_path() const {
        return "player/face" + std::to_string(face_id) + ".png";
    }

    [[nodiscard]]
    std::string body_texture_path() const {
        return "player/body" + std::to_string(body_id) + ".png";
    }

private:
    static bool number_str(const char* str, size_t sz) {
        for (size_t i = 0; i < sz; ++i)
            if (str[i] < '0' || str[i] > '9')
                return false;
        return true;
    }

public:
    static u32 texture_path_to_index(std::string_view path) {
        if (!path.ends_with(".png"))
            return 0;

        path = std::string_view(path.data(), path.size() - 4);
        if (path.empty())
            return 0;

        size_t idx = path.size();
        while (idx != 0 && isdigit(path[idx - 1])) {
            --idx;
        }

        return u32(std::stoul(std::string(path.substr(idx))));
    }

    static std::vector<std::string> available_face_textures() {
        namespace fs = std::filesystem;
        std::vector<std::string> res;

        for (auto& f : fs::directory_iterator("data/textures/player/")) {
            if (f.is_regular_file()) {
                auto fname = f.path().filename().string();
                if (fname.starts_with("face") && fname.ends_with(".png") &&
                    number_str(fname.data() + 4, fname.size() - 8)) {
                    res.push_back(f.path().string().substr(sizeof("data/textures/") - 1));
                }
            }
        }

        return res;
    }

    static std::vector<std::string> available_body_textures() {
        namespace fs = std::filesystem;
        std::vector<std::string> res;

        for (auto& f : fs::directory_iterator("data/textures/player/")) {
            if (f.is_regular_file()) {
                auto fname = f.path().filename().string();
                if (fname.starts_with("body") && fname.ends_with(".png") &&
                    number_str(fname.data() + 4, fname.size() - 8)) {
                    res.push_back(f.path().string().substr(sizeof("data/textures/") - 1));
                }
            }
        }

        return res;
    }

public:
    player_name_t name         = "";
    std::string   pistol       = std::string(weapon::default_secondary);
    u32           face_id      = 0;
    u32           body_id      = 0;
    sf::Color     body_color   = {255, 255, 255};
    sf::Color     tracer_color = {255, 255, 255};

    bool write_on_delete = true;
};
}
