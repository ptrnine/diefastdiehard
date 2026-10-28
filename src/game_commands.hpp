#pragma once

#include "base/ston.hpp"
#include "game_state.hpp"
#include "command_buffer.hpp"

namespace dfdh {

class game_commands {
public:
    game_commands(game_state& state): gs(state) {
        command_buffer().add_handler("player", &game_commands::cmd_player, this);
        command_buffer().add_handler("player delete", &game_commands::cmd_player_delete, this);
        command_buffer().add_handler("player wpn", &game_commands::cmd_player_wpn, this);
        command_buffer().add_handler("cfg", &game_commands::cmd_cfg, this);
        command_buffer().add_handler("cfg set", &game_commands::cmd_cfg_set, this);
        command_buffer().add_handler("cfg mode", &game_commands::cmd_cfg_mode, this);
        command_buffer().add_handler("cfg watch", &game_commands::cmd_cfg_watch, this);
        command_buffer().add_handler("cfg watch remove", &game_commands::cmd_cfg_watch_remove, this);
        command_buffer().add_handler("cfg watch list", &game_commands::cmd_cfg_watch_list, this);
        command_buffer().add_handler("cfg reload", &game_commands::cmd_cfg_reload, this);
        command_buffer().add_handler("cfg list", &game_commands::cmd_cfg_list, this);
        command_buffer().add_handler("cfg control", &game_commands::cmd_cfg_control, this);
        command_buffer().add_handler("help", &game_commands::cmd_help, this);
        command_buffer().add_handler("list", &game_commands::cmd_list, this);
        command_buffer().add_handler("game", &game_commands::cmd_game, this);
        command_buffer().add_handler("game speed", &game_commands::cmd_game_speed, this);
        command_buffer().add_handler("game mode", &game_commands::cmd_game_mode, this);
        command_buffer().add_handler("lobby", &game_commands::cmd_lobby, this);
        command_buffer().add_handler("level", &game_commands::cmd_level, this);
        command_buffer().add_handler("spawn", &game_commands::cmd_spawn, this);
        command_buffer().add_handler("ai", &game_commands::cmd_ai, this);
        command_buffer().add_handler("ai difficulty", &game_commands::cmd_ai_difficulty, this);
        command_buffer().add_handler("ai profiler", &game_commands::cmd_ai_profiler, this);
        command_buffer().add_handler("shutdown", &game_commands::cmd_shutdown, this);
        command_buffer().add_handler("sound volume", &game_commands::cmd_sound_volume, this);
        command_buffer().add_handler("efx reload", &game_commands::efx_reload, this);

        if (gs.lua_cmd_enabled)
            command_buffer().add_handler("lua", &game_commands::cmd_lua, this);
    }

    ~game_commands() {
        command_buffer().remove_handler("player");
        command_buffer().remove_handler("cfg");
        command_buffer().remove_handler("help");
        command_buffer().remove_handler("list");
        command_buffer().remove_handler("game");
        command_buffer().remove_handler("level");
        command_buffer().remove_handler("spawn");
        command_buffer().remove_handler("ai");
        command_buffer().remove_handler("connect");
        command_buffer().remove_handler("srv init");
        command_buffer().remove_handler("shutdown");
    }

