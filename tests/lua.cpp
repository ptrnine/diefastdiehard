/* The player class lua bindings smoke test: the script below is run through
 * the global lua instance against a real game_state, the way a user script
 * in data/scripts would use it. The script reports its verdict in the
 * __t_* globals, the C++ side asserts them
 */
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "game_state.hpp"
#include "script/lua.hpp"
#include "script/lua_init.hpp" /* the full luacpp context types */

using namespace dfdh;

static const char* const lua_player_script = R"LUA(
local log = require("mod/log")

local fails = {}
local function check(name, cond, value)
    if not cond then fails[#fails + 1] = name .. " = " .. tostring(value) end
end

__t_count      = -1
__t_score      = 0
__t_gun        = nil
__t_gun_after  = nil
__t_get        = false
__t_dup_is_nil = false
__t_removed    = false
__t_pos_is_num = false
__t_result     = "FAIL"

local names = GS:list_players()
check("list_players empty", #names == 0, #names)
__t_count = #names

local p = GS:create_player("luabot")
check("create_player", p ~= nil, p)
if not p then
    __t_result = table.concat(fails, "; ")
    return
end

check("name", p.name == "luabot", p.name)
check("group", p.group == 0, p.group)
check("score", p.score == 0, p.score)
check("deaths", p.deaths == 0, p.deaths)
check("pos", type(p.pos.x) == "number" and type(p.pos.y) == "number", tostring(p.pos))
__t_pos_is_num = type(p.pos.x) == "number"
check("size", p.size.x == 50 and p.size.y == 90, tostring(p.size))
check("on_left", p.on_left == false, p.on_left)
check("facing_dir", p.facing_dir.x == 1 and p.facing_dir.y == 0, tostring(p.facing_dir))
check("is_walking", p.is_walking == false, p.is_walking)
check("available_jumps", p.available_jumps == 1, p.available_jumps)
check("holds_secondary", p.holds_secondary == true, p.holds_secondary)
check("holds_primary", p.holds_primary == false, p.holds_primary)
check("throwing", p.throwing == false, p.throwing)
check("eliminated", p.eliminated == false, p.eliminated)
check("attacker empty", p.attacker == "", p.attacker)
check("teleports", p.teleports == 1, p.teleports) -- the spawn teleport counts
check("long_shot_enabled", p.long_shot_enabled == false, p.long_shot_enabled)
check("max_speed", p.max_speed == 280, p.max_speed)
check("jump_speed", p.jump_speed == 620, p.jump_speed)
check("x_accel", p.x_accel == 1550, p.x_accel)
check("x_slowdown", p.x_slowdown == 700, p.x_slowdown)

-- getters/setters
p.group = 2
p.max_speed = 400
p.long_shot_enabled = true
p.score = 7
p.tracer_color = {10, 20, 30, 255}
check("group set", p.group == 2, p.group)
check("max_speed set", p.max_speed == 400, p.max_speed)
check("score set", p.score == 7, p.score)
check("long_shot_enabled set", p.long_shot_enabled == true, p.long_shot_enabled)
check("tracer_color set", p.tracer_color[1] == 10 and p.tracer_color[2] == 20 and p.tracer_color[3] == 30
      and p.tracer_color[4] == 255, table.concat(p.tracer_color, ","))
check("body_color", type(p.body_color[1]) == "number", table.concat(p.body_color, ","))

-- actions
p:apply_impulse(vec2f.new(50, -100))
p:jump()
p:move_right()
p:stop()
p:relax()
p:clear_attacker()
p:add_score(3)
p:enable_double_jump()
check("add_score", p.score == 10, p.score)
__t_score = p.score
check("available_jumps after enable_double_jump", p.available_jumps == 1, p.available_jumps)

-- guns
p:setup_gun("wpn_ak9")
check("setup_gun", p.pistol_section == "wpn_ak9", p.pistol_section)
check("holds_primary after setup_gun", p.holds_primary == true, p.holds_primary)
__t_gun = p.pistol_section
p:remove_gun()
check("remove_gun", p.pistol_section == "", p.pistol_section)
__t_gun_after = p.pistol_section
p:shot() -- no gun: no error

-- GS accessors
local p2 = GS:get_player("luabot")
check("get_player", p2 ~= nil and p2.name == "luabot", tostring(p2))
__t_get = p2 ~= nil
check("get_player missing is nil", GS:get_player("no_such_bot") == nil, GS:get_player("no_such_bot"))
local dup = GS:create_player("luabot")
check("duplicate create_player is nil", dup == nil, dup)
__t_dup_is_nil = dup == nil

local removed = GS:remove_player("luabot")
check("remove_player", removed == true, removed)
__t_removed = removed
check("remove_player again is false", GS:remove_player("luabot") == false, GS:remove_player("luabot"))
__t_count = #GS:list_players()
check("list_players empty after remove", __t_count == 0, __t_count)

-- levels
local lvls = GS:list_levels()
check("list_levels", #lvls > 0, #lvls)
local has_aes = false
for i = 1, #lvls do
    if lvls[i] == "lvl_aes" then has_aes = true end
end
check("list_levels contains lvl_aes", has_aes, table.concat(lvls, ","))
check("set_level", GS:set_level("lvl_aes") == true, nil)
check("set_level unknown is false", GS:set_level("no_such_level") == false, nil)
check("set_level no-arg resets", GS:set_level() == true, nil)

if #fails > 0 then
    __t_result = table.concat(fails, "; ")
else
    __t_result = "OK"
end
log.info("lua player test result: %s", __t_result)
)LUA";

TEST_CASE("lua: player class bindings") {
    /* The tests run from a directory containing data/ and fs.cfg (the repo root or a dist) */
    REQUIRE(std::filesystem::exists(fs::current_path() / "data" / "scripts"));

    game_state gs;
    REQUIRE(gs.set_level("lvl_aes"));

    luactx_mgr lua = luactx_mgr::global(true);
    lua.ctx().provide(LUA_TNAME("GS"), &gs);
    lua.load();
    REQUIRE(lua);

    lua.execute_line(lua_player_script);

    auto& ctx = lua.ctx();
    CHECK(ctx.extract<std::string>(luacpp::lua_name{"__t_result"}) == "OK");
    CHECK(ctx.extract<u32>(luacpp::lua_name{"__t_count"}) == 0);
    CHECK(ctx.extract<i32>(luacpp::lua_name{"__t_score"}) == 10);
    CHECK(ctx.extract<std::string>(luacpp::lua_name{"__t_gun"}) == "wpn_ak9");
    CHECK(ctx.extract<std::string>(luacpp::lua_name{"__t_gun_after"}) == "");
    CHECK(ctx.extract<bool>(luacpp::lua_name{"__t_get"}));
    CHECK(ctx.extract<bool>(luacpp::lua_name{"__t_dup_is_nil"}));
    CHECK(ctx.extract<bool>(luacpp::lua_name{"__t_removed"}));
    CHECK(ctx.extract<bool>(luacpp::lua_name{"__t_pos_is_num"}));
}
