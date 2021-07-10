#pragma once

#include "game_state.hpp"
#include "ston.hpp"
#include "command_buffer.hpp"

namespace dfdh {

class game_commands {
public:
    game_commands(game_state& state): gs(state) {
        command_buffer().add_handler("player", &game_commands::cmd_player, this);
        command_buffer().add_handler("cfg", &game_commands::cmd_cfg, this);
        command_buffer().add_handler("cfg set", &game_commands::cmd_cfg_set, this);
        command_buffer().add_handler("cfg reload", &game_commands::cmd_cfg_reload, this);
        command_buffer().add_handler("cfg list", &game_commands::cmd_cfg_list, this);
        command_buffer().add_handler("help", &game_commands::cmd_help, this);
        command_buffer().add_handler("list", &game_commands::cmd_list, this);
        command_buffer().add_handler("game", &game_commands::cmd_game, this);
        command_buffer().add_handler("level", &game_commands::cmd_level, this);
        command_buffer().add_handler("ai", &game_commands::cmd_ai, this);
        command_buffer().add_handler("ai difficulty", &game_commands::cmd_ai_difficulty, this);
        command_buffer().add_handler("shutdown", &game_commands::cmd_shutdown, this);
    }

    ~game_commands() {
        command_buffer().remove_handler("player");
        command_buffer().remove_handler("cfg");
        command_buffer().remove_handler("help");
        command_buffer().remove_handler("list");
        command_buffer().remove_handler("game");
        command_buffer().remove_handler("level");
        command_buffer().remove_handler("ai");
        command_buffer().remove_handler("shutdown");
    }

