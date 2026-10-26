#include "src/stdafx.hpp"

#include "base/serialization.hpp"
#include "ui/player_configurator_ui.hpp"
#include "ui/lobby_ui.hpp"

#include "engine.hpp"
#include "command_buffer.hpp"
#include "game_commands.hpp"
#include "game_state.hpp"

#include "script/lua_init.hpp"

#include "net/net_server.hpp"
#include "net/net_client.hpp"
#include "net_player_controller.hpp"

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Graphics/RectangleShape.hpp>

inline constexpr bool MP_MOVE_ADJUSTMENT = true;

namespace dfdh {

class diefastdiehard : public dfdh::engine, public slot_holder {
public:
    /* Commands */
    void cmd_log_time(cmd_opt<bool> opt) {
        if (opt)
            devcons().show_time(opt.test());
        else
            glog().info("log time: {}", cmd_opt<bool>(devcons().is_show_time()));
    }

    void cmd_log_level(cmd_opt<bool> opt) {
        if (opt)
            devcons().show_level(opt.test());
        else
            glog().info("log time: {}", cmd_opt<bool>(devcons().is_show_level()));
    }

    void cmd_log_ring(cmd_opt<size_t> size) {
        if (size)
            devcons().ring_size(*size);
        else
            glog().info("log ring: {}", devcons().ring_size());
    }

    void cmd_log(const std::string& cmd) {
        if (cmd == "clear") {
            devcons().clear();
        }
        else if (cmd == "help") {
            gc.cmd_help("log");
        }
        else {
            glog().error("log: unknown subcommand '{}'", cmd);
        }
    }

    void cmd_fps(cmd_opt<bool> opt) {
        enable_fps_counter(opt ? opt.test() : !is_fps_counter_enabled());
        glog().info("fps: {}", cmd_opt<bool>(is_fps_counter_enabled()));
    }

    void cmd_net(const std::optional<std::string>& value) {
        if (value && *value == "help") {
            gc.cmd_help("net");
            return;
        }
        if (net_srv) {
            auto fixed = net_srv->input_buffer();
            glog().info("net input buffer: {}", fixed ? std::to_string(*fixed) + " ticks" : std::string("auto"));
            for (auto& st : net_srv->input_stats())
                glog().info("  {}: {} ticks, jitter {:.1f} ms, wait {:.1f} ms", st.player, st.target, st.jitter_s * 1000.0,
                            st.wait_s * 1000.f);
        }
        if (net_cli) {
            auto& clk = net_cli->clock_estimate();
            glog().info("net interp: {:.1f} ms (jitter {:.1f} ms + margin {:.1f} ms, min {:.1f} ms), rtt {:.1f} ms",
                        clk.interp_delay() * 1000.0, clk.jitter() * 1000.0, clk.interp_margin() * 1000.0,
                        clk.interp_min() * 1000.0, net_cli->rtt() * 1000.f);
        }
        if (!net_srv && !net_cli)
            glog().info("net: not in a multiplayer game");
    }

    void cmd_net_buffer(const std::string& value) {
        if (!net_srv) {
            glog().error("net buffer: not a server");
            return;
        }
        if (value == "auto")
            net_srv->input_buffer({});
        else if (auto ticks = parse_count(value))
            net_srv->input_buffer(*ticks);
        else
            glog().error("net buffer: expected 'auto' or a number of ticks, got '{}'", value);
        cmd_net({});
    }

    void cmd_net_interp(const std::string& key, const std::string& value) {
        if (!net_cli) {
            glog().error("net interp: not a client");
            return;
        }
        auto ms = parse_count(value);
        if (!ms || (key != "min" && key != "margin")) {
            glog().error("net interp: expected 'min|margin <ms>'");
            return;
        }
        auto& clk = net_cli->clock_estimate();
        auto  s   = double(*ms) / 1000.0;
        net_cli->interp_settings(key == "min" ? s : clk.interp_min(), key == "margin" ? s : clk.interp_margin());
        cmd_net({});
    }

    static std::optional<size_t> parse_count(const std::string& value) {
        try {
            return ston<size_t>(value);
        }
        catch (const std::exception&) {
            return {};
        }
    }

