local log = require("mod/log")

cmd [[
    level current lvl_aes
    sound volume 10

    player create 'name=lol group=1'
    player controller0 lol
]]

--    player wpn lol wpn_416
--    ai bind lol
--    ai difficulty lol insane
--
--    player create mda
--    player wpn mda wpn_416
--    ai bind mda
--    ai difficulty mda hard


--[[
    ai bind mda
    ai difficulty mda easy

    player create "name=mda1 group=0"
    player wpn mda1 wpn_m40
    ai bind mda1
    ai difficulty mda1 easy

    player create "name=mda2 group=0"
    player wpn mda2 wpn_vssk
    ai bind mda2
    ai difficulty mda2 easy
]]

--[[
    player create "name=mda3 group=1"
    player wpn mda3 wpn_vss
    ai bind mda3
    ai difficulty mda3 hard
]]

GS.pause = false

--[[

G = {}

G.game_update = function()
end

G.handle_event = function(evt)
    if evt.type == "KeyPressed" and evt.keycode == "G" then
        log("info", "%s", Cfg)
    end
end
--]]
