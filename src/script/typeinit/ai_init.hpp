#pragma once

#include "usertypes.hpp"
#include <luaffi/luacpp_ctx.hpp>

namespace dfdh
{

inline void lua_ai_init(luacpp::luactx& ctx) {
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "number"});
    ctx.set_member_table(luacpp::member_table<ai_player_t>{
        lua_getez(acceleration),
        lua_getez(available_jumps),
        lua_getez(barrel_pos),
        lua_getez(dir),
        lua_getez(group),
        lua_getez(gun_bullet_vel),
        lua_getez(gun_dispersion),
        lua_getez(gun_fire_rate),
        lua_getez(gun_hit_power),
        lua_getez(gun_mag_elapsed),
        lua_getez(gun_mag_size),
        lua_getez(is_walking),
        lua_getez(is_y_locked),
        lua_getez(jump_speed),
        lua_getez(long_shot_dir),
        lua_getez(long_shot_enabled),
        lua_getez(max_jump_dist),
        lua_getez(max_vel_x),
        lua_getez(name),
        lua_getez(on_left),
        lua_getez(pos),
        lua_getez(size),
        lua_getez(vel),
        lua_getez(x_accel),
        lua_getez(x_slowdown),
    });
    ctx.provide_member<ai_player_t>(LUA_TNAME("__tostring"), [](const ai_player_t&) {
        std::string res;
        /* XXX: implement me */
        return res;
    });

    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "number"});
    ctx.set_member_table(luacpp::member_table<ai_physic_sim_t>{
        lua_getez(bullet_gravity),
        lua_getez(enable_gravity_for_bullets),
        lua_getez(gravity),
        lua_getez(last_rps),
        lua_getez(time_speed),
    });

    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "number"});
    ctx.set_member_table(luacpp::member_table<ai_level_t>{
        lua_getez(level_size),
        lua_getez(platforms_bound_end_x),
        lua_getez(platforms_bound_start_x),
    });

    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.set_member_table(luacpp::member_table<ai_bullet_t>{
        lua_getez(group),
        lua_getez(hit_mass),
        lua_getez(pos),
        lua_getez(vel),
    });

    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.set_member_table(luacpp::member_table<ai_platform_t>{lua_getez(pos1), lua_getez(pos2)});


    ctx.annotate({.explicit_type = "table<integer, ai_bullet_t>"});
    ctx.annotate({.explicit_type = "ai_level_t"});
    ctx.annotate({.explicit_type = "ai_physic_sim_t"});
    ctx.annotate({.explicit_type = "table<integer, table<integer, vec2f>>"});
    ctx.annotate({.explicit_type = "table<integer, ai_platform_t>"});
    ctx.annotate({.explicit_type = "table<string, ai_player_t>"});
    ctx.set_member_table(luacpp::member_table<ai_data_t>{
        lua_getez(bullets),
        lua_getez(level),
        lua_getez(physic_sim),
        lua_getez(platform_map),
        lua_getez(platforms),
        lua_getez(players),
    });

    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "string"});
    using ai_operator_base_p = ai_operator_base*;
    ctx.set_member_table(
        luacpp::member_table<ai_operator_base_p>{{"player_name", {[](const ai_operator_base_p& o, luacpp::luactx& ctx) {
                                                      ctx.push(o->player_name());
                                                  }}},
                                                 {"difficulty", {[](const ai_operator_base_p& o, luacpp::luactx& ctx) {
                                                      ctx.push(o->difficulty());
                                                  }}}

        });

    ctx.provide_member<ai_operator_base*>(LUA_TNAME("produce_action"), [](ai_operator_base_p& o, int action) {
        if (action < 0 || action >= int(ai_action::COUNT)) {
            glog().error("lua ai operator: invalid action with index {}", action);
            return;
        }
        o->produce_action(ai_action(action));
    });
}

} // namespace dfdh
