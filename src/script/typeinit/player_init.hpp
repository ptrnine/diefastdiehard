#pragma once

#include "usertypes.hpp"
#include <luaffi/luacpp_ctx.hpp>

namespace dfdh
{
inline void lua_player_init(luacpp::luactx& ctx) {
    using player_p = player*;

    /* member table names are sorted (std::map): annotates go in the same order */
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "table<integer, number>"});
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "integer"});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.annotate({.explicit_type = "table<integer, number>"});
    ctx.annotate({.explicit_type = "vec2f"});
    ctx.annotate({.explicit_type = "number"});
    ctx.annotate({.explicit_type = "number"});

    ctx.set_member_table(luacpp::member_table<player_p>{
        {"attacker",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->attacker()); }}},
        {"available_jumps",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_available_jumps()); }}},
        {"barrel_pos",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->barrel_pos()); }}},
        {"body_color",
         {[](const player_p& p, luacpp::luactx& ctx) {
             auto c = p->body_color();
             ctx.push(std::tuple<float, float, float, float>{c.r, c.g, c.b, float(c.a)});
         }}},
        {"body_texture_path",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->body_texture_path()); }}},
        {"deaths",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_deaths()); }}},
        {"eliminated",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->eliminated()); }}},
        {"face_texture_path",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->face_texture_path()); }}},
        {"facing_dir",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->facing_dir()); }}},
        {"group",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_group()); },
          [](player_p& p, luacpp::luactx& ctx) {
              int group;
              ctx.get_new(group);
              p->group(group);
          }}},
        {"hat_texture_path",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->hat_texture_path()); }}},
        {"head_texture_path",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->head_texture_path()); }}},
        {"holds_primary",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->holds_primary()); }}},
        {"holds_secondary",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->holds_secondary()); }}},
        {"is_walking",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->is_walking()); }}},
        {"jump_speed",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_jump_speed()); },
          [](player_p& p, luacpp::luactx& ctx) {
              float value;
              ctx.get_new(value);
              p->jump_speed(value);
          }}},
        {"long_shot_enabled",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->long_shot_enabled()); },
          [](player_p& p, luacpp::luactx& ctx) {
              bool value;
              ctx.get_new(value);
              p->enable_long_shot(value);
          }}},
        {"max_speed",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_max_speed()); },
          [](player_p& p, luacpp::luactx& ctx) {
              float value;
              ctx.get_new(value);
              p->max_speed(value);
          }}},
        {"name",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->name()); }}},
        {"on_left",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_on_left()); }}},
        {"pos",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_position()); },
          [](player_p& p, luacpp::luactx& ctx) {
              vec2f value;
              ctx.get_new(value);
              p->position(value);
          }}},
        {"pistol_section",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->pistol_section()); }}},
        {"score",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->score()); },
          [](player_p& p, luacpp::luactx& ctx) {
              i32 value;
              ctx.get_new(value);
              p->score(value);
          }}},
        {"secondary_section",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->secondary_section()); }}},
        {"size",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_size()); }}},
        {"teleports",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->teleports()); }}},
        {"throwing",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->throwing()); }}},
        {"tracer_color",
         {[](const player_p& p, luacpp::luactx& ctx) {
             auto c = p->tracer_color();
             ctx.push(std::tuple<float, float, float, float>{c.r, c.g, c.b, float(c.a)});
         },
          [](player_p& p, luacpp::luactx& ctx) {
              auto [r, g, b, a] = ctx.get_new<std::tuple<float, float, float, float>>();
              p->tracer_color({u8(r), u8(g), u8(b), u8(a)});
          }}},
        {"vel",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_velocity()); },
          [](player_p& p, luacpp::luactx& ctx) {
              vec2f value;
              ctx.get_new(value);
              p->velocity(value);
          }}},
        {"x_accel",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_x_accel()); },
          [](player_p& p, luacpp::luactx& ctx) {
              float value;
              ctx.get_new(value);
              p->x_accel(value);
          }}},
        {"x_slowdown",
         {[](const player_p& p, luacpp::luactx& ctx) { ctx.push(p->get_x_slowdown()); },
          [](player_p& p, luacpp::luactx& ctx) {
              float value;
              ctx.get_new(value);
              p->x_slowdown(value);
          }}},
    });

    ctx.provide_member<player_p>(LUA_TNAME("__tostring"), [](const player_p& p) {
        return dfdh::format("<player {}>", p->name());
    });

    ctx.annotate({.comment = "Move the player (the movement is consumed on the next sim tick)"});
    ctx.provide_member<player_p>(LUA_TNAME("move_left"), [](player_p& p) { p->move_left(); });
    ctx.annotate({.comment = "Move the player (the movement is consumed on the next sim tick)"});
    ctx.provide_member<player_p>(LUA_TNAME("move_right"), [](player_p& p) { p->move_right(); });
    ctx.annotate({.comment = "Stop moving"});
    ctx.provide_member<player_p>(LUA_TNAME("stop"), [](player_p& p) { p->stop(); });
    ctx.annotate({.comment = "Jump (grounded or an air jump if any is left)"});
    ctx.provide_member<player_p>(LUA_TNAME("jump"), [](player_p& p) { p->jump(); });
    ctx.annotate({.comment = "Fast fall while on the ground"});
    ctx.provide_member<player_p>(LUA_TNAME("jump_down"), [](player_p& p) { p->jump_down(); });

    ctx.annotate({.comment = "Pull the gun trigger, returns whether a shot was fired"});
    ctx.provide_member<player_p>(LUA_TNAME("shot"), [](player_p& p) { return p->shot(); });
    ctx.annotate({.comment = "Relax the gun trigger"});
    ctx.provide_member<player_p>(LUA_TNAME("relax"), [](player_p& p) { return p->relax(); });
    ctx.annotate({.comment = "Throw the held gun away, returns whether the throw started"});
    ctx.provide_member<player_p>(LUA_TNAME("drop_gun"), [](player_p& p) { return p->drop_gun(); });

    ctx.annotate({.comment = "Apply an impulse (velocity change, px/s)"});
    ctx.provide_member<player_p>(LUA_TNAME("apply_impulse"), [](player_p& p, const vec2f& impulse) {
        p->apply_impulse(impulse);
    });
    ctx.annotate({.comment = "Instantly move the player (stops it, resets the position trace)"});
    ctx.provide_member<player_p>(LUA_TNAME("teleport"), [](player_p& p, const vec2f& pos) {
        p->teleport(pos);
    });
    ctx.annotate({.comment = "Refill the air jumps"});
    ctx.provide_member<player_p>(LUA_TNAME("enable_double_jump"), [](player_p& p) { p->enable_double_jump(); });
    ctx.annotate({.comment = "Forget the last player who hit this one"});
    ctx.provide_member<player_p>(LUA_TNAME("clear_attacker"), [](player_p& p) { p->clear_attacker(); });
    ctx.annotate({.comment = "Add to the player's score"});
    ctx.provide_member<player_p>(LUA_TNAME("add_score"), [](player_p& p, i32 value) { p->add_score(value); });

    ctx.annotate({.comment = "Equip a gun from a weapon section (data/cfg/wpn), magazines_count is the ammo limit"});
    ctx.provide_member<player_p>(LUA_TNAME("setup_gun"), [](player_p& p, const std::string& section, u32 magazines_count) {
        p->setup_gun(section, magazines_count);
    },
                           [](player_p& p, const std::string& section) { p->setup_gun(section); });
    ctx.annotate({.comment = "Remove the gun from the hands"});
    ctx.provide_member<player_p>(LUA_TNAME("remove_gun"), [](player_p& p) { p->remove_gun(); });
}
} // namespace dfdh
