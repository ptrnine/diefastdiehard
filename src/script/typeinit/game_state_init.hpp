#pragma once

#include "usertypes.hpp"
#include <luaffi/luacpp_ctx.hpp>

namespace dfdh
{
inline void lua_game_state_init(luacpp::luactx& ctx) {
    using game_state_p = game_state*;

    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.set_member_table(luacpp::member_table<game_state*>{
        lua_p_getez(cam_pos),
        lua_p_getsetez(debug_physics),
        lua_p_getsetez(game_speed),
        lua_p_getsetez(gravity_for_bullets),
        {"pause",
         {[](const game_state_p& g, luacpp::luactx& ctx) { ctx.push(!g->on_game); },
          [](game_state_p& g, luacpp::luactx& ctx) {
              bool pause;
              ctx.get_new(pause);
              g->game_run(!pause);
          }}},
    });

    ctx.annotate({.comment = "List of all player names", .explicit_type = "table<integer, string>"});
    ctx.provide_member<game_state_p>(LUA_TNAME("list_players"), [](const game_state_p& g) {
        std::vector<std::string> res;
        for (auto& [name, _] : g->players) res.push_back(std::string(name));
        return res;
    });

    ctx.annotate({.comment = "Positions (bottom left) of the crates on the level as a flat list: x1, y1, x2, y2, ...",
                  .explicit_type = "table<integer, number>"});
    ctx.provide_member<game_state_p>(LUA_TNAME("list_crates"), [](const game_state_p& g) {
        std::vector<float> res;
        for (auto& c : g->crates) {
            auto pos = c->get_position();
            res.push_back(pos.x);
            res.push_back(pos.y);
        }
        return res;
    });

    ctx.annotate({.comment = "Get player by name, nil if not found",
                  .argument_names = {"name"},
                  .explicit_type = "player"});
    ctx.provide_member<game_state_p>(LUA_TNAME("get_player"), [](const game_state_p& g, const std::string& name) {
        auto found = g->players.find(player_name_t(name));
        return found != g->players.end() ? found->second.get() : nullptr;
    });

    ctx.annotate({.comment = "Create a player (spawned at a random spawn point, nil if the name is taken)",
                  .argument_names = {"name"},
                  .explicit_type = "player"});
    ctx.provide_member<game_state_p>(LUA_TNAME("create_player"), [](game_state_p& g, const std::string& name) {
        return g->player_create(player_name_t(name));
    });

    ctx.annotate({.comment = "Remove a player, returns false if not found",
                  .argument_names = {"name"},
                  .explicit_type = "boolean"});
    ctx.provide_member<game_state_p>(LUA_TNAME("remove_player"), [](game_state_p& g, const std::string& name) {
        return g->player_remove(player_name_t(name));
    });

    ctx.annotate({.comment = "Load and switch to the level, true on success",
                  .argument_names = {"level_name"},
                  .explicit_type = "boolean"});
    ctx.provide_member<game_state_p>(LUA_TNAME("set_level"),
        [](game_state_p& g) { return g->set_level(); },
        [](game_state_p& g, const std::string& name) { return g->set_level(name); });

    ctx.annotate({.comment = "List of all level names (the lvl_ sections of the level config)",
                  .explicit_type = "table<integer, string>"});
    ctx.provide_member<game_state_p>(LUA_TNAME("list_levels"), [](const game_state_p&) {
        return level::list_available_levels();
    });
}
} // namespace dfdh