    void cmd_help(const std::string& cmd) {
        std::string_view help;
        if (cmd == "player") {
            help = "available commands:\n"
                   "  player delete [player_name]                - delete player\n"
                   "  player reload [player_name]?               - reload player(s) configuration\n"
                   "  player controller[N] [player_name]         - binds specified controller to player\n"
                   "  player controller[N] delete                - deletes specified controller\n"
                   "  player conf [player_name]                  - open configuration ui for player\n"
                   "  player wpn [player_name] [wpn_section]?    - set/or show player weapon\n"
                   "  player create [player_name|player_params]  - create player\n"
                   "      player_params must be set in \"name=NAME group=INT\" format";
        }
        else if (cmd == "log") {
            help = "available commands:\n"
                   "  log time [on/off]  - enables or disables time showing\n"
                   "  log level [on/off] - enables or disables level showing\n"
                   "  log ring  [uint]   - setup log ring buffer size\n"
                   "  log clear          - clear devconsole log";
        }
        else if (cmd == "cfg") {
            help = "shows loaded configs\n"
                   "available commands:\n"
                   "  cfg [section_name] [key]?                    - show key/values for specified section\n"
                   "  cfg reload [weapons|levels]?                 - reload config users\n"
                   "  cfg list [prefix]?                           - show sections\n"
                   "  cfg set [section_name] [key] [value]?        - setup value for specified section\n"
                   "  cfg mode [readonly/readwrite] | [r/rw]       - setup config mode\n"
                   "  cfg watch [section_name]                     - watch section for changes and automatically\n"
                   "                                                 reload dependent entities\n"
                   "  cfg watch remove [section_name]              - remove section from watch list\n"
                   "  cfg watch list                               - list watched sections\n"
                   "  cfg control [section_name] [key] [dx] [dy]?  - enable control for config value";
        }
        else if (cmd == "level") {
            help = "level manipulations\n"
                   "available commands:\n"
                   "  level list cached?            - list available (or cached) levels\n"
                   "  level cache [level_section]   - cache specified level\n"
                   "  level cache clear             - clear levels cache\n"
                   "  level current                 - prints section name of the current level\n"
                   "  level current [level_section] - setup current level\n"
                   "  level current none            - reset current level";
        }
        else if (cmd == "lobby") {
            help = "multiplayer lobby\n"
                   "available commands:\n"
                   "  lobby       - stop the game and open the lobby (server)\n"
                   "  lobby start - start the game with the lobby's map and game mode";
        }
        else if (cmd == "hud") {
            help = "on-screen information\n"
                   "available commands:\n"
                   "  hud net [on/off]?  - the network state (like the net command) at the top left corner";
        }
        else if (cmd == "net") {
            help = "multiplayer latency settings\n"
                   "available commands:\n"
                   "  net                         - show the input buffers (server) and the interpolation delay (client)\n"
                   "  net buffer [auto|ticks]     - input jitter buffer of the clients (server; auto: from their jitter;\n"
                   "                                a tick is ~8.3 ms)\n"
                   "  net interp [min|margin] [ms] - remote players are shown jitter + state interval + margin\n"
                   "                                in the past, at least min (client)\n"
                   "  net input margin [ms]       - the inputs arrive at the server this long before it needs them\n"
                   "                                (client: aligns its ticks, at most a frame more)";
        }
        else if (cmd == "game") {
            help = "game settings\n"
                   "available commands:\n"
                   "  game [on/off]?          - shows or setups game running state\n"
                   "  game speed [speed]?     - shows or setups game speed\n"
                   "  game mode               - shows current game mode\n"
                   "  game mode [game_mode]   - setup current game mode (\"default\" or \"gm_default\")\n"
                   "  game mode list          - list available game modes";
        }
        else if (cmd == "ai") {
            help = "ai operator settings\n"
                   "available commands:\n"
                   "  ai list                                    - lists all operated player names\n"
                   "  ai bind [player_name]                      - binds player to the ai operator\n"
                   "  ai difficulty [player_name] [difficulty]?  - shows or setups ai difficulty\n"
                   "    difficulty: easy medium hard\n"
                   "  ai profiler [on/off]                       - enable/disable ai profiler";
        }
        else if (cmd == "spawn") {
            help = "spawn options\n"
                   "available commands:\n"
                   "  spawn crate  - spawn a crate that drops to a platform";
        }
        else if (gs.lua_cmd_enabled && cmd == "lua") {
            help = "Lua interpreter\n"
                   "lua [command]    - execute lua line";
        }
        else if (cmd == "fps") {
            help = "FPS counter in the top left corner\n"
                   "fps [on/off]?  - toggle or enable/disable the fps counter";
        }
        else if (cmd == "sound") {
            help = "Sound settings\n"
                   "sound volume [0 - 100]  - set or get sound volume";
        }
        else if (cmd == "efx") {
            help = "Effects manipulation\n"
                   "efx reload  - reload all effects in data/efx/";
        }

        if (!help.empty())
            glog().detail("{}", help);
    }

    void cmd_player_delete(const std::string& player_name) {
        auto found = gs.players.find(player_name);
        if (found != gs.players.end()) {
            auto found_ai = gs.ai_operators.find(player_name);
            if (found_ai != gs.ai_operators.end()) {
                gs.ai_operators.erase(found_ai);
                glog().info("ai operator {} deleted", player_name);
            }

            gs.players.erase(found);
            glog().info("player {} deleted", player_name);
        }
        else {
            glog().error("player {} not found", player_name);
        }
    }

    void cmd_player_wpn(const std::string& player_name, const std::optional<std::string>& wpn_section) {
        auto found = gs.players.find(player_name);
        if (found == gs.players.end()) {
            glog().error("player {} not found", player_name);
            return;
        }

        auto& player = *found->second;
        if (wpn_section) {
            if (weapon_mgr().is_exists(*wpn_section))
                player.setup_pistol(*wpn_section, gs.cur_game_mode.magazines_count);
            else
                glog().error("weapon {} not found", *wpn_section);
        }
        else {
            glog().info("player wpn {}: {}", player_name, player.pistol_section());
        }
    }

