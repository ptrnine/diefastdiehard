#include "src/stdafx.hpp"

#include "base/serialization.hpp"
#include "ui/player_configurator_ui.hpp"
#include "ui/lobby_ui.hpp"
#include "ui/connect_ui.hpp"
#include "ui/main_menu.hpp"

#include "engine.hpp"
#include "command_buffer.hpp"
#include "game_commands.hpp"
#include "game_state.hpp"

#include "efx/efx_editor.hpp"

#include "script/lua_init.hpp"
#include "script/lua_fifo.hpp"

#include "net/net_server.hpp"
#include "net/net_client.hpp"
#include "net_player_controller.hpp"

#include <format>
#include <sstream>

#include <SFML/Network/Dns.hpp>
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

    /* The network state: the input buffers (server), the RTT and the interpolation delay (client) */
    [[nodiscard]]
    std::vector<std::string> net_info_lines() const {
        std::vector<std::string> lines;
        if (net_srv) {
            auto fixed = net_srv->input_buffer();
            lines.push_back("net input buffer: " + (fixed ? std::to_string(*fixed) + " ticks" : std::string("auto")));
            for (auto& st : net_srv->input_stats())
                lines.push_back(std::format("  {}: {} ticks, jitter {:.1f} ms, wait {:.1f} ms, margin {}", std::string(st.player),
                                            st.target, st.jitter_s * 1000.0, st.wait_s * 1000.f,
                                            st.margin_s ? std::format("{:.1f} ms", *st.margin_s * 1000.f) : "-"));
        }
        if (net_cli) {
            auto& clk = net_cli->clock_estimate();
            lines.push_back(std::format("net rtt: {:.1f} ms", net_cli->rtt() * 1000.f));
            lines.push_back(std::format("net interp: {:.1f} ms", clk.interp_delay() * 1000.0));
            lines.push_back(std::format("  jitter {:.1f} + state interval {:.1f} + margin {:.1f} ms, min {:.1f} ms",
                                        clk.jitter() * 1000.0, clk.max_interval() * 1000.0, clk.interp_margin() * 1000.0,
                                        clk.interp_min() * 1000.0));
            auto margin = net_cli->input_margin_estimate();
            lines.push_back(std::format("net input margin: {} (target {:.1f} .. +{:.1f} ms frame), tick rate {:+.2f} %",
                                        margin ? std::format("{:.1f} ms", *margin * 1000.0) : std::string("-"),
                                        net_cli->input_margin() * 1000.0, net_cli->frame_interval() * 1000.0,
                                        -net_cli->tick_adjust() * 100.0));
        }
        if (!net_srv && !net_cli)
            lines.emplace_back("net: not in a multiplayer game");
        return lines;
    }

    void cmd_net(const std::optional<std::string>& value) {
        if (value && *value == "help") {
            gc.cmd_help("net");
            return;
        }
        for (auto& line : net_info_lines())
            glog().info("{}", line);
    }

    void cmd_hud(const std::optional<std::string>&) {
        gc.cmd_help("hud");
    }

    void cmd_hud_net(cmd_opt<bool> opt) {
        net_hud = opt ? opt.test() : !net_hud;
        glog().info("hud net: {}", cmd_opt<bool>(net_hud));
    }

    /* The network state (like the net command) at the top left corner, under the fps counter */
    void net_hud_draw() {
        auto lines = net_info_lines();
        if (lines.empty())
            return;

        /* The rows with nuklear's default spacing between them and padding around */
        constexpr float row_height = 20.f; /* like the fps counter */
        auto            top        = is_fps_counter_enabled() ? 36.f : 4.f;
        auto            bounds     = nk_rect(4.f, top, 560.f, (row_height + 4.f) * float(lines.size()) + 16.f);

        auto style = &ui.nk_ctx()->style;
        ui.style_push_style_item(&style->window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
        /* Nuklear takes the bounds of a new window only: keep it in place (fps counter toggled, more clients) */
        ui.window_set_bounds("net_hud", bounds);
        if (ui.begin("net_hud", bounds, NK_WINDOW_NO_INPUT | NK_WINDOW_NO_SCROLLBAR)) {
            ui.layout_row_dynamic(row_height, 1);
            for (auto& line : lines)
                ui.label_colored(line.data(), NK_TEXT_LEFT, sf::Color(255, 255, 0));
        }
        ui.end();
        ui.style_pop_style_item();
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

    void cmd_net_input_margin(const std::string& value) {
        if (!net_cli) {
            glog().error("net input margin: not a client");
            return;
        }
        if (auto ms = parse_count(value))
            net_cli->input_margin(double(*ms) / 1000.0);
        else
            glog().error("net input margin: expected milliseconds, got '{}'", value);
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

    void cmd_ui(const std::optional<std::string>&) {
        gc.cmd_help("ui");
    }

    void cmd_ui_connect() {
        if (net_srv) {
            glog().error("ui connect: this game is a server");
            return;
        }
        if (net_cli && !connect_wnd->connecting()) {
            glog().error("ui connect: already connected to a server");
            return;
        }
        connect_wnd->show(true);
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
        command_buffer().add_handler("hud", &diefastdiehard::cmd_hud, this);
        command_buffer().add_handler("hud net", &diefastdiehard::cmd_hud_net, this);
        command_buffer().add_handler("net buffer", &diefastdiehard::cmd_net_buffer, this);
        command_buffer().add_handler("net interp", &diefastdiehard::cmd_net_interp, this);
        command_buffer().add_handler("net input margin", &diefastdiehard::cmd_net_input_margin, this);
        command_buffer().add_handler("ui", &diefastdiehard::cmd_ui, this);
        command_buffer().add_handler("ui connect", &diefastdiehard::cmd_ui_connect, this);
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
        /* The first run creates the current player and its settings section (written by the destructor) */
        {
            player_configurator current{player_configurator::current_player()};
        }
        init_connect_ui();
        init_main_menu();

        /* Taken before init_client: it reads the server address with args.next() */
        auto lua_fifo_path = args.by_key_opt<std::string>("--lua-exec-fifo");

        if (args.get("--physic-debug"))
            gs.debug_physics = true;

        if (args.get("--server"))
            init_server(args);
        else if (args.get("--client"))
            init_client(args);

        init_lua();

        if (args.get("--lua-exec-stdin"))
            execute_lua_stdin();

        if (lua_fifo_path)
            lua_exec_fifo.emplace(std::move(*lua_fifo_path));
    }

    /* Execute the whole stdin the way the in-game `lua` command executes its line */
    void execute_lua_stdin() {
        std::ostringstream oss;
        oss << std::cin.rdbuf();
        auto code = oss.str();
        if (code.empty())
            glog().error("--lua-exec-stdin: stdin is empty, nothing to execute");
        else
            lua->execute_line(code);
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

        connect_update();

        if (server_stop) {
            /* The menu's lobby was cancelled: back to the main menu */
            server_stop = false;
            server_from_ui = false;
            lobby_wnd.reset();
            net_srv.reset();
            if (host_player) {
                if (gs.has_controller(0))
                    gs.controller_delete(0);
                gs.player_remove(*host_player);
                host_player.reset();
            }
            glog().info("multiplayer: server stopped");
        }

        if (net_srv)
            net_srv->update(gs);

        lua_game_update(&gs);

        if (lua_exec_fifo)
            if (auto code = lua_exec_fifo->poll())
                lua->execute_line(*code);
    }

    void handle_event(const sf::Event& evt) final {
        if (auto key = evt.getIf<sf::Event::KeyPressed>(); key && key->code == sf::Keyboard::Key::Escape && close_top_window())
            return;

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
        if (net_hud)
            net_hud_draw();
        /* Any configurator (the menu's, the lobby's, the console's) opens where the last one was closed */
        if (gs.pconf_ui && !gs.pconf_ui->on_bounds_save)
            pconf_bounds_persist(*gs.pconf_ui);
        gs.ui_update(ui, window_size_float());
        /* Shown out of a game with no window open; drawn first: the windows go over it while it fades */
        main_menu_wnd->show(!net_srv && !net_cli && !gs.lobby.active && !connect_wnd->shown() &&
                            !(gs.pconf_ui && gs.pconf_ui->shown()));
        main_menu_wnd->update(ui, window_size_float());
        connect_wnd->update(ui, window_size_float());
        if (lobby_shown())
            lobby_wnd->update(ui, gs, window_size_float());
    }

    void render_update(sf::RenderWindow& wnd) final {
        update_cam();
        gs.render_update(wnd);
    }

    void post_update() final {
        signal_slot_event_updater::instance().operate_tasks();
    }

private:
    /* The player configurator is drawn instead of the lobby while it is open. A client's lobby is shown once the server accepted it */
    [[nodiscard]]
    bool lobby_shown() const {
        return lobby_wnd && gs.lobby.active && !(gs.pconf_ui && gs.pconf_ui->shown()) && (!net_cli || net_cli->joined());
    }

    /* Esc: close the topmost window or the settings submenu. Returns false if there was nothing to close */
    bool close_top_window() {
        if (gs.pconf_ui && gs.pconf_ui->shown())
            gs.pconf_ui->close();
        else if (connect_wnd->shown())
            connect_wnd->close();
        else if (lobby_shown())
            lobby_wnd->close();
        else
            return main_menu_wnd->back();
        return true;
    }

    /* The configurator window's position and size in [game_settings], saved on close */
    void pconf_bounds_persist(player_configurator_ui& pconf) {
        auto& p    = game_settings();
        auto  r    = pconf.bounds();
        auto  pos  = p.value_or_default("pconf_window_pos", vec2f{r.x, r.y});
        auto  size = p.value_or_default("pconf_window_size", vec2f{r.w, r.h});
        pconf.bounds(nk_rect(pos.x, pos.y, size.x, size.y));
        pconf.on_bounds_save = [this](const struct nk_rect& bounds) {
            auto& p = game_settings();
            p.set("pconf_window_pos", vec2f{bounds.x, bounds.y});
            p.set("pconf_window_size", vec2f{bounds.w, bounds.h});
            game_settings_commit();
        };
    }

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
    std::optional<lua_fifo>            lua_exec_fifo;

    std::shared_ptr<net::net_server>        net_srv;
    std::shared_ptr<net::net_client>        net_cli;
    std::shared_ptr<net_player_controller>  net_ctrl;
    player_name_t                            my_player_name;
    std::optional<lobby_ui>                  lobby_wnd;
    std::optional<connect_ui>                connect_wnd;
    std::optional<main_menu>                 main_menu_wnd;
    bool                                     server_from_ui = false; /* hosted from the main menu: cancel lobby stops it */
    bool                                     server_stop    = false; /* the menu's lobby was cancelled */
    std::optional<player_name_t>             host_player;           /* the current player, created to host from the menu */
    bool                                     client_from_ui = false; /* joined from the connect window: leave returns to it */
    std::optional<std::string>               client_error;          /* the server rejected the joining client */
    bool                                     client_leave = false;  /* the lobby's leave was clicked */
    bool                                     net_hud = false; /* hud net: the network state on the screen */

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
        act.cancel_lobby  = [this] {
            gs.lobby_close();
            /* Handled in game_update: the lobby window can't be destroyed while it is drawn */
            if (server_from_ui)
                server_stop = true;
        };
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
        /* Leave: back to the connect window, or quit the app if joined with --client (the net client destructor tells the server) */
        act.leave = [this] {
            if (client_from_ui)
                client_leave = true;
            else
                gs.sig_shutdown.emit_deferred();
        };
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

    static constexpr u16 default_port = 10443;

    /* Returns false if the server can't listen on the port */
    bool server_start(u16 port) {
        net_srv = std::make_shared<net::net_server>(port);
        if (!net_srv->is_ready()) {
            glog().error("multiplayer: can't start the server on port {}", port);
            return false;
        }
        glog().info("multiplayer: server started on port {}", port);
        return true;
    }

    void init_server(args_view& args) {
        u16 port = default_port;
        if (auto port_arg = args.by_key_opt<std::string>("--port"))
            port = ston<u16>(*port_arg);

        server_start(port);
        if (auto buffer = args.by_key_opt<std::string>("--input-buffer"); buffer && *buffer != "auto") {
            if (auto ticks = parse_count(*buffer))
                net_srv->input_buffer(*ticks);
            else
                glog().error("multiplayer: --input-buffer expects 'auto' or a number of ticks, got '{}'", *buffer);
        }

        init_server_lobby();
    }

    void init_main_menu() {
        main_menu::actions_t act;
        act.host = [this] {
            if (!server_start(u16(game_settings().value_or_default_and_set<u32>("host_port", default_port)))) {
                net_srv.reset();
                return;
            }
            server_from_ui = true;
            /* The current player hosts: created before the lobby lists the local players */
            auto name = player_configurator::current_player();
            if (gs.player_create(name)) {
                host_player = name;
                gs.controller_bind(0, name);
                gs.rebuild_controlled_players();
            }
            init_server_lobby();
        };
        act.connect = [this] { connect_wnd->show(true); };
        act.player  = [this] {
            gs.pconf_ui = std::make_unique<player_configurator_ui>(player_configurator::current_player(), true);
            gs.pconf_ui->on_name_change = [](const player_name_t& name) { player_configurator::current_player(name); };
            gs.pconf_ui->show(true);
        };
        act.exit    = [this] { gs.sig_shutdown.emit_deferred(); };
        main_menu_wnd.emplace(std::move(act));
    }

    void init_connect_ui() {
        connect_ui::actions_t act;
        act.connect = [this](const std::string& host, u16 port) -> std::optional<std::string> {
            if (net_srv)
                return "this game is a server";
            /* The joined game replaces the local one */
            players_remove_all();
            client_from_ui = true;
            return client_connect(host, port, player_configurator::current_player());
        };
        act.cancel = [this] { client_reset(); };
        act.load   = [this] {
            auto&                p = game_settings();
            connect_ui::params_t res;
            res.host = p.value_or_default("connect_host", res.host);
            res.port = std::to_string(p.value_or_default<u32>("connect_port", ston<u32>(res.port)));
            return res;
        };
        act.save = [this](const connect_ui::params_t& params) {
            auto& p = game_settings();
            p.set("connect_host", params.host);
            /* An invalid port is not kept */
            if (auto port = parse_count(params.port); port && *port > 0 && *port <= 65535)
                p.set("connect_port", u32(*port));
            game_settings_commit();
        };
        connect_wnd.emplace(std::move(act));
    }

    void players_remove_all() {
        std::vector<player_name_t> names;
        for (auto& [name, _] : gs.players)
            names.push_back(name);
        for (auto& name : names)
            gs.player_remove(name);
    }

    /* Drop the network client and the players of its game */
    void client_reset() {
        net_cli.reset();
        net_ctrl.reset();
        lobby_wnd.reset();
        if (gs.lobby.active)
            gs.lobby_close();
        gs.lobby.level.clear();
        gs.lobby.mode.clear();
        gs.lobby_networked  = false;
        gs.crates_networked = false;
        client_error.reset();
        client_leave = false;
        players_remove_all();
    }

    /* The connect window's attempt: accepted, rejected or timed out. The lobby's leave */
    void connect_update() {
        if (client_leave) {
            client_reset();
            connect_wnd->show(true);
            return;
        }
        if (!connect_wnd->connecting())
            return;

        if (client_error) {
            auto error = "server: " + *client_error;
            client_reset();
            connect_wnd->failed(std::move(error));
        }
        else if (net_cli && net_cli->joined()) {
            connect_wnd->connected();
        }
        else if (connect_wnd->timed_out()) {
            client_reset();
            connect_wnd->failed("no answer from the server");
        }
    }

    /* An ip or a host name (the socket is IPv4) */
    static std::optional<sf::IpAddress> resolve_host(const std::string& host) {
        if (auto ip = sf::IpAddress::fromString(host))
            return ip;
        if (auto resolved = sf::Dns::resolve(host)) {
            auto v4 = std::ranges::find_if(*resolved, [](const sf::IpAddress& a) { return a.isV4(); });
            if (v4 != resolved->end())
                return *v4;
        }
        return {};
    }

    /* Start joining the server (the client lobby is opened): an error message if the client can't start */
    std::optional<std::string> client_connect(const std::string& host, u16 port, const std::string& name) {
        auto ip = resolve_host(host);
        if (!ip)
            return std::format("invalid server address '{}'", host);

        my_player_name = name;

        /* Load skin, tracer and gun settings from the local player config */
        player_configurator pc{name};
        pc.write_on_delete = false;

        net_cli = std::make_shared<net::net_client>(
            *ip, port, name, pc.pistol, pc.face_id, pc.body_id, pc.head_id, pc.hat_id, pc.body_color, pc.tracer_color);

        if (!net_cli->is_ready()) {
            net_cli.reset();
            return std::format("can't connect to {}:{}", host, port);
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

        net_cli->sig_error.attach_function("mp_error", [this](const std::string& msg) {
            glog().error("multiplayer: join failed: {}", msg);
            /* Handled in game_update: the client can't be destroyed from its own signal */
            if (connect_wnd->connecting())
                client_error = msg;
        });

        init_client_lobby();
        return {};
    }

    void init_client(args_view& args) {
        auto addr_str = args.next("missing <ip:port> after --client");
        auto pos = addr_str.rfind(':');
        if (pos == std::string::npos || pos == 0) {
            glog().error("multiplayer: invalid server address '{}', expected <ip:port>", addr_str);
            return;
        }

        auto host = std::string(addr_str.substr(0, pos));
        auto port = ston<u16>(std::string(addr_str.substr(pos + 1)));
        auto name = args.by_key_default<std::string>("--name", std::string(player_configurator::current_player()));

        if (auto error = client_connect(host, port, name)) {
            glog().error("multiplayer: {}", *error);
            return;
        }

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
        net_cli->input_margin(interp_ms("--input-margin", net::net_client::default_input_margin_s));
    }
};
}

#ifdef _WIN32

#include <windows.h>
#include <tlhelp32.h>

/* Print the faulting address (with the module it lies in) and keep the console open */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    auto addr = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);

    char       mod_name[MAX_PATH] = "?";
    uintptr_t  mod_base          = 0;
    auto       snap              = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    if (snap != INVALID_HANDLE_VALUE && Module32FirstW(snap, &me)) {
        do {
            auto lo = reinterpret_cast<std::uintptr_t>(me.modBaseAddr);
            if (addr >= lo && addr < lo + me.modBaseSize) {
                mod_base = lo;
                WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, mod_name, static_cast<int>(sizeof mod_name) - 1, 0, 0);
                break;
            }
        } while (Module32NextW(snap, &me));
        CloseHandle(snap);
    }

    std::fprintf(stderr, "fatal: exception 0x%08X at %p in module %s (base %p, offset 0x%lx)\n",
                 info->ExceptionRecord->ExceptionCode,
                 static_cast<void*>(info->ExceptionRecord->ExceptionAddress),
                 mod_name,
                 reinterpret_cast<void*>(mod_base),
                 (unsigned long)(mod_base ? addr - mod_base : 0));

    /* stack backtrace: module+offset per frame */
    void* frames[24];
    auto depth = CaptureStackBackTrace(0, 24, frames, nullptr);
    for (auto i = 0u; i < depth; ++i) {
        char       nm[MAX_PATH] = "?";
        std::uintptr_t mb = 0;
        auto       s2     = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        auto       fp     = reinterpret_cast<std::uintptr_t>(frames[i]);
        MODULEENTRY32W m2;
        m2.dwSize = sizeof m2;
        if (s2 != INVALID_HANDLE_VALUE && Module32FirstW(s2, &m2)) {
            do {
                auto lo = reinterpret_cast<std::uintptr_t>(m2.modBaseAddr);
                if (fp >= lo && fp < lo + m2.modBaseSize) {
                    mb = lo;
                    WideCharToMultiByte(CP_UTF8, 0, m2.szModule, -1, nm, static_cast<int>(sizeof nm) - 1, 0, 0);
                    break;
                }
            } while (Module32NextW(s2, &m2));
            CloseHandle(s2);
        }
        std::fprintf(stderr, "  #%u %s+0x%lx\n", i, nm, (unsigned long)(mb ? fp - mb : fp));
    }
    std::fflush(stderr);
    std::system("pause");
    return EXCEPTION_EXECUTE_HANDLER;
}

