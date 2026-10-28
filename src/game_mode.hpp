#pragma once

#include <string>
#include <vector>

#include "base/cfg.hpp"
#include "base/types.hpp"
#include "base/vec_math.hpp"

namespace dfdh {

/**
 * Game mode: a set of game rules, stored in a gm_ config section
 * (data/game_mode.cfg). The server sends the current one to its clients
 */
struct game_mode {
    /* What the player score counts */
    enum class score_system_t : u8 {
        kills = 0, /* "default": +1 for a kill, -1 for a suicide */
        lives,     /* "lives": starts at start_lives, -1 for any death, a player with no lives is out of the game */
    };

    static constexpr std::string_view section_prefix = "gm_";
    static constexpr std::string_view default_section = "gm_default";

    std::string section;
    float       bullet_gravity    = 0.f; /* bullet gravity y value, 0 = off */
    float       crate_drop_period = 0.f; /* crate drop cooldown period (seconds), 0 = crates never drop */
    u32         crate_limit       = 0;   /* max crates alive at once, 0 = unlimited */
    u32         magazines_count   = 0;   /* magazines of a gun (pistols are unlimited), 0 = unlimited */
    score_system_t score_system   = score_system_t::kills;
    i32         start_lives       = 3;   /* lives score system: the starting score */

    bool operator==(const game_mode& gm) const {
        return section == gm.section && exactly_equal(bullet_gravity, gm.bullet_gravity) &&
               exactly_equal(crate_drop_period, gm.crate_drop_period) && crate_limit == gm.crate_limit &&
               magazines_count == gm.magazines_count &&
               score_system == gm.score_system && start_lives == gm.start_lives;
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
        gm.crate_limit       = u32(std::max(sect.value_or_default("crate_limit", 0), 0));
        gm.magazines_count   = u32(std::max(sect.value_or_default("magazines_count", 0), 0));
        gm.score_system      = score_system_from_string(sect.value_or_default("score_system", std::string("default")));
        gm.start_lives       = std::max(sect.value_or_default("start_lives", 3), 1);
        return gm;
    }

    /* Throws cfg_exception on an unknown score system */
    static score_system_t score_system_from_string(const std::string& name) {
        if (name == "default")
            return score_system_t::kills;
        if (name == "lives")
            return score_system_t::lives;
        throw cfg_exception("unknown score_system \"" + name + "\" (expected default or lives)");
    }

    static std::string_view score_system_name(score_system_t value) {
        return value == score_system_t::lives ? "lives" : "default";
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

    [[nodiscard]]
    bool crate_limit_reached(size_t crate_count) const {
        return crate_limit != 0 && crate_count >= crate_limit;
    }

    [[nodiscard]]
    bool lives_enabled() const {
        return score_system == score_system_t::lives;
    }

    /* The score of a player on start (and after a game mode switch) */
    [[nodiscard]]
    i32 start_score() const {
        return lives_enabled() ? start_lives : 0;
    }

    /* The score rules differ: the scores restart */
    [[nodiscard]]
    bool same_score_rules(const game_mode& gm) const {
        return section == gm.section && score_system == gm.score_system && start_lives == gm.start_lives;
    }
};

}