    void cmd_help(const std::string& cmd) {
        std::string_view help;
        if (cmd == "player") {
            help = "available commands:\n"
                   "  player reload [player_name]?               - reload player(s) configuration\n"
                   "  player controller[N] [player_name]         - binds specified controller to player\n"
                   "  player controller[N] delete                - deletes specified controller\n"
                   "  player conf [player_name]                  - open configuration ui for player\n"
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
                   "  cfg [section_name] [key]?              - shows key/values for specified section\n"
                   "  cfg reload [weapons|levels]?           - reloads config users\n"
                   "  cfg list [prefix]?                     - show sections\n"
                   "  cfg set [section_name] [key] [value]?  - setup value for specified section";
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
        else if (cmd == "ai") {
            help = "ai operator settings\n"
                   "available commands:\n"
                   "  ai list                                    - lists all operated player names\n"
                   "  ai bind [player_name]                      - binds player to the ai operator\n"
                   "  ai difficulty [player_name] [difficulty]?  - shows or setups ai difficulty\n"
                   "    difficulty: easy medium hard";
        }

        if (!help.empty())
            LOG("{}", help);
    }

    void cmd_player(const std::string& cmd, const std::optional<std::string>& value) {
        if (cmd == "reload") {
            gs.player_conf_reload(value ? player_name_t(*value) : player_name_t{});
        }
        else if (cmd.starts_with("controller")) {
            auto strnum = cmd.substr("controller"sv.size());
            if (strnum.empty()) {
                LOG_ERR("player controller: missing controller number");
                return;
            }

            u32 controller_id;
            try {
                controller_id = ston<u32>(strnum);
            } catch (...) {
                LOG_ERR("player controller: invalid controller number");
                return;
            }

            if (!value) {
                LOG_ERR("player {}: missing action or player name", cmd);
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
                LOG_ERR("player create: missing player name argument");
                return;
            }

            if (gs.is_client()) {
                // TODO: handle this
            }
            else {
                auto params = *value / split(' ', '\t');
                std::string name;
                int group = 0;
                for (auto param : params) {
                    auto prm = std::string_view(param.begin(), param.end());
                    if (prm.starts_with("name="))
                        name = std::string(prm.substr("name="sv.size()));
                    else if (prm.starts_with("group=")) {
                        auto group_str = std::string(prm.substr("group="sv.size()));
                        try {
                            group = ston<int>(group_str);
                        } catch (...) {
                            LOG_WARN("player create: invalid group number in params");
                        }
                    }
                    else {
                        name = std::string(prm);
                    }
                }
                if (name.empty())
                    LOG_ERR("player create: missing name in params");
                else if (auto plr = gs.player_create(name))
                    plr->group(group);
            }
        }
        else if (cmd == "conf") {
            if (!value) {
                LOG_ERR("player conf: missing player name argument");
                return;
            }

            gs.pconf_ui = std::make_unique<player_configurator_ui>(*value);
            gs.pconf_ui->show(true);
        }
        else if (cmd == "help") {
            cmd_help("player");
        }
        else {
            LOG_ERR("player: unknown subcommand '{}'", cmd);
        }
    }

    void cmd_cfg_list(const std::optional<std::string>& value) {
        std::string sects;
        if (value) {
            for (auto& [sect, _] : cfg().sections())
                if (sect.starts_with(*value))
                    sects += "\n" + sect;
        }
        else {
            for (auto& [sect, _] : cfg().sections()) sects += "\n" + sect;
        }
        LOG("Sections:{}", sects);
        return;
    }

    void cmd_cfg_reload(const std::optional<std::string>& type) {

#define reload_type(TYPE, ...)                                                                     \
    do {                                                                                           \
        if (!type || *type == TYPE) {                                                              \
            __VA_ARGS__                                                                            \
            if (type) {                                                                            \
                LOG_INFO(TYPE " configs reloaded!");                                               \
                return;                                                                            \
            }                                                                                      \
        }                                                                                          \
    } while (0)

        reload_type("weapons", weapon_mgr().reload(););
        reload_type("levels",
            for (auto& [_, lvl] : gs.levels) lvl->cfg_reload();
            if (gs.cur_level) {
                gs.cur_level->setup_to(gs.sim);
                gs.events.push(game_state_event::level_changed);
            }
        );

        LOG_INFO("configs reloaded!");
        return;
#undef reload_type
    }

    void cmd_cfg(const std::string& sect, const std::optional<std::string>& value) {
        if (sect == "help") {
            cmd_help("cfg");
            return;
        }

        auto found_sect = cfg().sections().find(sect);
        if (found_sect == cfg().sections().end()) {
            LOG_ERR("cfg: section [{}] not found", sect);
            return;
        }

        if (value) {
            auto found_key = found_sect->second.sects.find(*value);
            if (found_key == found_sect->second.sects.end()) {
                LOG_ERR("cfg {}: key '{}' not found", sect, *value);
                return;
            }

            LOG("{} = {}", *value, found_key->second);
        }
        else {
            LOG("{}", found_sect->second.content());
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
            LOG("{}", to_print);
            */
        }
    }

    void cmd_cfg_set(const std::string& sect, const std::string& key, const std::optional<std::string>& value) {
        auto found_sect = cfg().sections().find(sect);
        if (found_sect == cfg().sections().end()) {
            LOG_ERR("cfgset: section [{}] not found", sect);
            return;
        }

        auto found_key = found_sect->second.sects.find(key);
        if (found_key == found_sect->second.sects.end()) {
            LOG_ERR("cfgset {}: key '{}' not found", sect, key);
            return;
        }

        found_key->second = value ? *value : "";
        LOG_INFO("[{}] {} = {}", sect, key, value ? *value : "");
    }

    void cmd_level(const std::string& cmd, const std::optional<std::string>& value) {
        if (cmd == "list") {
            if (value && *value == "cached") {
                std::vector<std::string> res;
                for (auto& [l, _] : gs.levels)
                    res.push_back(l);
                LOG("Cached levels: {}", res);
            } else {
                LOG("Available levels: {}", level::list_available_levels());
            }
        }
        else if (cmd == "cache") {
            if (value) {
                if (*value == "clear")
                    gs.level_cache_clear();
                else
                    gs.level_cache(*value);
            } else {
                LOG_ERR("log cache: missing level section argument");
            }
        }
        else if (cmd == "current") {
            if (value)
                gs.level_current(*value != "none" ? *value : std::string{});
            else {
                LOG_INFO("level current: {}", gs.cur_level ? gs.cur_level->section_name() : "none");
            }
        }
        else if (cmd == "help") {
            cmd_help("level");
        }
        else {
            LOG_ERR("level: unknown subcommand '{}'", cmd);
        }
    }

    void cmd_game(const std::optional<std::string>& value) {
        if (value) {
            if (*value == "on")
                gs.game_run();
            else if (*value == "off")
                gs.game_stop();
            else
                LOG_ERR("game: invalid argument {}", *value);
        } else {
            LOG_INFO("game: {}", gs.on_game ? "on" : "off");
        }
    }

    void cmd_ai_difficulty(const std::string& player_name, const std::optional<std::string>& difficulty) {
        auto found = gs.ai_operators.find(player_name);
        if (found == gs.ai_operators.end()) {
            LOG_ERR("ai difficulty: ai operator for player {} was not found", player_name);
            return;
        }

        if (difficulty) {
#define choose_difficulty(VALUE)                                                                   \
    else if (*difficulty == #VALUE) {                                                              \
        found->second->set_difficulty(ai_##VALUE);                                                 \
        LOG_INFO("{} ai difficulty set to " #VALUE, player_name);                                  \
    }
            if (false) {}
            choose_difficulty(easy)
            choose_difficulty(medium)
            choose_difficulty(hard)
            else
                LOG_ERR("invalid ai difficulty '{}'", *difficulty);
        } else {
            LOG_INFO("ai difficulty {}: {}",
                     player_name,
                     std::array{
                         "easy", "medium", "hard"}[size_t(found->second->difficulty())]);
        }
    }

    void cmd_ai(const std::string& cmd, const std::optional<std::string>& value) {
        if (cmd == "list") {
            std::string msg = "active AI operators:\n";
            for (auto& [name, _] : gs.ai_operators) msg += "  " + std::string(name);
            LOG("{}", msg);
        }
        else if (cmd == "bind") {
            if (value) {
                if (gs.ai_bind(*value))
                    LOG_INFO("player {} now ai-operated", *value);
            }
            else
                LOG_ERR("ai bind: missing player name");
        }
        else if (cmd == "help") {
            cmd_help("ai");
        }
        else {
            LOG_ERR("ai: unknown subcommand {}", cmd);
        }
    }

    void cmd_list() {
        std::string commands;
        for (auto& [cmd, _] : command_buffer().get_command_tree()->subcmds) commands += "\n" + cmd;
        LOG("available commands: {}", commands);
    }

    void cmd_shutdown() {
        gs.shutdown();
    }

private:
    game_state& gs;
};

} // namespace dfdh