    void cmd_lua(const std::string& cmd) {
        gs.sig_execute_lua.emit_immediate(cmd);
    }

    void cmd_spawn(const std::string& cmd, const std::optional<std::string>& value) {
        if (cmd == "crate") {
            if (value) {
                glog().error("spawn crate: takes no arguments");
                return;
            }
            gs.crates_spawn();
        }
        else if (cmd == "help") {
            cmd_help("spawn");
        }
        else
            glog().error("spawn: unknown subcommand '{}'", cmd);
    }

    void cmd_player(const std::string& cmd, const std::optional<std::string>& value) {
        if (cmd == "reload") {
            gs.player_conf_reload(value ? player_name_t(*value) : player_name_t{});
        }
        else if (cmd.starts_with("controller")) {
            auto strnum = cmd.substr("controller"sv.size());
            if (strnum.empty()) {
                glog().error("player controller: missing controller number");
                return;
            }

            u32 controller_id;
            try {
                controller_id = ston<u32>(strnum);
            } catch (...) {
                glog().error("player controller: invalid controller number");
                return;
            }

            if (!value) {
                glog().error("player {}: missing action or player name", cmd);
                return;
            }

            auto ok = *value == "delete" ?
                gs.controller_delete(controller_id) :
                gs.controller_bind(controller_id, *value);

            if (ok)
                gs.rebuild_controlled_players();
        }
        else if (cmd == "create") {
            if (!value) {
                glog().error("player create: missing player name argument");
                return;
            }

            auto        params = *value / split(' ', '\t');
            std::string name;
            int         group = 0;
            for (auto param : params) {
                auto prm = std::string_view(param.begin(), param.end());
                if (prm.starts_with("name="))
                    name = std::string(prm.substr("name="sv.size()));
                else if (prm.starts_with("group=")) {
                    auto group_str = std::string(prm.substr("group="sv.size()));
                    try {
                        group = ston<int>(group_str);
                    }
                    catch (...) {
                        glog().warn("player create: invalid group number in params");
                    }
                }
                else {
                    name = std::string(prm);
                }
            }
            if (name.empty()) {
                glog().error("player create: missing name in params");
                return;
            }

            if (auto plr = gs.player_create(name))
                plr->group(group);
        }
        else if (cmd == "conf") {
            if (!value) {
                glog().error("player conf: missing player name argument");
                return;
            }

            gs.pconf_ui = std::make_unique<player_configurator_ui>(*value);
            gs.pconf_ui->show(true);
        }
        else if (cmd == "help") {
            cmd_help("player");
        }
        else {
            glog().error("player: unknown subcommand '{}'", cmd);
        }
    }

    void cmd_cfg_list(const std::optional<std::string>& value) {
        std::string sects;
        if (value) {
            for (auto& [sect, _] : cfg::global().get_sections())
                if (sect.starts_with(*value))
                    sects += "\n" + sect;
        }
        else {
            for (auto& [sect, _] : cfg::global().get_sections()) sects += "\n" + sect;
        }
        glog().detail("Sections:{}", sects);
        return;
    }

    void cmd_cfg_reload(const std::optional<std::string>& type) {

#define reload_type(TYPE, ...)                                                                     \
    do {                                                                                           \
        if (!type || *type == TYPE) {                                                              \
            __VA_ARGS__                                                                            \
            if (type) {                                                                            \
                glog().info(TYPE " configs reloaded!");                                               \
                return;                                                                            \
            }                                                                                      \
        }                                                                                          \
    } while (0)

        reload_type("weapons", weapon_mgr().reload(););
        reload_type("levels",
            for (auto& [lvl_name, lvl] : gs.levels) lvl->cfg_reload();
            if (gs.cur_level) {
                gs.cur_level->setup_to(gs.sim);
                gs.sig_level_changed.emit_deferred(gs.cur_level->section_name());
            }
        );

        glog().info("configs reloaded!");
        return;
#undef reload_type
    }

