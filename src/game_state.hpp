#pragma once

#include <SFML/Graphics/RectangleShape.hpp>

#include "base/types.hpp"
#include "base/cfg_value_control.hpp"
#include "base/signals.hpp"
#include "ui/player_configurator_ui.hpp"
#include "bullet.hpp"
#include "physic/physic_simulation.hpp"
#include "physic/instant_kick.hpp"
#include "adjustment_box.hpp"
#include "player_controller.hpp"
#include "level.hpp"
#include "ai.hpp"
#include "crate.hpp"
#include "game_mode.hpp"
#include "lobby.hpp"
#include "dropped_gun.hpp"

namespace dfdh {

class game_state {
public:
    game_state():
        blt_mgr("blt_mgr", sim, player_hit_callback),
        kick_mgr("kick_mgr", sim, player_hit_callback),
        conf_watcher(&cfg::mutable_global()),
        dropped_guns(sim, players),
        gravity_for_bullets(bullet_gravity_enabled) {
        sim.add_update_callback("player", [this](const physic_simulation& sim, float timestep) {
            for (auto& [_, p] : players)
                p->physic_update(sim, timestep);
        });

        /* Authoritative per tick history of the players, for retroactive hits */
        sim.add_tick_begin_callback("history", [this](const physic_simulation&, float) { history_tick_begin(); }, 10);
        sim.add_tick_end_callback("history", [this](const physic_simulation&, float) { history_tick_end(); }, 100);

        /* Lag-compensated bullets and guns hit the rewound players, before this tick's new shots */
        sim.add_tick_end_callback(
            "rewound_bullets", [this](const physic_simulation&, float) { rewound_bullets_tick_update(); }, -1);
        sim.add_tick_end_callback(
            "rewound_gun_hits", [this](const physic_simulation&, float) { rewound_gun_hits_tick_update(); }, -1);
        /* Bullets drawn behind (bullet::catch_up_t) catch up, before this tick's new shots */
        sim.add_tick_end_callback(
            "bullets_catch_up", [this](const physic_simulation&, float) { bullets_catch_up_update(); }, -1);

        /* Players (weapons, shots, recoil, death) are updated on every simulation tick */
        sim.add_tick_end_callback("players", [this](const physic_simulation&, float) {
            if (on_game)
                players_tick_update();
        });

        /* A gun hit is a hit like a bullet's: lag-compensated (rewound) for a network thrower */
        dropped_guns.set_collision_handlers(
            [this](const player& pl, float rewind) { return rewound_bb(pl, rewind); },
            [this](dropped_gun& gun, player& victim, const vec2f& impulse, const vec2f& pos) {
                auto ref = bullet_ref{gun.hit_ref_id(), {}, gun.owner()};
                if (gun.rewind() > 0.f)
                    rewound_gun_hits.push_back(
                        {victim.name(), impulse, pos, ref, sim.current_update_time() - to_duration(gun.rewind())});
                else
                    victim.on_bullet_hit(impulse, pos, ref);
            });

        sim.add_platform_callback("player", [this](physic_point* pnt) {
            for (auto& [_, p] : players)
                if (pnt == p->collision_box())
                    p->on_ground(pnt->prev_scalar_velocity());
        });

        if (!game_mode_apply(std::string(game_mode::default_section)))
            game_mode_set(game_mode{.section = std::string(game_mode::default_section)});
    }

    void player_conf_reload(const player_name_t& player_name = {}) {
        if (player_name.empty()) {
            for (auto& [name, p] : players) {
                p->set_from_config(cur_game_mode.magazines_count);
                glog().info("reload player config {}", name);
            }
        } else {
            auto found = players.find(player_name);
            if (found == players.end()) {
                glog().error("can't reload player conf: player {} not found", player_name);
                return;
            }

            found->second->set_from_config(cur_game_mode.magazines_count);
            glog().info("reload player config {}", player_name);
        }
    }

    player* player_create(const player_name_t& player_name) {
        if (players.contains(player_name)) {
            glog().error("player {} already exists", player_name);
            return nullptr;
        }

        auto player = player::create(player_name, sim, player::default_size());
        player->set_from_config(cur_game_mode.magazines_count);
        player->score(cur_game_mode.start_score());
        player->set_drop_gun_cb([this, player_name](const player::drop_gun_spec_t& spec) {
            auto* gun = dropped_guns.spawn(spec.section, spec.position, spec.on_left,
                                           spec.long_shot, spec.group, spec.velocity,
                                           spec.angle_velocity,
                                           spec.angle);
            gun->owner(player_name);
            gun->predicted(spec.predicted);
            /*
             * A network thrower released the gun lag seconds after its screen
             * did, where it saw the others rewind seconds in the past: the gun
             * meets the players as the thrower saw them
             */
            if (auto* thrower = players.contains(player_name) ? players.at(player_name).get() : nullptr;
                thrower && thrower->net_controlled()) {
                auto& lc = thrower->lag_compensation();
                gun->rewind(std::min(lc.lag + lc.rewind, max_lag_compensation));
            }
            return gun;
        });
        auto [it, _] = players.emplace(player_name, player);
        player_spawn(it->second.get());
        return it->second.get();
    }

    /**
     * Player driven from this PC: by an attached controller, or the network
     * client's predicted local player (driven by per-tick input sampling)
     */
    [[nodiscard]]
    bool locally_controlled(const player& pl) const {
        return controlled_players.contains(pl.name()) || pl.predicted();
    }

    [[nodiscard]]
    bool has_controller(u32 controller_id) const {
        return controllers.contains(controller_id);
    }

    bool controller_delete(u32 controller_id) {
        auto found_controller = controllers.find(controller_id);
        if (found_controller == controllers.end()) {
            glog().error("player controller{} was not found", controller_id);
            return false;
        }

        /* The player it drove is no longer controlled */
        std::erase_if(controlled_players,
                      [&](const auto& entry) { return entry.second.controller == found_controller->second; });
        controllers.erase(found_controller);
        glog().info("player controller{} deleted", controller_id);

        return true;
    }

    /**
     * Attach an already-created controller (e.g. a network player controller)
     */
    bool controller_attach(u32 controller_id, const std::shared_ptr<player_controller>& controller) {
        auto found_player = players.find(controller->player_name);
        if (found_player == players.end()) {
            glog().error("failed to attach player controller{}: player {} not found",
                    controller_id,
                    controller->player_name);
            return false;
        }

        auto& c =
            controllers
                .emplace(controller_id, controller)
                .first->second;
        c->player_name = controller->player_name;
        glog().info("player controller{} was attached to player {}", controller_id, controller->player_name);

        return true;
    }

