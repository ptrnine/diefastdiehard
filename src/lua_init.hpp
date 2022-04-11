#pragma once

#include "vec2.hpp"
#include "vec_math.hpp"
#include "ai_types.hpp"
#include "log.hpp"
#include "cfg.hpp"
#include "game_state.hpp"
#include "event_type_generated.hpp"

#include <luaffi/luacpp_basic.hpp>

#define lua_p_getsetez(lua_or_cpp_name)                                                                                \
    {                                                                                                                  \
        #lua_or_cpp_name, {                                                                                            \
            [](auto v, auto ctx) { ctx.push(v->lua_or_cpp_name); }, [](auto v, auto ctx) {                             \
                ctx.get_new(v->lua_or_cpp_name);                                                                       \
            }                                                                                                          \
        }                                                                                                              \
    }

#define lua_p_getez(lua_or_cpp_name)                                                                                   \
    {                                                                                                                  \
        #lua_or_cpp_name, {                                                                                            \
            [](auto v, auto ctx) {                                                                                     \
                ctx.push(v->lua_or_cpp_name);                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    }

template <>
struct luacpp::typespec_list_s<0> {
    using type = std::tuple<typespec<dfdh::vec2f, LUA_TNAME("vec2f")>,
                            typespec<dfdh::player_name_t, LUA_TNAME("player_name_t")>,
                            typespec<dfdh::ai_player_t, LUA_TNAME("ai_player")>,
                            typespec<dfdh::ai_physic_sim_t, LUA_TNAME("ai_physic_sim")>,
                            typespec<dfdh::ai_level_t, LUA_TNAME("ai_level")>,
                            typespec<dfdh::ai_bullet_t, LUA_TNAME("ai_bullet")>,
                            typespec<dfdh::ai_platform_t, LUA_TNAME("ai_platform")>,
                            typespec<dfdh::ai_data_t, LUA_TNAME("ai_data")>,
                            typespec<dfdh::game_state*, LUA_TNAME("game_state")>,
                            typespec<sf::Event, LUA_TNAME("sfml_event")>,
                            typespec<dfdh::cfg*, LUA_TNAME("cfg_ref")>,
                            typespec<dfdh::cfg, LUA_TNAME("cfg_t")>,
                            typespec<dfdh::cfg_section<false>*, LUA_TNAME("cfg_section_ref")>,
                            typespec<dfdh::cfg_value<std::string, false>, LUA_TNAME("cfg_value_t")>>;
};

#include <luaffi/luacpp_ctx.hpp>

#include "lua.hpp"

namespace dfdh
{

inline luacpp::explicit_return lua_return_cfg_value(luacpp::luactx& ctx, cfg_value<std::string, false>& v) {
    auto str = v.value();

    if (!v.has_value())
        return {ctx, nullptr};

    if (str == "true" || str == "on")
        return {ctx, true};
    if (str == "false" || str == "off")
        return {ctx, false};

    if (!isdigit(str.front()) && str.front() != '-' && str.front() != '+')
        return {ctx, str};

    if (str.find_first_of(" \t") == std::string::npos) {
        try {
            return {ctx, ston<double>(str)};
        }
        catch (...) {
        }
    }

    try {
        if (str.find(' ') != std::string::npos || str.find('\t') != std::string::npos) {
            auto splits = str / split(' ', '\t');
            auto first  = splits.begin();
            auto second = std::next(splits.begin());
            return {ctx,
                    vec2f(ston<float>({(*first).begin(), (*first).end()}),
                          ston<float>({(*second).begin(), (*second).end()}))};
        }
    }
    catch (...) {
    }

    return {ctx, str};
}

inline void lua_cfg_init(luacpp::luactx& ctx) {
    using cfg_ptr_t = cfg*;
    ctx.annotate_args("config", "section_name");
    ctx.provide_member<cfg*>(LUA_TNAME("get_section"), [](const cfg_ptr_t& conf, const std::string& section_name) {
        return conf->try_get_section({section_name});
    });
    ctx.annotate_args("config", "section_name");
    ctx.provide_member<cfg>(LUA_TNAME("get_section"), [](cfg& conf, const std::string& section_name) {
        return conf.try_get_section({section_name});
    });
    ctx.provide_member<cfg*>(LUA_TNAME("__tostring"), [](const cfg_ptr_t& conf) { return format("{}", *conf); });
    ctx.provide_member<cfg>(LUA_TNAME("__tostring"), [](const cfg& conf) { return format("{}", conf); });
    ctx.set_member_table(luacpp::member_table<cfg_ptr_t>{
        {"readonly",
         {[](const cfg_ptr_t& conf, luacpp::luactx& ctx) { ctx.push(conf->is_readonly()); },
          [](cfg_ptr_t& conf, luacpp::luactx& ctx) {
              bool v;
              ctx.get_new(v);
              conf->set_readonly(v);
          }}}});
    ctx.set_member_table(
        luacpp::member_table<cfg>{{"readonly",
                                   {[](const cfg& conf, luacpp::luactx& ctx) { ctx.push(conf.is_readonly()); },
                                    [](cfg& conf, luacpp::luactx& ctx) {
                                        bool v;
                                        ctx.get_new(v);
                                        conf.set_readonly(v);
                                    }}}});
    ctx.annotate_args("config_path");
    ctx.provide_member<cfg>(LUA_TNAME("open"), [](const std::string& path) { return cfg(path); });

    ctx.annotate_args("config_path");
    ctx.provide_member<cfg>(LUA_TNAME("new"),
                            [](const std::string& path) { return cfg(path, cfg_mode::create_if_not_exists); });
    ctx.provide_member<cfg>(LUA_TNAME("commit"), [](cfg& conf) { conf.commit(); });

    ctx.annotate_args("config", "section_name", "preffered_file_path", "insert_mode");
    ctx.provide_member<cfg>(
        LUA_TNAME("section"),
        [](cfg& conf, const std::string& section_name) { return &conf.get_or_create(section_name); },
        [&ctx](cfg& conf, const std::string& section_name, const std::string& preffered_file_path) {
            if (preffered_file_path == "at_the_start")
                return luacpp::explicit_return(ctx,
                                               &conf.get_or_create(section_name, {}, cfg::insert_mode::at_the_start));
            else if (preffered_file_path == "at_the_end")
                return luacpp::explicit_return(ctx,
                                               &conf.get_or_create(section_name, {}, cfg::insert_mode::at_the_end));
            else if (preffered_file_path == "lexicographicaly")
                return luacpp::explicit_return(
                    ctx, &conf.get_or_create(section_name, {}, cfg::insert_mode::lexicographicaly));
            else {
                try {
                    return luacpp::explicit_return(ctx, &conf.get_or_create(section_name, preffered_file_path));
                }
                catch (const cfg_not_found&) {
                    return luacpp::explicit_return(ctx, nullptr, "cfg not found");
                }
            }
        },
        [&ctx](cfg&               conf,
               const std::string& section_name,
               const std::string& preffered_file_path,
               const std::string& insert_mode) {
            auto im = cfg::insert_mode::lexicographicaly;
            if (insert_mode == "at_the_start")
                im = cfg::insert_mode::at_the_start;
            else if (insert_mode == "at_the_end")
                im = cfg::insert_mode::at_the_end;

            try {
                return luacpp::explicit_return(ctx, &conf.get_or_create(section_name, preffered_file_path, im));
            }
            catch (const cfg_not_found&) {
                return luacpp::explicit_return(ctx, nullptr, "cfg not found");
            }
        });

    static constexpr auto list_sections_f = [](cfg_ptr_t conf) {
        return conf->list_sections();
    };
    ctx.annotate_args("config");
    ctx.provide_member<cfg_ptr_t>(LUA_TNAME("list_sections"), list_sections_f);
    ctx.annotate_args("config");
    ctx.provide_member<cfg>(LUA_TNAME("list_sections"), list_sections_f);

    static constexpr auto has_section_f = [](cfg_ptr_t conf, const std::string& section_name) {
        return conf->has_section(section_name);
    };
    ctx.annotate_args("config", "section_name");
    ctx.provide_member<cfg_ptr_t>(LUA_TNAME("has_section"), has_section_f);
    ctx.annotate_args("config", "section_name");
    ctx.provide_member<cfg>(LUA_TNAME("has_section"), has_section_f);

    using section_ptr_t = cfg_section<false>*;
    ctx.provide_member<cfg_section<false>*>(LUA_TNAME("value"), [&](const section_ptr_t& sect, const std::string& key) {
        if (auto value = sect->try_get<std::string>(key))
            return lua_return_cfg_value(ctx, *value);
        return luacpp::explicit_return{0};
    });

    ctx.provide(LUA_TNAME("Cfg"), &cfg::mutable_global());
}

inline void lua_vec2f_init(luacpp::luactx& ctx) {
    ctx.set_member_table(luacpp::member_table<vec2f>{lua_getsetez(x), lua_getsetez(y)});
    ctx.provide(
        LUA_TNAME("vec2f.new"),
        [] { return vec2f(0); },
        [](const vec2f& v) { return v; }, // deep-copy
        [](double v) { return vec2f(float(v)); },
        [](double x, double y) { return vec2f(float(x), float(y)); });

    ctx.provide(LUA_TNAME("__add"), static_cast<vec2f (vec2f::*)(const vec2f&) const>(&vec2f::operator+));
    ctx.provide(LUA_TNAME("__sub"), static_cast<vec2f (vec2f::*)(const vec2f&) const>(&vec2f::operator-));
    ctx.provide(LUA_TNAME("__mul"), &vec2f::operator*);
    ctx.provide(LUA_TNAME("__div"), &vec2f::operator/);
    ctx.provide_member<vec2f>(LUA_TNAME("magnitude"), [](const vec2f& v) { return magnitude(v); });
    ctx.provide_member<vec2f>(LUA_TNAME("__tostring"),
                              [](const vec2f& v) { return std::to_string(v.x) + " " + std::to_string(v.y); });
}

inline void lua_player_name_init(luacpp::luactx& ctx) {
    ctx.provide_member<player_name_t>(
        LUA_TNAME("new"), [] { return player_name_t{}; }, [](const std::string& str) { return player_name_t{str}; });
    ctx.provide_member<player_name_t>(LUA_TNAME("__tostring"), [](const player_name_t& v) { return std::string(v); });
}

inline void lua_ai_instance_init(luacpp::luactx& ctx) {
    lua_vec2f_init(ctx);
    lua_player_name_init(ctx);

    ctx.set_member_table(luacpp::member_table<ai_player_t>{lua_getez(pos),
                                                           lua_getez(dir),
                                                           lua_getez(size),
                                                           lua_getez(vel),
                                                           lua_getez(barrel_pos),
                                                           lua_getez(name),
                                                           lua_getez(available_jumps),
                                                           lua_getez(x_accel),
                                                           lua_getez(x_slowdown),
                                                           lua_getez(jump_speed),
                                                           lua_getez(max_jump_dist),
                                                           lua_getez(max_vel_x),
                                                           lua_getez(gun_dispersion),
                                                           lua_getez(group),
                                                           lua_getez(gun_bullet_vel),
                                                           lua_getez(gun_fire_rate),
                                                           lua_getez(on_left),
                                                           lua_getez(is_y_locked)});

    ctx.set_member_table(
        luacpp::member_table<ai_physic_sim_t>{lua_getez(gravity), lua_getez(time_speed), lua_getez(last_rps)});

    ctx.set_member_table(luacpp::member_table<ai_level_t>{lua_getez(level_size)});

    ctx.set_member_table(
        luacpp::member_table<ai_bullet_t>{lua_getez(pos), lua_getez(vel), lua_getez(hit_mass), lua_getez(group)});

    ctx.set_member_table(luacpp::member_table<ai_platform_t>{lua_getez(pos1), lua_getez(pos2)});

    ctx.set_member_table(luacpp::member_table<ai_data_t>{
        lua_getez(players),
        lua_getez(bullets),
        lua_getez(physic_sim),
        lua_getez(platforms),
        lua_getez(platform_map),
        lua_getez(level),
    });
}

inline void lua_init(luacpp::luactx& ctx) {
    lua_vec2f_init(ctx);
    lua_player_name_init(ctx);

    ctx.annotate_args("level", "message");
    ctx.provide(LUA_TNAME("log"), [](const std::string& level, const std::string& str) {
        if (level.starts_with("warn")) {
            if (level.find("upd") != std::string::npos)
                LOG_WARN_UPDATE("{}", str);
            else
                LOG_WARN("{}", str);
        }
        else if (level.starts_with("err")) {
            if (level.find("upd") != std::string::npos)
                LOG_ERR_UPDATE("{}", str);
            else
                LOG_ERR("{}", str);
        }
        else {
            if (level.find("upd") != std::string::npos)
                LOG_INFO_UPDATE("{}", str);
            else
                LOG_INFO("{}", str);
        }
    });

    /* Game state */
    using game_state_p = game_state*;
    ctx.set_member_table(
        luacpp::member_table<game_state*>{lua_p_getsetez(game_speed),
                                         {"pause",
                                          {[](const game_state_p& g, luacpp::luactx& ctx) { ctx.push(!g->on_game); },
                                           [](game_state_p& g, luacpp::luactx& ctx) {
                                               bool pause;
                                               ctx.get_new(pause);
                                               g->game_run(!pause);
                                           }}},
                                         lua_p_getsetez(debug_physics),
                                         lua_p_getsetez(gravity_for_bullets),
                                         lua_p_getez(cam_pos)});

    /* SFML event */
    ctx.set_member_table(luacpp::member_table<sf::Event>{
        {"type",
         {[](const sf::Event& e, luacpp::luactx& ctx) { ctx.push(sfml_event_type_to_str(e.type)); },
          [](sf::Event& e, luacpp::luactx& ctx) {
              std::string event_type;
              ctx.get_new(event_type);
              e.type = sfml_str_to_event_type(event_type);
          }}},
        {"keycode",
         {[](const sf::Event& e, luacpp::luactx& ctx) { ctx.push(sfml_key_to_str(e.key.code)); },
          [](sf::Event& e, luacpp::luactx& ctx) {
              std::string key;
              ctx.get_new(key);
              e.key.code = sfml_str_to_key(key);
          }}},
    });

    lua_cfg_init(ctx);
}

void luactx_mgr::load() {
    std::string file_name = "dfdh";
    std::string assist_file_name = "__assist";
    if (_instance_name == "ai") {
        file_name += "_ai";
        assist_file_name += "_ai";
    }

    try {
        auto path = fs::current_path() / "data/scripts";
        _ctx->load_and_call((path / (file_name + ".lua")).string().data());
        loaded = true;

        auto ofd = std::ofstream(path / (assist_file_name + ".lua"));
        ofd << _ctx->generate_assist();
    }
    catch (const std::exception& e) {
        LOG_ERR("lua: {}", e.what());
        loaded = false;
    }
}

luactx_mgr::luactx_mgr(std::string instance_name):
    _ctx(std::make_unique<luacpp::luactx>(true)), _instance_name(std::move(instance_name)) {
    if (_instance_name == "global")
        lua_init(*_ctx);
    else if (_instance_name == "ai")
        lua_ai_instance_init(*_ctx);
    else
        throw std::runtime_error("Invalid lua instance name: " + _instance_name);
}

template <typename NameT, typename... Ts>
auto luactx_mgr::try_call_proc(bool game_loop, NameT name, Ts&&... args) {
    if (game_loop) {
        auto now               = std::chrono::steady_clock::now();
        auto [pos, was_insert] = call_history.emplace(std::string_view(NameT{}),
                                                      failed_info{std::chrono::steady_clock::time_point{}, false});
        if (!was_insert && now - pos->second.time < std::chrono::seconds(1))
            return;
    }

    try {
        _ctx->extract<void(Ts...)>(name)(std::forward<Ts>(args)...);
        if (game_loop)
            call_history.at(std::string_view(NameT{})).failed = false;
    }
    catch (const std::exception& e) {
        if (game_loop) {
            auto& info = call_history.at(std::string_view(NameT{}));
            info.time = std::chrono::steady_clock::now();
            if (!info.failed)
                LOG_ERR("lua error: {}", e.what());
            info.failed = true;
        }
    }
}

void luactx_mgr::execute_line(const std::string& line) {
    try {
        _ctx->load_and_call(luacpp::lua_code{line});
    }
    catch (const std::exception& e) {
        LOG_ERR("lua error: {}", e.what());
    }
}

} // namespace dfdh
