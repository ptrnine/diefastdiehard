#pragma once

#include <string>
#include <vector>

#include "base/cfg.hpp"
#include "base/vec_math.hpp"

namespace dfdh {

/**
 * Game mode: a set of game rules, stored in a gm_ config section
 * (data/game_mode.cfg). The server sends the current one to its clients
 */
struct game_mode {
    static constexpr std::string_view section_prefix = "gm_";
    static constexpr std::string_view default_section = "gm_default";

    std::string section;
    float       bullet_gravity    = 0.f; /* bullet gravity y value, 0 = off */
    float       crate_drop_period = 0.f; /* crate drop cooldown period (seconds), 0 = crates never drop */

    bool operator==(const game_mode& gm) const {
        return section == gm.section && exactly_equal(bullet_gravity, gm.bullet_gravity) &&
               exactly_equal(crate_drop_period, gm.crate_drop_period);
    }

    /* Accepts the name with or without the gm_ prefix ("default" -> "gm_default") */
    static std::string section_name(const std::string& name) {
        return name.starts_with(section_prefix) ? name : std::string(section_prefix) + name;
    }

    /* Throws cfg_exception if the section is not found */
    static game_mode load(const std::string& name) {
        auto  sect_name = section_name(name);
        auto& sect      = cfg::global().get_section(sect_name);

        game_mode gm;
        gm.section           = sect_name;
        gm.bullet_gravity    = sect.value_or_default("bullet_gravity", 0.f);
        gm.crate_drop_period = std::max(sect.value_or_default("crate_drop_period", 0.f), 0.f);
        return gm;
    }

    static std::vector<std::string> list_available() {
        std::vector<std::string> res;
        for (auto& [sect, _] : cfg::global().get_sections())
            if (sect.starts_with(section_prefix))
                res.push_back(sect);
        return res;
    }

    [[nodiscard]]
    bool bullet_gravity_enabled() const {
        return !exactly_equal(bullet_gravity, 0.f);
    }

    [[nodiscard]]
    bool crates_enabled() const {
        return crate_drop_period > 0.f;
    }
};

}