    bool controller_bind(u32 controller_id, const player_name_t& player_name) {
        auto& controller =
            controllers
                .emplace(controller_id, std::make_shared<player_controller>(controller_id))
                .first->second;
        auto found_player = players.find(player_name);
        if (found_player == players.end()) {
            glog().error("failed to bind player controller{}: player {} not found",
                    controller_id,
                    player_name);
            return false;
        }

        controller->player_name = player_name;
        glog().info("player controller{} was bind to player {}", controller_id, player_name);

        return true;
    }

    void rebuild_controlled_players() {
        controlled_players.clear();
        std::vector<u32> to_delete;

        for (auto& [id, controller] : controllers) {
            auto found_player = players.find(controller->player_name);
            if (found_player == players.end()) {
                glog().warn("controller{} maps to non-existent player and will be deleted", id);
                to_delete.push_back(id);
            }
            else {
                controlled_players.emplace(found_player->second->name(),
                                           controll_player_t{controller, found_player->second});
            }
        }

        for (auto& id : to_delete) controllers.erase(id);
    }

    void level_cache_clear() {
        levels.clear();
        glog().info("level cache was cleared");
    }

    bool level_current(const std::string& level_name = {}) {
        if (level_name.empty()) {
            if (!cur_level) {
                glog().info("current level already reseted");
            } else {
                cur_level.reset();
                crates.clear();
                crate_timer.restart();
                glog().info("current level has been reseted");
            }
            return true;
        }

        if (cur_level && cur_level->section_name() == level_name) {
            glog().info("level {} already in use", level_name);
            return true;
        }

        if (!levels.contains(level_name) && !level_cache(level_name))
            return false;

        cur_level = levels.at(level_name);
        level_setup(level_name);

        glog().info("level {} was loaded", level_name);
        return true;
    }

    /* Apply the current level to the simulation and restart the level-scoped state */
    void level_setup(const std::string& level_name) {
        cur_level->setup_to(sim);
        crates.clear();
        crate_timer.restart();
        sig_level_changed.emit_deferred(level_name);
    }

    /* Game mode */

    /* Apply the game mode from its gm_ config section ("default" or "gm_default") */
    bool game_mode_apply(const std::string& name) {
        try {
            game_mode_set(game_mode::load(name));
            return true;
        }
        catch (const cfg_exception& e) {
            glog().error("game mode {} failed: {}", name, e.what());
            return false;
        }
    }

    /* Apply the game mode rules (the network client applies the server's game mode this way) */
    void game_mode_set(const game_mode& gm) {
        bool reset_scores = !cur_game_mode.same_score_rules(gm);
        cur_game_mode = gm;
        sim.bullet_gravity({0.f, gm.bullet_gravity});
        gravity_for_bullets = gm.bullet_gravity_enabled();
        crate_timer.restart();
        if (reset_scores)
            players_score_reset();
        glog().info("game mode {} was applied (score system: {})",
                    gm.section,
                    game_mode::score_system_name(gm.score_system));
    }

    /* Everyone starts over: the game mode's start score, eliminated players are back in the game */
    void players_score_reset() {
        for (auto& [_, pl] : players)
            pl->score(cur_game_mode.start_score());
        players_elimination_update();
    }

    /* Lobby */

    /* The lobby phase: the game stops until lobby_start (the current level and game mode are the default choice) */
    void lobby_open() {
        lobby.active = true;
        lobby.ready.clear();
        lobby_default_choice();
        glog().info("lobby opened");
    }

    /* No choice yet: the current level and game mode (e.g. the level is loaded by the script after the lobby opened) */
    void lobby_default_choice() {
        if (lobby_networked)
            return;
        if (lobby.level.empty() && cur_level)
            lobby.level = cur_level->section_name();
        if (lobby.mode.empty())
            lobby.mode = cur_game_mode.section;
    }

    /* Load the chosen level, apply the chosen game mode and spawn everyone with the start score */
    bool lobby_start() {
        bool same_level = cur_level && cur_level->section_name() == lobby.level;
        if (lobby.level.empty() || !level_current(lobby.level)) {
            glog().error("lobby: can't start: no level");
            return false;
        }
        /* The same level: restart its state anyway */
        if (same_level)
            level_setup(lobby.level);

        if (!lobby.mode.empty())
            game_mode_apply(lobby.mode);
        players_score_reset();

        dropped_guns.guns().clear();
        for (auto& b : blt_mgr.bullets())
            b.physic()->delete_later();

        player_spawn();

        lobby.active = false;
        lobby.ready.clear();
        game_run(true);
        glog().info("lobby: game started on {} ({})", lobby.level, cur_game_mode.section);
        return true;
    }

    void lobby_player_group(const player_name_t& name, int group) {
        if (auto found = players.find(name); found != players.end())
            found->second->group(group);
    }

    bool level_cache(const std::string& level_name) {
        try {
            auto [_, was_insert] = levels.emplace(level_name, level::create(level_name));
            if (was_insert)
                glog().info("level {} was cached", level_name);
            else
                glog().info("level {} already cached", level_name);
            return true;
        }
        catch (const std::exception& e) {
            glog().error("level cache {} failed: {}", level_name, e.what());
            return false;
        }
    }

    void game_run(bool value = true) {
        if (on_game)
            glog().info(value ? "game already running" : "game stopped");
        else
            glog().info(value ? "game running" : "game already stopped");
        on_game = value;
    }

    void game_stop() {
        game_run(false);
    }

    /*
     * Fell off the map. Default score system: a kill of the last attacker
     * (+1 to the attacker), a suicide without one (-1 to the player).
     * Lives score system: -1 life for any death, out of the game without lives
     */
    void player_on_dead(player* player) {
        if (cur_game_mode.lives_enabled()) {
            player->add_score(-1);
            player->increase_deaths();
            if (!player->attacker().empty())
                glog().info("{} knocked out {} (lives {})", player->attacker(), player->name(), player->score());
            else
                glog().info("{} fell off (lives {})", player->name(), player->score());

            if (player->score() <= 0)
                player_eliminate(player);
            else
                player_spawn(player);
            return;
        }

        auto found = players.find(player->attacker());
        if (!player->attacker().empty() && found != players.end() && found->second.get() != player) {
            found->second->add_score(1);
            glog().info("{} knocked out {} (score {})", found->first, player->name(), found->second->score());
        }
        else {
            player->add_score(-1);
            glog().info("{} fell off (score {})", player->name(), player->score());
        }

        player->increase_deaths();
        player_spawn(player);
    }

    /* Eliminated players are kept far away from the level */
    static constexpr vec2f eliminated_park_pos = {-100000.f, -100000.f};

