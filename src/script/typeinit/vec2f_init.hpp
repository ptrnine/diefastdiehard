#pragma once

#include "usertypes.hpp"
#include <luaffi/luacpp_ctx.hpp>

namespace dfdh
{
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
                              [](const vec2f& v) { return dfdh::format("{}", v); });
}
} // namespace dfdh