#endif

void print_usage(const std::string& program) {
    std::cout <<
        program << " [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help              show this help and exit\n"
        "  --physic-debug          physics debug mode\n"
        "  --lua-exec-stdin        execute the whole stdin as lua code after init\n"
        "                          (the same as the in-game `lua` console command)\n"
        "  --lua-exec-fifo=<path>  (re)create a named pipe and execute lua code\n"
        "                          written to it, one chunk per writer close\n"
        "\n"
        "Multiplayer server:\n"
        "  --server                run a multiplayer server\n"
        "  --port=N                server port (default 10443)\n"
        "  --input-buffer=auto|N   client input jitter buffer in ticks\n"
        "\n"
        "Multiplayer client:\n"
        "  --client <ip:port>      connect to a server\n"
        "  --name=<name>           player name (default: the current player, Settings -> Player)\n"
        "  --interp-min=MS         minimum interpolation delay, ms\n"
        "  --interp-margin=MS      interpolation delay margin, ms\n"
        "  --input-margin=MS       input margin, ms\n";
}

int main(int argc, char* argv[]) {
    auto args = dfdh::args_view(argc, argv);
    if (args.get("--help") || args.get("-h")) {
        print_usage(std::string(args.program_name()));
        return EXIT_SUCCESS;
    }

#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#endif
    try {
        return std::make_unique<dfdh::diefastdiehard>()->run(dfdh::args_view(argc, argv));
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
    }
    catch (...) {
        std::fprintf(stderr, "fatal: unknown error\n");
    }
    std::fflush(stderr);
#ifdef _WIN32
    std::system("pause");
#endif
    return 1;
}