    /* Lives score system: an out of lives player is out of the game until the scores reset */
    [[nodiscard]]
    bool player_out_of_lives(const player& pl) const {
        return cur_game_mode.lives_enabled() && pl.score() <= 0;
    }

    /*
     * Out of the game: parked away from the level without gravity, not updated,
     * drawn or respawned (server-driven: parked by the server)
     */
    void player_eliminate(player* pl) {
        pl->eliminated(true);
        pl->collision_box()->enable_gravity(false);
        if (!pl->server_driven()) {
            pl->teleport(eliminated_park_pos);
        }
        glog().info("{} is out of the game", pl->name());
    }

    void player_revive(player* pl) {
        pl->eliminated(false);
        pl->collision_box()->enable_gravity(true);
        if (!pl->server_driven())
            player_spawn(pl);
        glog().info("{} is back in the game", pl->name());
    }

    /* Eliminates or revives players by their scores (network client: by the synced scores) */
    void players_elimination_update() {
        for (auto& [_, pl] : players) {
            bool out = player_out_of_lives(*pl);
            if (out && !pl->eliminated())
                player_eliminate(pl.get());
            else if (!out && pl->eliminated())
                player_revive(pl.get());
        }
    }

    void player_spawn(player* player = nullptr) {
        if (!cur_level) {
            glog().warn("Can't spawn players: level was not loaded");
            return;
        }

        if (player) {
            if (player->eliminated())
                return;
            /* A player spawns with its secondary gun only (server-driven: synced from the server) */
            if (!player->server_driven())
                player->equip_secondary();
            player->teleport(cur_level->pick_player_spawn());
            player->enable_double_jump();
        } else {
            for (auto& [_, p] : players)
                player_spawn(p.get());
        }
    }

    bool ai_bind(const player_name_t& player_name) {
        if (ai_operators.contains(player_name)) {
            glog().warn("player {} is already ai controled", player_name);
            return false;
        }

        if (!players.contains(player_name)) {
            glog().error("player {} not found", player_name);
            return false;
        }

        ai_operators.emplace(player_name, ai_operator::create(player_name, "medium"));

        if (!ai_mgr().running()) {
            ai_provide_level_info();
            ai_provide_player_level_sim_info();
            ai_mgr().worker_start();
        }
        return true;
    }

    /* Game updates */

    void handle_event(const sf::Event& evt) {
        if (!on_game)
            return;

        for (auto& [_, control_data] : controlled_players) {
            auto& [controller, this_player] = control_data;
            /* A server-driven player (network client) gets its input from the server only */
            if (!this_player->server_driven())
                this_player->update_input(*controller, evt);
        }

        if (cfgval_ctrl) {
            cfgval_ctrl->handle_event(evt);
            if (auto section_name = cfgval_ctrl->consume_update())
                reload_section(*section_name);
        }
    }

    void reload_section(const std::string& section_name) {
        try {
            if (section_name.starts_with("wpn_")) {
                /* The changed section is a weapon or one of its animations (wpn_<name>_<anim>) */
                for (auto& [name, section] : cfg::global().get_sections()) {
                    if (section_name.starts_with(name) && section.has_key("class")) {
                        weapon_mgr().reload(section_name);
                        return;
                    }
                }
            }
            else if (section_name.starts_with("lvl_")) {
                auto found_lvl = levels.find(section_name);
                if (found_lvl != levels.end()) {
                    found_lvl->second->cfg_reload();
                    if (cur_level == found_lvl->second)
                        level_setup(section_name);
                }
            }
            else if (section_name == cur_game_mode.section) {
                game_mode_apply(section_name);
            }
        }
        catch (const cfg_exception& e) {
            glog().error("reload section [{}] failed: {}", section_name, e.what());
        }
    }

    void render_update(sf::RenderWindow& wnd) {
        if (!cur_level)
            return;

        cur_level->draw(wnd);

        for (auto& [_, player] : players)
            if (!player->eliminated())
                player->draw(wnd, sim.interpolation_factor(), sim.last_timestep(), gravity_for_bullets);

        for (auto& crate_ : crates)
            crate_->draw(wnd, sim.interpolation_factor(), sim.last_timestep());

        dropped_guns.draw(wnd, sim.interpolation_factor(), sim.last_timestep());

        blt_mgr.draw(wnd, sim.interpolation_factor(), sim.last_timestep());

        if (debug_physics) {
            for (auto& e : sim.point_primitives()) {
                for (auto p : group_tree_view(e.get())) {
                    sf::RectangleShape s;
                    s.setPosition(p->bb().getPosition());
                    s.setSize({p->bb().width, std::max(p->bb().height, 2.f)});
                    s.setFillColor(sf::Color(0, 255, 0));
                    wnd.draw(s);
                }
            }

            for (auto& e : sim.line_primitives()) {
                for (auto p : group_tree_view(e.get())) {
                    sf::RectangleShape s;
                    s.setPosition(p->bb().getPosition());
                    s.setSize(p->bb().getSize());
                    s.setOutlineColor(sf::Color(0, 255, 0));
                    s.setOutlineThickness(1.f);
                    wnd.draw(s);
                }
            }

            for (auto& p : sim.platforms()) {
                sf::Vertex line[] = {
                    sf::Vertex(p.get_position()),
                    sf::Vertex(p.get_position() + vec2f(p.length(), 0.f)),
                };
                wnd.draw(line, 2, sf::Lines);
            }
        }
    }

    void ui_update(ui_ctx& ui, const vec2f& window_size) {
        if (on_game && !lobby.active) {
            ammo_hud_draw(ui, window_size);
            score_hud_draw(ui, window_size);
        }
        if (pconf_ui)
            pconf_ui->update(ui);
    }

    /* Ammo of the locally controlled players' guns at the bottom right corner: magazine/reserve */
    void ammo_hud_draw(ui_ctx& ui, const vec2f& window_size) {
        std::vector<std::pair<std::string, std::string>> ammo; /* player name, magazine/reserve */
        for (auto& [name, pl] : players) {
            auto& gun = pl->get_gun();
            if (!gun || !locally_controlled(*pl))
                continue;
            auto reserve = gun.ammo_reserve() == weapon_instance::unlimited_ammo ? std::string("\u221E")
                                                                                 : std::to_string(gun.ammo_reserve());
            ammo.emplace_back(std::string(name), std::to_string(gun.ammo_elapsed()) + "/" + reserve);
        }
        if (ammo.empty())
            return;

        /* Several local players: tell them apart by name */
        std::vector<std::string> lines;
        for (auto& [name, text] : ammo)
            lines.push_back(ammo.size() > 1 ? name + ": " + text : text);

        auto height = hud_row_height * float(lines.size()) + 2.f * hud_margin;
        hud_text_draw(ui,
                      "ammo_hud",
                      nk_rect(window_size.x - hud_width - hud_margin, window_size.y - height, hud_width, height),
                      lines);
    }