    void init_commands() {
        command_buffer().add_handler("net", &diefastdiehard::cmd_net, this);
        command_buffer().add_handler("net buffer", &diefastdiehard::cmd_net_buffer, this);
        command_buffer().add_handler("net interp", &diefastdiehard::cmd_net_interp, this);
        command_buffer().add_handler("fps", &diefastdiehard::cmd_fps, this);
        command_buffer().add_handler("log", &diefastdiehard::cmd_log, this);
        command_buffer().add_handler("log time", &diefastdiehard::cmd_log_time, this);
        command_buffer().add_handler("log level", &diefastdiehard::cmd_log_level, this);
        command_buffer().add_handler("log ring", &diefastdiehard::cmd_log_ring, this);
        command_buffer().add_handler("profiler", &diefastdiehard::enable_profiler_print, static_cast<engine*>(this));
    }

public:
    void init_signals() {
        gs.sig_level_changed.attach_function("level_changed", [this](const std::string&) {
            apply_window_size(window().getSize().x, window().getSize().y);
        });
        gs.sig_shutdown.attach_function("shutdown", [this] { window().close(); });
        gs.sig_execute_lua.attach_function("execute_lua", [this](const std::string& code) { lua->execute_line(code); });
    }

public:
    void on_init(args_view args) final {
        init_signals();
        init_commands();

        if (args.get("--physic-debug"))
            gs.debug_physics = true;

        if (args.get("--server"))
            init_server(args);
        else if (args.get("--client"))
            init_client(args);

        init_lua();
    }

    void on_destroy() final {
        if (net_cli)
            net_cli->leave();

        if (!gs.ai_operators.empty())
            ai_mgr().worker_stop();
    }

    void game_update() final {
        gs.game_update();

        if (net_cli) {
            /* Client mode: free the keyboard from the level bot (recreated on each level load) */
            if (gs.has_controller(0))
                gs.controller_delete(0);

            net_cli->update(gs);
        }

        if (net_srv)
            net_srv->update(gs);

        lua_game_update(&gs);
    }

    void handle_event(const sf::Event& evt) final {
        if (net_ctrl)
            net_ctrl->track_event(evt);

        gs.handle_event(evt);
        lua_handle_event(evt);
    }

    void on_window_resize(u32 width, u32 height) override {
        engine::on_window_resize(width, height);
        apply_window_size(width, height);
    }

    void ui_update() final {
        engine::ui_update();
        gs.ui_update(ui, window_size_float());
        /* The player configurator is drawn instead of the lobby while it is open */
        if (lobby_wnd && gs.lobby.active && !(gs.pconf_ui && gs.pconf_ui->shown()))
            lobby_wnd->update(ui, gs, window_size_float());
        //mainmenu().update(ui);
    }

    void render_update(sf::RenderWindow& wnd) final {
        update_cam();
        gs.render_update(wnd);
    }

    void post_update() final {
        signal_slot_event_updater::instance().operate_tasks();
    }

private:
    void init_lua() {
        lua.emplace(luactx_mgr::global(true));

        lua->ctx().annotate({.argument_names = {"commands"}});
        lua->ctx().provide(LUA_TNAME("cmd"), [](const std::string& commands) {
            for (auto cmd : commands / split('\n')) command_buffer().push(std::string(cmd.begin(), cmd.end()));
        });
        lua->ctx().provide(LUA_TNAME("GS"), &gs);

        lua_handle_event = lua->get_caller<void(const sf::Event&)>("G.handle_event", 2s, true);
        lua_game_update  = lua->get_caller<void(game_state*)>("G.game_update", 2s, true);

        lua->load();
    }

    void apply_window_size(u32 width, u32 height) {
        if (!gs.cur_level)
            return;

        auto& level = *gs.cur_level;
        auto f = (level.view_size().x / float(width)) * (float(height) / level.view_size().y);
        vec2f view_size {
            level.view_size().x,
            f * level.view_size().y
        };

        _view.setSize(view_size);
        window().setView(_view);
    }

