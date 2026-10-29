#pragma once

#include "base/vec2.hpp"
#include "base/vec_math.hpp"
#include "base/log.hpp"
#include "ai_types.hpp"
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

using lua_timer_t = std::chrono::steady_clock::time_point;

template <>
struct luacpp::typespec_list_s<0> {
    using type = std::tuple<typespec<dfdh::vec2f, LUA_TNAME("vec2f")>,
                            typespec<dfdh::ai_player_t, LUA_TNAME("ai_player_t")>,
                            typespec<dfdh::ai_physic_sim_t, LUA_TNAME("ai_physic_sim_t")>,
                            typespec<dfdh::ai_level_t, LUA_TNAME("ai_level_t")>,
                            typespec<dfdh::ai_bullet_t, LUA_TNAME("ai_bullet_t")>,
                            typespec<dfdh::ai_platform_t, LUA_TNAME("ai_platform_t")>,
                            typespec<dfdh::ai_data_t, LUA_TNAME("ai_data_t")>,
                            typespec<dfdh::ai_operator_base*, LUA_TNAME("ai_operator_t")>,
                            typespec<dfdh::player*, LUA_TNAME("player")>,
                            typespec<dfdh::game_state*, LUA_TNAME("game_state")>,
                            typespec<sf::Event, LUA_TNAME("sfml_event")>,
                            typespec<dfdh::cfg*, LUA_TNAME("cfg_ref")>,
                            typespec<dfdh::cfg, LUA_TNAME("cfg_t")>,
                            typespec<dfdh::cfg_section<false>*, LUA_TNAME("cfg_section_ref")>,
                            typespec<dfdh::cfg_value<std::string, false>, LUA_TNAME("cfg_value_t")>,
                            typespec<lua_timer_t, LUA_TNAME("timer")>>;
};