    /* Scores of all players at the top right corner, best first */
    void score_hud_draw(ui_ctx& ui, const vec2f& window_size) {
        std::vector<const player*> sorted;
        for (auto& [_, pl] : players)
            sorted.push_back(pl.get());
        if (sorted.empty())
            return;
        std::ranges::stable_sort(sorted, std::greater{}, [](const player* pl) { return pl->score(); });

        std::vector<std::string> lines;
        for (auto* pl : sorted)
            lines.push_back(std::string(pl->name()) + ": " + (pl->eliminated() ? "out" : std::to_string(pl->score())));

        auto height = hud_row_height * float(lines.size()) + 2.f * hud_margin;
        hud_text_draw(ui, "score_hud", nk_rect(window_size.x - hud_width - hud_margin, hud_margin, hud_width, height), lines);
    }

    static constexpr int   hud_outline    = 3; /* black outline thickness, px */
    static constexpr float hud_row_height = ui_ctx::hud_font_size + 2.f * float(hud_outline);
    static constexpr float hud_width      = 400.f;
    static constexpr float hud_margin     = 12.f;

    /* Right-aligned HUD lines, white with a black outline, in a transparent window */
    static void hud_text_draw(ui_ctx& ui, const char* window_name, struct nk_rect bounds, const std::vector<std::string>& lines) {
        auto style = &ui.nk_ctx()->style;
        ui.style_push_style_item(&style->window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
        ui.style_push_vec2(&style->window.padding, {0.f, 0.f});
        ui.style_push_vec2(&style->window.spacing, {0.f, 0.f});

        /* Nuklear takes the bounds of a new window only: keep it in place (window resize, more lines) */
        ui.window_set_bounds(window_name, bounds);
        if (ui.begin(window_name, bounds, NK_WINDOW_NO_INPUT | NK_WINDOW_NO_SCROLLBAR)) {
            auto* font   = ui.hud_font();
            auto* canvas = ui.window_get_canvas();
            ui.layout_row_dynamic(hud_row_height, 1);
            for (auto& line : lines) {
                struct nk_rect row;
                if (nk_widget(&row, ui.nk_ctx()) == NK_WIDGET_INVALID)
                    continue;

                /* Right-aligned: the text is drawn several times around its place for the outline */
                constexpr auto outline = hud_outline;
                auto len   = int(line.size());
                auto tw    = font->width(font->userdata, font->height, line.data(), len);
                auto place = nk_rect(row.x + row.w - tw - float(outline), row.y + float(outline), tw + 1.f, font->height);
                for (int dx = -outline; dx <= outline; ++dx)
                    for (int dy = -outline; dy <= outline; ++dy)
                        if ((dx != 0 || dy != 0) && dx * dx + dy * dy <= outline * outline + 1)
                            nk_draw_text(canvas,
                                         nk_rect(place.x + float(dx), place.y + float(dy), place.w, place.h),
                                         line.data(), len, font, nk_rgba(0, 0, 0, 0), nk_rgba(0, 0, 0, 255));
                nk_draw_text(canvas, place, line.data(), len, font, nk_rgba(0, 0, 0, 0), nk_rgba(255, 255, 255, 255));
            }
        }
        ui.end();

        ui.style_pop_vec2();
        ui.style_pop_vec2();
        ui.style_pop_style_item();
    }

    /**
     * Bullets spawned during the last game_update() frame,
     * collected for the server to broadcast to clients
     */
    struct bullet_event_t {
        player_name_t owner;
        u32           id;           // bullet net_id (no live bullet if the hit-scan consumed it), 0 if predicted
        shot_key_t    key;          // deterministic shot key (network players)
        vec2f         start;        // muzzle position
        vec2f         position;     // spawn position (lag-forwarded)
        vec2f         velocity;     // spawn velocity (lag-forwarded)
        vec2f         muzzle_velocity;
        float         forward;      // seconds it was forwarded by (position, velocity = ballistic_forward(start, muzzle_velocity, forward))
        float         rewind;       // seconds the players it is tested against are rewound by (lag compensation)
        float         hit_mass;
        sf::Color     tracer_color;
        bool          gravity; // gravity flag the bullet was spawned with

        /* Predicted bullet (client side, own bullets): not authoritative, no net id yet */
        bool                        predicted = false;
        std::weak_ptr<physic_point> physic;
    };

    std::vector<bullet_event_t> cur_frame_bullets;

    /* Spawns the bullets of a player's weapon through spawn_bullet() */
    struct bullet_spawn_callback {
        void operator()(const vec2f& position, const vec2f& velocity, float mass) {
            if (!pl->server_driven() || pl->predicted())
                gs->spawn_bullet(*pl, position, velocity, mass, gravity);
        }

        constexpr operator bool() const {
            return true;
        }

        game_state* gs;
        player*     pl;
        bool        gravity;
    };

    /* Lag compensation is capped: a client can't shoot further in the past than this */
    static constexpr float max_lag_compensation = 1.f;

    struct hitscan_hit_t {
        player* target;
        vec2f   position;
    };

    /**
     * Nearest player hit by the segment a -> b, testing every player at the
     * position it had rewind_s seconds ago (position trace). The owner and,
     * like for bullets, the players of the owner's group are skipped
     */
    std::optional<hitscan_hit_t> hitscan(const vec2f& a, const vec2f& b, float rewind_s, const player& owner) {
        return hitscan(a, b, rewind_s, &owner, owner.get_group());
    }

    /* Player box rewind_s seconds ago (position trace), the current one if there is no trace back then */
    [[nodiscard]]
    sf::FloatRect rewound_bb(const player& pl, float rewind_s) const {
        auto bb = pl.bb();
        if (rewind_s > 0.f)
            if (auto past = pl.position_trace_lookup(sim.current_update_time(), rewind_s)) {
                bb.left = past->x;
                bb.top  = past->y - bb.height;
            }
        return bb;
    }

    /* Owner may be gone (nullptr): only its group is skipped then */
    std::optional<hitscan_hit_t> hitscan(const vec2f& a, const vec2f& b, float rewind_s, const player* owner, int group) {
        return hitscan_if(a, b, rewind_s, owner, group, [](const player&) { return true; });
    }

    /* Like hitscan, the players target(player) rejects are skipped too */
    template <typename F>
    std::optional<hitscan_hit_t>
    hitscan_if(const vec2f& a, const vec2f& b, float rewind_s, const player* owner, int group, F&& target) {
        std::optional<hitscan_hit_t> res;
        float                        best_t = std::numeric_limits<float>::max();
        auto                         d      = b - a;

        for (auto& [_, pl] : players) {
            if (pl.get() == owner || !target(*pl))
                continue;
            if (group != -1 && pl->get_group() != -1 && pl->get_group() == group)
                continue;

            if (auto t0 = segment_hit(a, b, rewound_bb(*pl, rewind_s)); t0 && *t0 < best_t) {
                best_t = *t0;
                res    = hitscan_hit_t{pl.get(), a + d * *t0};
            }
        }
        return res;
    }

    /* Where the segment a -> b enters the box: a + (b - a) * t, t in [0, 1] */
    [[nodiscard]]
    static std::optional<float> segment_hit(const vec2f& a, const vec2f& b, const sf::FloatRect& bb) {
        auto d = b - a;

        /* Segment vs AABB (slabs) */
        float t0 = 0.f, t1 = 1.f;
        auto  slab = [&](float origin, float dir, float min, float max) {
            if (std::abs(dir) < 1e-6f)
                return origin >= min && origin <= max;
            auto ta = (min - origin) / dir;
            auto tb = (max - origin) / dir;
            t0      = std::max(t0, std::min(ta, tb));
            t1      = std::min(t1, std::max(ta, tb));
            return t0 <= t1;
        };
        if (slab(a.x, d.x, bb.left, bb.left + bb.width) && slab(a.y, d.y, bb.top, bb.top + bb.height))
            return t0;
        return {};
    }

    /**
     * Spawn a bullet of the owner's weapon. A lag-compensated (network) owner
     * shot lag seconds ago on its screen: the bullet spawns forwarded by the
     * distance it would have flown since, and the skipped path is swept
     * against the players as the owner saw them while the bullet flew it:
     * the path at flight f against the players rewind + lag - f ago (its
     * view time + f, like rewound_bullets_tick_update sweeps the rest).
     * A server-driven owner's bullet is a prediction (client side): it gets
     * no net id until the authoritative bullet is matched to it
     */
    void spawn_bullet(player& owner, const vec2f& start, const vec2f& velocity, float mass, bool gravity) {
        auto lag    = std::min(owner.lag_compensation().lag, max_lag_compensation);
        auto rewind = std::min(owner.lag_compensation().rewind, max_lag_compensation);
        auto key    = next_shot_key(owner);

        if (owner.server_driven()) {
            auto& blt = blt_mgr.shot(sim, start, mass, velocity, owner.tracer_color(), gravity, owner.get_group(),
                                     player::player_group_getter, player::owner_checker(&owner));
            blt.predicted_key(key);
            /* Its hits on the remote players are tested like the server does (net_client) */
            blt.frame_on_client({start, velocity, gravity, 0.f, 0.f, 0.f, true}, start);
            cur_frame_bullets.push_back(
                {owner.name(), 0, key, start, start, velocity, velocity, 0.f, 0.f, mass, owner.tracer_color(), gravity, true, blt.physic()});
            return;
        }

        auto id = ++_next_bullet_id;

        auto [pos, vel] = ballistic_forward(start, velocity, gravity, lag);

        if (lag > 0.f) {
            /* Tick by tick, ending at flight f1 (like a tick of the rewound sweep) */
            auto tick_s = std::chrono::duration<float>(sim.tick_period()).count();
            for (float f0 = 0.f; f0 < lag; f0 += tick_s) {
                auto f1      = std::min(f0 + tick_s, lag);
                auto a       = ballistic_forward(start, velocity, gravity, f0).first;
                auto [b, bv] = ballistic_forward(start, velocity, gravity, f1);
                auto back    = rewind + (lag - f1);
                if (auto hit = hitscan(a, b, back, owner)) {
                    apply_hit_at(*hit->target, bv * mass, hit->position, bullet_ref{id, {}, owner.name()},
                                 sim.current_update_time() - to_duration(back));
                    cur_frame_bullets.push_back(
                        {owner.name(), id, key, start, start, velocity, velocity, 0.f, 0.f, mass, owner.tracer_color(), gravity, false, {}});
                    return;
                }
            }
        }

        /*
         * Not only with a lag: one shorter than a tick (rounded down, a short
         * input queue on a fast link) still has the targets rewound by the
         * owner's view. Uncompensated, it hit them where they were rewind
         * later than the owner saw them
         */
        auto compensated = lag > 0.f || rewind > 0.f;
        auto& blt = blt_mgr.shot(sim, pos, mass, vel, owner.tracer_color(), gravity, owner.get_group(),
                                 player::player_group_getter, player::owner_checker(&owner));
        blt.owner(owner.name());
        blt.net_id(id);
        if (compensated)
            blt.lag_compensate(owner.name(), rewind);
        /* The owner is shown firing now: drawn from the muzzle */
        if (lag > 0.f) {
            blt.catch_up() = bullet::catch_up_t{lag};
            bullet_catch_up_update(blt, false);
        }
        cur_frame_bullets.push_back({owner.name(), id, key, start, pos, vel, velocity, lag, compensated ? rewind : 0.f, mass,
                                     owner.tracer_color(), gravity, false, {}});
    }

    /*
     * How long a bullet drawn behind (catch-up, s) at pos flying vel may stay
     * behind: it is gone 30 px before a player it may hit ahead on its path
     * (not the owner nor its group, nor skip), so the bullet reaches the
     * player when the hit shows up. Shortened over 2x the distance: drawn at
     * most 1.5x faster meanwhile
     */
    [[nodiscard]]
    float bullet_catch_up_limit(float catch_up, const vec2f& pos, const vec2f& vel, const player* owner, int group,
                                const player* skip = nullptr) const {
        constexpr float near   = 30.f;  /* px */
        constexpr float aside  = 150.f; /* px: passing further aside, the player is not its target */
        constexpr float span_k = 2.f;

        auto speed = magnitude(vel);
        if (catch_up <= 0.f || speed < 1.f)
            return catch_up;
        auto dir = vel / speed;
        for (auto& [_, p] : players) {
            if (p.get() == skip || p.get() == owner || p->eliminated() ||
                (group != -1 && p->get_group() != -1 && p->get_group() == group))
                continue;
            auto size   = p->get_size();
            auto to     = p->get_position() + vec2f{size.x * 0.5f, -size.y * 0.5f} - pos;
            auto along  = to.x * dir.x + to.y * dir.y;
            auto across = std::abs(to.x * dir.y - to.y * dir.x);
            if (along >= 0.f && across <= aside)
                catch_up = std::min(catch_up, std::max(along - near, 0.f) / (span_k * speed));
        }
        return catch_up;
    }

    /* Where a bullet drawn behind is drawn until the next tick; tick: it caught up for a tick first */
    void bullet_catch_up_update(bullet& blt, bool tick) {
        auto& cu = blt.catch_up();
        auto& ph = blt.physic();
        auto  dt = std::chrono::duration<float>(sim.tick_period()).count();
        if (tick)
            cu->behind -= (bullet::catch_up_speed - 1.f) * dt;

        auto found = players.find(blt.owner());
        auto owner = found != players.end() ? found->second.get() : nullptr;
        cu->behind = bullet_catch_up_limit(cu->behind, ph->get_position(), ph->get_velocity(), owner, blt.group());
        if (cu->behind <= 0.f) {
            cu.reset();
            return;
        }

        auto [drawn_pos, drawn_vel] =
            ballistic_forward(ph->get_position(), ph->get_velocity(), ph->is_gravity_enabled(), -cu->behind);
        cu->gap      = drawn_pos - ph->get_position();
        cu->gap_vel  = drawn_vel - ph->get_velocity();
        cu->step_vel = drawn_vel * (std::min(cu->behind, (bullet::catch_up_speed - 1.f) * dt) / dt);
    }

    void bullets_catch_up_update() {
        for (auto& blt : blt_mgr.bullets())
            if (blt.catch_up() && !blt.physic()->ready_delete_later())
                bullet_catch_up_update(blt, true);
    }

    /**
     * Sweep the path every lag-compensated bullet flew this tick against the
     * players as its shooter saw them (rewound), for the bullet's whole flight
     */
    void rewound_bullets_tick_update() {
        for (auto& blt : blt_mgr.bullets()) {
            auto& ph = blt.physic();
            if (blt.rewind() <= 0.f || ph->ready_delete_later())
                continue;

            auto [from, to] = blt.take_swept_segment();
            auto found      = players.find(blt.owner());
            auto owner      = found != players.end() ? found->second.get() : nullptr;
            if (auto hit = hitscan(from, to, blt.rewind(), owner, blt.group())) {
                apply_hit_at(*hit->target, ph->impulse(), hit->position, bullet_ref::of(ph.get()),
                             sim.current_update_time() - to_duration(blt.rewind()));
                ph->delete_later();
            }
        }
    }

    /* A rewound gun hit, applied at the tick end (apply_hit_at re-simulates the victim) */
    struct rewound_gun_hit_t {
        player_name_t                         victim;
        vec2f                                 impulse;
        vec2f                                 position;
        bullet_ref                            ref;
        std::chrono::steady_clock::time_point when;
    };

    std::vector<rewound_gun_hit_t> rewound_gun_hits;

    void rewound_gun_hits_tick_update() {
        for (auto& hit : std::exchange(rewound_gun_hits, {}))
            if (auto found = players.find(hit.victim); found != players.end())
                apply_hit_at(*found->second, hit.impulse, hit.position, hit.ref, hit.when);
    }

    /*
     * Authoritative history: per player and tick, the movement controls in
     * effect, the impulses applied (hits before the movement step, others
     * after it) and the state at the end of the tick
     */
    struct tick_record_t {
        std::chrono::steady_clock::time_point time; /* the tick's (scheduled) time */
        u16                                   teleports;
        player::controls_t                    controls;
        player::tick_impulses_t               impulses;
        std::optional<player::physic_state>   end; /* empty while the tick runs */
    };

    static constexpr size_t history_ticks = physic_simulation::tick_rate * 3 / 2; /* > max_lag_compensation */
    static_assert(float(history_ticks) * physic_simulation::tick_s > max_lag_compensation);

    std::map<player_name_t, std::deque<tick_record_t>> history;

    static std::chrono::steady_clock::duration to_duration(float seconds) {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(seconds));
    }