    void update_cam() {
        if (!gs.cur_level)
            return;

        float timestep = cam_timer.elapsed();
        cam_timer.restart();
        if (!approx_equal(gs.cam_pos.x, _view.getCenter().x, 0.001f) ||
            !approx_equal(gs.cam_pos.y, _view.getCenter().y, 0.001f)) {
            auto diff = gs.cam_pos - _view.getCenter();
            auto dir  = normalize(diff);
            if (!std::isinf(dir.x) && !std::isinf(dir.y)) {
                auto mov = dir * timestep * 600.f;
                auto c   = _view.getCenter();
                auto nc  = c + mov;
                if ((c.x < gs.cam_pos.x && nc.x > gs.cam_pos.x) ||
                    (c.x > gs.cam_pos.x && nc.x < gs.cam_pos.x))
                    nc.x = gs.cam_pos.x;
                if ((c.y < gs.cam_pos.y && nc.y > gs.cam_pos.y) ||
                    (c.y > gs.cam_pos.y && nc.y < gs.cam_pos.y))
                    nc.y = gs.cam_pos.y;

                _view.setCenter(nc);
                window().setView(_view);
            }
        }

        vec2f center_min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        vec2f center_max = {std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest()};
        for (auto& [_, pl] : gs.players) {
            if (pl->eliminated())
                continue;
            auto pos =
                lerp(pl->collision_box()->get_position(),
                     pl->collision_box()->get_position() + pl->collision_box()->get_velocity() * gs.sim.last_timestep(),
                     gs.sim.interpolation_factor());
            center_min.x = std::min(center_min.x, pos.x);
            center_max.x = std::max(center_max.x, pos.x);

            if (!gs.locally_controlled(*pl))
                continue;

            center_min.y = std::min(center_min.y, pos.y);
            center_max.y = std::max(center_max.y, pos.y);
        }
        auto center = (center_min + center_max) / 2.f;

        for (auto& [_, this_player] : gs.players) {
            if (!gs.locally_controlled(*this_player) || this_player->eliminated())
                continue;

            auto pl_pos_x = lerp(this_player->get_position().x,
                                 this_player->get_position().x +
                                     this_player->collision_box()->get_velocity().x * gs.sim.last_timestep(),
                                 gs.sim.interpolation_factor());
            auto view_sz_x = _view.getSize().x * 0.46f;
            if (center.x - pl_pos_x > view_sz_x)
                center.x = pl_pos_x + view_sz_x;
            if (pl_pos_x - center.x > view_sz_x)
                center.x = pl_pos_x - view_sz_x;

            auto& level = *gs.cur_level;

            if (center.x + _view.getSize().x / 2.f > level.level_size().x)
                center.x = level.level_size().x - _view.getSize().x / 2.f;
            if (center.x - _view.getSize().x / 2.f < 0.f)
                center.x = _view.getSize().x / 2.f;
            if (center.y + _view.getSize().y / 2.f > level.level_size().y)
                center.y = level.level_size().y - _view.getSize().y / 2.f;
            if (center.y - _view.getSize().y / 2.f < 0.f)
                center.y = _view.getSize().y / 2.f;
        }

        gs.cam_pos = center;
    }

private:
    game_state    gs;
    game_commands gc{gs};

    sf::RectangleShape point_shape;
    sf::RectangleShape line_shape;

    timer    cam_timer;
    sf::View _view;

    std::optional<luactx_mgr>          lua;
    lua_caller<void(const sf::Event&)> lua_handle_event;
    lua_caller<void(game_state*)>      lua_game_update;

    std::shared_ptr<net::net_server>        net_srv;
    std::shared_ptr<net::net_client>        net_cli;
    std::shared_ptr<net_player_controller>  net_ctrl;
    player_name_t                            my_player_name;
    std::optional<lobby_ui>                  lobby_wnd;

    /* Open the player configurator of a local player, on_close: the config was written */
    void customize_player(const player_name_t& name, std::function<void()> on_close) {
        gs.pconf_ui           = std::make_unique<player_configurator_ui>(name);
        gs.pconf_ui->on_close = std::move(on_close);
        gs.pconf_ui->show(true);
    }

    void init_server_lobby() {
        lobby_ui::actions_t act;
        act.set_level = [this](const std::string& level) { gs.lobby.level = level; };
        act.set_mode  = [this](const std::string& mode) { gs.lobby.mode = mode; };
        act.set_group = [this](const player_name_t& name, int group) { gs.lobby_player_group(name, group); };
        act.customize = [this](const player_name_t& name) {
            customize_player(name, [this, name] { gs.player_conf_reload(name); });
        };
        act.start         = [this] { gs.lobby_start(); };
        act.local_players = [this] {
            std::vector<player_name_t> res;
            for (auto& [name, _] : gs.controlled_players)
                res.push_back(name);
            return res;
        };
        lobby_wnd.emplace(lobby_ui::role_t::owner, std::move(act));
        gs.lobby_open();
    }