    void cmd_cfg(const std::string& sect, const std::optional<std::string>& value) {
        if (sect == "help") {
            cmd_help("cfg");
            return;
        }

        auto optsect = cfg::global().try_get_section(sect);
        if (!optsect) {
            glog().error("cfg: section [{}] not found", sect);
            return;
        }

        if (value) {
            auto optkey = optsect->try_get<std::string>(*value);
            if (!optkey) {
                glog().error("cfg {}: key '{}' not found", sect, *value);
                return;
            }

            glog().detail("{} = {}", *value, optkey->value());
        }
        else {
            glog().detail("{}", *optsect);
            /*
            size_t spaces = 0;
            std::string to_print;
            for (auto& [k, v] : found_sect->second.sects) {
                to_print.push_back('\n');
                to_print += k;
                if (k.size() > spaces)
                    spaces = k.size();
                else
                    to_print.resize(to_print.size() + (spaces - k.size()), ' ');
                to_print += " = "sv;
                to_print += v;
            }
            glog().detail("{}", to_print);
            */
        }
    }

    void cmd_cfg_set(const std::string& sect, const std::string& key, const std::optional<std::string>& value) {
        auto optsect = cfg::mutable_global().try_get_section(sect);
        if (!optsect) {
            glog().error("cfgset: section [{}] not found", sect);
            return;
        }

        auto optkey = optsect->try_get<std::string>(key);
        if (!optkey) {
            glog().error("cfgset {}: key '{}' not found", sect, key);
            return;
        }

        optkey->set(value ? *value : "");
        cfg::mutable_global().commit();
        glog().info("[{}] {} = {}", sect, key, value ? *value : "");
    }

    void cmd_cfg_mode(const std::optional<std::string>& value) {
        if (value) {
            bool readonly = false;
            if (*value == "readonly" || *value == "r")
                readonly = true;
            else if (*value != "readwrite" && *value != "rw")
                glog().error("cfg mode: invalid argument {} (must be readonly/r or readwrite/rw", *value);
            if (cfg::mutable_global().is_readonly() != readonly) {
                cfg::mutable_global().set_readonly(readonly);
                glog().info("Config has been mounted in {} mode", readonly ? "readonly" : "readwrite");
            }
        }
        else
            glog().info("config mode: {}", cfg::mutable_global().is_readonly() ? "readonly" : "readwrite");
    }

    void cmd_cfg_watch(const std::string& value) {
        try {
            gs.conf_watcher.watch_section(cfg::mutable_global().get_section(value));
            glog().info("watch changes in section [{}]...", value);
        } catch (const std::exception& e) {
            glog().error("cfg watch: {}", e.what());
        }
    }

    void cmd_cfg_watch_list() {
        glog().info("watched sections:");
        for (auto& sect_name : gs.conf_watcher.watched_section_names())
            glog().info("    {}", sect_name);
    }

    void cmd_cfg_watch_remove(const std::string& value) {
        try {
            auto ok = gs.conf_watcher.remove_section(cfg::mutable_global().get_section(value));
            if (ok)
                glog().info("remove section [{}] from watch list", value);
            else
                glog().error("cfg watch remove: section [{}] does not exists", value);
        } catch (const std::exception& e) {
            glog().error("cfg watch remove: {}", e.what());
        }
    }

    void cmd_level(const std::string& cmd, const std::optional<std::string>& value) {
        if (cmd == "list") {
            if (value && *value == "cached") {
                std::vector<std::string> res;
                for (auto& [l, _] : gs.levels)
                    res.push_back(l);
                glog().detail("Cached levels: {}", res);
            } else {
                glog().detail("Available levels: {}", level::list_available_levels());
            }
        }
        else if (cmd == "cache") {
            if (value) {
                if (*value == "clear")
                    gs.level_cache_clear();
                else
                    gs.level_cache(*value);
            } else {
                glog().error("log cache: missing level section argument");
            }
        }
        else if (cmd == "current") {
            if (value)
                gs.level_current(*value != "none" ? *value : std::string{});
            else {
                glog().info("level current: {}", gs.cur_level ? gs.cur_level->section_name() : "none");
            }
        }
        else if (cmd == "help") {
            cmd_help("level");
        }
        else {
            glog().error("level: unknown subcommand '{}'", cmd);
        }
    }

    void cmd_game_speed(const std::optional<std::string>& value) {
        if (!value) {
            glog().info("game speed: {}", gs.game_speed);
        }
        else {
            float v;
            try {
                v = ston<float>(*value);
            }
            catch (...) {
                glog().error("game speed: argument must be a number");
                return;
            }
            gs.game_speed = v;
        }
    }