    /* Tick begin, after the network inputs: the controls for the tick */
    void history_tick_begin() {
        std::erase_if(history, [this](const auto& h) { return !players.contains(h.first); });
        for (auto& [name, pl] : players) {
            if (pl->server_driven())
                continue;
            auto& h = history[name];
            /* No history across a teleport (respawn) */
            if (!h.empty() && h.back().teleports != pl->teleports())
                h.clear();
            pl->begin_tick_record();
            h.push_back({sim.current_update_time(), pl->teleports(), pl->controls(), {}, {}});
            if (h.size() > history_ticks)
                h.pop_front();
        }
    }

    /* Tick end, after everything: the impulses and the state */
    void history_tick_end() {
        for (auto& [name, h] : history) {
            auto found = players.find(name);
            if (h.empty() || found == players.end())
                continue;
            h.back().impulses = found->second->take_tick_impulses();
            h.back().end      = found->second->get_physic_state();
        }
    }

    /**
     * A lag-compensated hit lands where the shooter saw it: at time when, in
     * the past. The victim's recorded state at that moment gets the hit and
     * the victim is re-simulated to now with its recorded controls and
     * impulses (its history is rewritten), so the shooter's prediction and
     * the server agree. Applied now if there is no history back then.
     * The jump to the rewritten present is drawn smoothly (render offset):
     * on the server's screen the victim is shown in the present, so it would
     * teleport by the knockback of the rewound time
     */
    void apply_hit_at(player& victim, const vec2f& impulse, const vec2f& hit_position, const bullet_ref& ref,
                      std::chrono::steady_clock::time_point when) {
        victim.hit_feedback(impulse, hit_position, ref);

        auto found = history.find(victim.name());
        if (found == history.end()) {
            victim.apply_hit_physics(impulse);
            return;
        }
        auto& h = found->second;

        /* The last finished tick at or before the hit; the hit goes to the next one, before its movement */
        std::optional<size_t> from;
        for (size_t i = h.size(); i-- > 0;)
            if (h[i].end && h[i].time <= when && h[i].teleports == victim.teleports()) {
                from = i;
                break;
            }
        if (!from || *from + 1 >= h.size()) {
            victim.apply_hit_physics(impulse); /* no history back then, or not in the past */
            return;
        }

        /* A running tick's impulses are still on the player (taken at the tick end) */
        auto impulses_of = [&](tick_record_t& r) -> player::tick_impulses_t& {
            return r.end ? r.impulses : victim.pending_tick_impulses();
        };
        auto& hit_list = impulses_of(h[*from + 1]).before_move;
        hit_list.insert(hit_list.begin(), {true, impulse});

        auto live_controls = victim.controls();
        auto timestep      = sim.last_timestep();
        auto old_pos       = victim.get_position();

        victim.restore_physic_state(*h[*from].end);
        victim.replaying(true);
        for (size_t i = *from + 1; i < h.size(); ++i) {
            auto& r = h[i];
            victim.controls(r.controls);
            for (auto& imp : impulses_of(r).before_move)
                victim.apply_tick_impulse(imp);
            sim.step_isolated(victim.collision_box(), timestep, [&](float ts) { victim.physic_update(sim, ts); });
            for (auto& imp : impulses_of(r).after_move)
                victim.apply_tick_impulse(imp);

            if (r.end)
                r.end = victim.get_physic_state();
            victim.rewrite_position_trace(r.time, victim.get_position());
        }
        victim.replaying(false);
        victim.controls(live_controls);
        victim.place_legs();
        victim.add_render_offset(old_pos - victim.get_position());
    }