    void init_client_lobby() {
        lobby_ui::actions_t act;
        act.set_group = [this](const player_name_t&, int group) {
            net_cli->lobby_request(i16(group), net_cli->last_lobby_request().ready != 0);
        };
        act.set_ready = [this](bool ready) {
            auto own   = gs.players.find(my_player_name);
            auto group = own != gs.players.end() ? own->second->get_group() : -1;
            net_cli->lobby_request(i16(group), ready);
        };
        act.own_ready = [this] { return net_cli->last_lobby_request().ready != 0; };
        act.customize = [this](const player_name_t& name) {
            customize_player(name, [this, name] {
                player_configurator pc{name};
                pc.write_on_delete = false;
                net_cli->resend_config(pc);
            });
        };
        act.local_players = [this] { return std::vector<player_name_t>{my_player_name}; };
        lobby_wnd.emplace(lobby_ui::role_t::client, std::move(act));
        gs.lobby_networked = true;
        gs.lobby_open();
    }

    void init_server(args_view& args) {
        u16 port = 10443;
        if (auto port_arg = args.by_key_opt<std::string>("--port"))
            port = ston<u16>(*port_arg);

        net_srv = std::make_shared<net::net_server>(port);
        if (auto buffer = args.by_key_opt<std::string>("--input-buffer"); buffer && *buffer != "auto") {
            if (auto ticks = parse_count(*buffer))
                net_srv->input_buffer(*ticks);
            else
                glog().error("multiplayer: --input-buffer expects 'auto' or a number of ticks, got '{}'", *buffer);
        }

        if (!net_srv->is_ready())
            glog().error("multiplayer: can't start the server on port {}", port);
        else
            glog().info("multiplayer: server started on port {}", port);

        init_server_lobby();
    }

    void init_client(args_view& args) {
        auto addr_str = args.next("missing <ip:port> after --client");
        auto pos = addr_str.rfind(':');
        if (pos == std::string::npos || pos == 0) {
            glog().error("multiplayer: invalid server address '{}', expected <ip:port>", addr_str);
            return;
        }

        auto ip = sf::IpAddress(addr_str.substr(0, pos));
        auto port = ston<u16>(std::string(addr_str.substr(pos + 1)));
        if (ip == sf::IpAddress::None) {
            glog().error("multiplayer: invalid server ip '{}'", std::string(addr_str.substr(0, pos)));
            return;
        }

        auto name = args.by_key_default<std::string>("--name", "client0");
        my_player_name = name;

        /* Load skin, tracer and gun settings from the local player config */
        player_configurator pc{name};
        pc.write_on_delete = false;

        net_cli = std::make_shared<net::net_client>(
            ip, port, name, pc.pistol, pc.face_id, pc.body_id, pc.body_color, pc.tracer_color);

        auto interp_ms = [&](const char* key, double default_s) {
            auto value = args.by_key_opt<std::string>(key);
            if (!value)
                return default_s;
            if (auto ms = parse_count(*value))
                return double(*ms) / 1000.0;
            glog().error("multiplayer: {} expects milliseconds, got '{}'", key, *value);
            return default_s;
        };
        net_cli->interp_settings(interp_ms("--interp-min", net::server_clock::min_interp_s),
                                 interp_ms("--interp-margin", net::server_clock::interp_margin_s));

        if (!net_cli->is_ready()) {
            glog().error("multiplayer: can't connect to {}", addr_str);
            return;
        }

        net_cli->sig_joined.attach_function("mp_joined", [this](const player_name_t& pname) {
            /*
             * The first server state broadcast may be processed before this
             * deferred WELCOME handler runs (datagrams can be reordered, e.g.
             * under DFDH_DEBUG_PING), in which case handle_state has already
             * created the local player: reuse it instead of failing
             */
            auto found = gs.players.find(pname);
            player* pl = found != gs.players.end() ? found->second.get()
                                                   : gs.player_create(pname);
            if (!pl)
                return;

            /*
             * The server is authoritative for the local player as well: no local
             * bullets, the actual ones arrive with the server broadcast
             */
            pl->set_server_driven(true);

            /*
             * The keyboard drives the local player through per-tick input
             * sampling only (the same input stream the server applies), so the
             * controller is not attached as a regular event-driven controller
             */
            net_ctrl = std::make_shared<net_player_controller>(1000);
            net_ctrl->player_name = pname;
            net_cli->input_source([ctrl = net_ctrl] { return ctrl->take_input_state(); });
            /* No local spawn: the server spawns the player, the first state places it. The server's lobby starts the game */
            glog().info("multiplayer: controlling player {}", pname);

            /* Crates are server-authoritative: no local spawn, physics or pickup */
            gs.crates_networked = true;
        });

        init_client_lobby();

        net_cli->sig_error.attach_function("mp_error", [](const std::string& msg) {
            glog().error("multiplayer: join failed: {}", msg);
        });
    }
};
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    return std::make_unique<dfdh::diefastdiehard>()->run(dfdh::args_view(argc, argv));
}