    void cmd_game_mode(const std::optional<std::string>& value) {
        auto& gm = gs.cur_game_mode;
        if (!value) {
            glog().info("game mode: {} (bullet_gravity = {}, crate_drop_period = {}, crate_limit = {}, "
                        "magazines_count = {}, score_system = {}, start_lives = {})",
                        gm.section,
                        gm.bullet_gravity,
                        gm.crate_drop_period,
                        gm.crate_limit,
                        gm.magazines_count,
                        game_mode::score_system_name(gm.score_system),
                        gm.start_lives);
        }
        else if (*value == "list") {
            glog().detail("Available game modes: {}", game_mode::list_available());
        }
        else if (*value == "help") {
            cmd_help("game");
        }
        else if (gs.game_mode_networked) {
            glog().error("game mode: the game mode is set by the server");
        }
        else {
            gs.game_mode_apply(*value);
        }
    }

    void cmd_lobby(const std::optional<std::string>& value) {
        if (value && *value == "help") {
            cmd_help("lobby");
            return;
        }
        if (gs.lobby_networked) {
            glog().error("lobby: the lobby is driven by the server");
            return;
        }
        if (!value)
            gs.lobby_open();
        else if (*value == "start")
            gs.lobby_start();
        else
            glog().error("lobby: unknown subcommand '{}'", *value);
    }

    void cmd_game(const std::optional<std::string>& value) {
        if (value) {
            if (*value == "help")
                cmd_help("game");
            else if (*value == "on")
                gs.game_run();
            else if (*value == "off")
                gs.game_stop();
            else
                glog().error("game: invalid argument {}", *value);
        } else {
            glog().info("game: {}", gs.on_game ? "on" : "off");
        }
    }

    void cmd_ai_difficulty(const std::string& player_name, const std::optional<std::string>& difficulty) {
        auto found = gs.ai_operators.find(player_name);
        if (found == gs.ai_operators.end()) {
            glog().error("ai difficulty: ai operator for player {} was not found", player_name);
            return;
        }

        if (difficulty) {
            try {
                found->second->set_difficulty(*difficulty);
            }
            catch (const std::exception& e) {
                glog().error("ai difficulty: cannot set difficulty {}: {}", *difficulty, e.what());
            }
        }
        else {
            glog().info("ai difficulty {}: {}", player_name, found->second->difficulty());
        }
    }

    void cmd_ai_profiler(bool value) {
        gs.ai_profiler_enabled = value;
    }

    void cmd_ai(const std::string& cmd, const std::optional<std::string>& value) {
        if (cmd == "list") {
            std::string msg = "active AI operators:\n";
            for (auto& [name, _] : gs.ai_operators) msg += "  " + std::string(name);
            glog().detail("{}", msg);
        }
        else if (cmd == "bind") {
            if (value) {
                if (gs.ai_bind(*value))
                    glog().info("player {} now ai-operated", *value);
            }
            else
                glog().error("ai bind: missing player name");
        }
        else if (cmd == "help") {
            cmd_help("ai");
        }
        else {
            glog().error("ai: unknown subcommand {}", cmd);
        }
    }

    void cmd_cfg_control(const std::string&                section,
                         const std::string&                key,
                         const std::string&                str_steps) {
        auto sect = cfg::global().try_get_section(section);
        if (!sect) {
            glog().error("cfg control: section [{}] was not found", section);
            return;
        }

        if (!sect->has_key(key)) {
            glog().error("cfg control {}: key {} was not found", section, key);
            return;
        }

        cfg_value_control::value_t steps;
        try {
            u32 idx = 0;
            for (auto str_step_view : str_steps / split(' ', '\t')) {
                steps[idx] = ston<float>(std::string(str_step_view.begin(), str_step_view.end()));
                ++idx;
            }
        }
        catch (...) {
            glog().error("cfg control {} {} {}: steps argument must contain numbers only",
                    section,
                    key,
                    str_steps);
            return;
        }

        gs.cfgval_ctrl = cfg_value_control(section, key, steps);
        glog().info("controlling [{}]:{} with steps {}", section, key, str_steps);
    }

    void cmd_list() {
        std::string commands;
        for (auto& [cmd, _] : command_buffer().get_command_tree()->subcmds) commands += "\n" + cmd;
        glog().detail("available commands: {}", commands);
    }

    void cmd_sound_volume(const std::optional<std::string>& volume) {
        if (volume) {
            try {
                sound_mgr().volume(ston<float>(*volume));
            } catch (...) {
                glog().error("sound volume: value must be a number [0 - 100]");
            }
        }
        else {
            glog().info("sound volume: {}", sound_mgr().volume());
        }
    }

    void efx_reload() {
        gs.load_effects();
        glog().info("effects reloaded");
    }

    void cmd_shutdown() {
        gs.sig_shutdown.emit_deferred();
    }

private:
    game_state& gs;
};

} // namespace dfdh