    /* Key of the owner's bullet being spawned: pellets of one shot are numbered in spawn order */
    [[nodiscard]]
    shot_key_t next_shot_key(const player& owner) const {
        auto& gun = owner.get_gun();
        auto  key = shot_key_t{gun.seed_base().value_or(0), gun.shot_index(), 0};
        for (auto& be : cur_frame_bullets)
            if (be.owner == owner.name() && be.key.pull_seq == key.pull_seq && be.key.shot_index == key.shot_index)
                ++key.pellet;
        return key;
    }

    /* Bullet position and velocity after flying for t seconds */
    [[nodiscard]]
    std::pair<vec2f, vec2f> ballistic_forward(const vec2f& pos, const vec2f& vel, bool gravity, float t) const {
        auto g = gravity ? sim.bullet_gravity() : vec2f{0.f, 0.f};
        return {pos + vel * t + g * (0.5f * t * t), vel + g * t};
    }

    void game_update() {
        /* Try reload watched sections */
        for (auto& section_name : cfg::mutable_global().replace_changed_sections())
            reload_section(section_name);

        if (lobby.active)
            lobby_default_choice();

        /* Nothing is simulated in the lobby */
        if (on_game && !lobby.active) {
            if (cur_level)
                dropped_guns.set_level_size(cur_level->level_size());

            cur_frame_bullets.clear();
            sim.update(physic_simulation::tick_rate, game_speed); /* players_tick_update() on every tick */

            kick_mgr.update();
            adj_box_mgr.update(sim);

            crates_update();

            if (!ai_operators.empty())
                ai_operators_consume();

            if (ai_profiler_enabled)
                ai_mgr().profiler_print([](auto& prof) {
                    glog().detail(prof.short_print_format() ? "ai worker: {}" : "ai_worker: min|max|avg: {}", prof);
                });
        }
        else {
            sim.update_pass();
        }
    }

    /*
     * Players update at the end of a simulation tick: weapons fire on ticks,
     * so a network player's shots (and recoil) land on the same input ticks
     * on the server and in the client's prediction
     */
    void players_tick_update() {
        players_elimination_update();

        for (auto& [_, pl] : players) {
            /* Out of the game: stays parked */
            if (pl->eliminated()) {
                if (!pl->server_driven()) {
                    pl->position(eliminated_park_pos);
                    pl->velocity({0.f, 0.f});
                }
                continue;
            }

            if (controlled_players.contains(pl->name()) || ai_operators.contains(pl->name()) || pl->net_controlled() ||
                pl->predicted())
                pl->game_update(sim,
                                blt_mgr,
                                cam_pos,
                                gravity_for_bullets,
                                false, /* bullets are spawned by the callback */
                                bullet_spawn_callback{this, pl.get(), gravity_for_bullets});
            else
                pl->game_update(sim, blt_mgr, cam_pos, gravity_for_bullets, false, {});
        }

        /* Server-driven players die and respawn on the server */
        for (auto& [_, pl] : players) {
            if (pl->server_driven() || pl->eliminated())
                continue;
            pl->update_attacker();
            if (pl->collision_box()->get_position().y > 2100.f)
                player_on_dead(pl.get());
        }
    }

    /* Crates */

    static constexpr float crate_spawn_height = 200.f; /* above the most top platform */

    void crates_clear() {
        crates.clear();
    }

    /* Creates a server-driven (ghost) crate on the client side */
    crate* crate_create_net() {
        auto c = std::make_unique<crate>(sim, vec2f{0.f, 0.f}, 0.f);
        c->net_sync(true);
        auto* ptr = c.get();
        crates.push_back(std::move(c));
        return ptr;
    }

    void crates_update() {
        if (!cur_level) {
            crates.clear();
            return;
        }

        /* On the client the server is authoritative for crates */
        if (crates_networked)
            return;

        /* Drop period is defined by the game mode; no period means crates never drop */
        if (cur_game_mode.crates_enabled() && crate_timer.elapsed() >= cur_game_mode.crate_drop_period) {
            crates_spawn();
            crate_timer.restart();
        }

        for (auto& c : crates)
            c->update();

        /* Pick up: the first player overlapping a crate takes it */
        for (auto& [_, pl] : players) {
            auto pbb = pl->bb();

            for (auto it = crates.begin(); it != crates.end();) {
                /* Net-synced crates are picked up on the server */
                if ((*it)->net_synced() || !(*it)->bb().intersects(pbb)) {
                    ++it;
                    continue;
                }

                if (auto wpn = crate_loot_table::random_pick()) {
                    pl->setup_pistol(*wpn, cur_game_mode.magazines_count);
                    glog().info("player {} picked up a crate and got weapon {}", pl->name(), *wpn);
                } else
                    glog().warn("player {} picked up a crate but the loot table is empty", pl->name());

                it = crates.erase(it);
            }
        }
    }

    void crates_spawn() {
        auto& platforms = cur_level->get_platforms();
        if (platforms.empty()) {
            glog().warn("can't spawn crate: level has no platforms");
            return;
        }

        /* Map horizontal bounding box and the most top platform */
        float min_x = std::numeric_limits<float>::max();
        float max_x = -std::numeric_limits<float>::max();
        float top_y = std::numeric_limits<float>::max();
        for (auto& p : platforms) {
            auto pos = p.ph.get_position();
            min_x = std::min(min_x, pos.x);
            max_x = std::max(max_x, pos.x + p.ph.length());
            top_y = std::min(top_y, pos.y);
        }

        /* Random x inside the bounding box, trace platforms under it */
        float x = rand_float(min_x, max_x);
        std::vector<const level::platform_t*> under;
        for (auto& p : platforms) {
            auto pos = p.ph.get_position();
            if (x >= pos.x && x <= pos.x + p.ph.length())
                under.push_back(&p);
        }

        if (under.empty()) {
            /* x landed in a gap between platforms: clamp to the nearest one */
            auto* nearest  = &platforms.front();
            float best_dist = std::numeric_limits<float>::max();
            for (auto& p : platforms) {
                auto l = p.ph.get_position().x;
                auto r = l + p.ph.length();
                auto d = x < l ? l - x : x > r ? x - r : 0.f;
                if (d < best_dist) {
                    best_dist = d;
                    nearest   = &p;
                }
            }
            auto nl = nearest->ph.get_position().x;
            x       = rand_float(nl, nl + nearest->ph.length());
            under.push_back(nearest);
        }

        /* Random platform under x is the drop target */
        auto* target = under[rand_num<u32>(0, u32(under.size() - 1))];

        auto c = std::make_unique<crate>(sim, vec2f{x, top_y - crate_spawn_height}, target->ph.get_position().y);
        c->id(_next_crate_id++);
        crates.push_back(std::move(c));
        glog().info("crate spawned at ({}, {}), target platform y={}", x, top_y - crate_spawn_height, target->ph.get_position().y);
    }

    /* AI operators */

    void ai_operators_consume() {
        for (auto& [name, ai_op] : ai_operators) {
            auto& player = players.at(name);

            while (auto op = ai_op->consume_action()) {
                switch (*op) {
                case ai_action::jump: player->jump(); break;
                case ai_action::jump_down: player->jump_down(); break;
                case ai_action::move_left: player->move_left(); break;
                case ai_action::move_right: player->move_right(); break;
                case ai_action::stop: player->stop(); break;
                case ai_action::shot: player->shot(); break;
                case ai_action::relax: player->relax(); break;
                case ai_action::enable_long_shot: player->enable_long_shot(); break;
                case ai_action::disable_long_shot: player->disable_long_shot(); break;
                default: break;
                }
            }
        }

        ai_provide_player_level_sim_info();
    }

    void ai_provide_player_level_sim_info() {
        ai_mgr().provide_bullets(blt_mgr.bullets(), [](const bullet& bl) {
            return ai_bullet_t{
                bl.physic()->get_position(),
                bl.physic()->get_velocity(),
                bl.physic()->get_mass(),
                bl.group()
            };
        });

        /* Eliminated players are out of the AI's world */
        std::vector<std::pair<player_name_t, std::shared_ptr<player>>> active;
        for (auto& [name, pl] : players)
            if (!pl->eliminated())
                active.emplace_back(name, pl);

        ai_mgr().provide_players(active, [gy = sim.gravity().y](const std::shared_ptr<player>& pl) {
            auto& gun = pl->get_gun();
            auto* wpn = gun.get_weapon();
            auto  dir = vec2f(pl->get_on_left() ? -1.f : 1.f, 0.f);

            return ai_player_t{pl->get_position(),
                               pl->collision_box()->get_direction(),
                               pl->get_size(),
                               pl->get_velocity(),
                               pl->movement_acceleration(),
                               pl->barrel_pos(),
                               pl->name(),
                               pl->get_available_jumps(),
                               pl->get_x_accel(),
                               pl->get_x_slowdown(),
                               pl->get_jump_speed(),
                               pl->calc_max_jump_y_dist(gy),
                               pl->get_max_speed(),
                               wpn ? wpn->get_dispersion() : 0.01f,
                               pl->get_group(),
                               gun ? gun.get_bullet_vel() : 1500.f,
                               wpn ? wpn->hit_power() : 0.f,
                               wpn ? wpn->get_fire_rate() : 100.f,
                               wpn ? wpn->mag_size() : 1,
                               gun ? gun.ammo_elapsed() : 1,
                               pl->get_on_left(),
                               pl->collision_box()->is_lock_y(),
                               pl->long_shot_enabled(),
                               pl->is_walking(),
                               wpn ? wpn->long_shot_dir(dir) : dir};
        });

        ai_mgr().provide_physic_sim(sim.gravity(), sim.last_speed(), sim.last_rps(), gravity_for_bullets,
                                     sim.bullet_gravity().y);
    }

    void ai_provide_level_info() {
        if (!cur_level)
            return;

        /* TODO: move platforms into the ai_level_t */
        ai_mgr().provide_level({cur_level->level_size()});
        ai_mgr().provide_platforms(cur_level->get_platforms(), [](const level::platform_t& pl) {
            auto pos = pl.ph.get_position();
            auto len = pl.ph.length();
            return ai_platform_t{pos, vec2f(pos.x + len, pos.y)};
        });
    }

    physic_simulation  sim;
    bullet_mgr         blt_mgr;
    instant_kick_mgr   kick_mgr;
    adjustment_box_mgr adj_box_mgr;
    float              game_speed = 1.f;

    struct controll_player_t {
        std::shared_ptr<player_controller> controller;
        std::shared_ptr<player>            this_player;
    };

    using controlled_players_t = std::map<player_name_t, controll_player_t>;

    controlled_players_t                                  controlled_players;
    std::map<player_name_t, std::shared_ptr<player>>      players;
    std::map<std::string, std::shared_ptr<level>>         levels;
    std::map<u32, std::shared_ptr<player_controller>>     controllers;
    std::map<player_name_t, std::shared_ptr<ai_operator>> ai_operators;
    std::shared_ptr<level>                                cur_level;
    game_mode                                             cur_game_mode;
    std::unique_ptr<player_configurator_ui>               pconf_ui;
    std::optional<cfg_value_control>                      cfgval_ctrl;
    cfg_watcher                                           conf_watcher;
    dropped_gun_mgr                                       dropped_guns;

    std::vector<std::unique_ptr<crate>> crates;
    game_timer crate_timer;
    u32   _next_crate_id    = 1;
    u32   _next_bullet_id   = 0;

    vec2f cam_pos = {0.f, 0.f};

    bool on_game             = false;
    bool debug_physics       = false;
    /* true on the multiplayer client: crates are driven by the server state */
    bool crates_networked    = false;
    bool game_mode_networked = false; /* network client: the server sets the game mode */
    bool lobby_networked     = false; /* network client: the server drives the lobby */
    lobby_state lobby;
    /* References the global dfdh::bullet_gravity_enabled flag */
    bool& gravity_for_bullets;
    bool lua_cmd_enabled     = true;
    bool ai_profiler_enabled = false;

    /* Signals */
    signal<void(std::string)> sig_level_changed;
    signal<void()>            sig_shutdown;
    signal<void(std::string)> sig_execute_lua;
};

} // namespace dfdh
