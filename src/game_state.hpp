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

        /* Lag-compensated bullets hit the rewound players, before this tick's new shots */
        sim.add_tick_end_callback(
            "rewound_bullets", [this](const physic_simulation&, float) { rewound_bullets_tick_update(); }, -1);

        /* Players (weapons, shots, recoil, death) are updated on every simulation tick */
        sim.add_tick_end_callback("players", [this](const physic_simulation&, float) {
            if (on_game)
                players_tick_update();
        });

        sim.add_platform_callback("player", [this](physic_point* pnt) {
            for (auto& [_, p] : players)
                if (pnt == p->collision_box())
                    p->on_ground(pnt->prev_scalar_velocity());
        });
    }

    void player_conf_reload(const player_name_t& player_name = {}) {
        if (player_name.empty()) {
            for (auto& [name, p] : players) {
                p->set_from_config();
                glog().info("reload player config {}", name);
            }
        } else {
            auto found = players.find(player_name);
            if (found == players.end()) {
                glog().error("can't reload player conf: player {} not found", player_name);
                return;
            }

            found->second->set_from_config();
            glog().info("reload player config {}", player_name);
        }
    }

    player* player_create(const player_name_t& player_name) {
        if (players.contains(player_name)) {
            glog().error("player {} already exists", player_name);
            return nullptr;
        }

        auto player = player::create(player_name, sim, player::default_size());
        player->set_from_config();
        player->set_drop_gun_cb([this](const player::drop_gun_spec_t& spec) {
            return dropped_guns.spawn(spec.section, spec.position, spec.on_left,
                                      spec.long_shot, spec.group, spec.velocity,
                                      spec.angle_velocity);
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
                apply_cur_level_bullet_gravity();
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
        apply_cur_level_bullet_gravity();
        crates.clear();
        crate_timer.restart();
        sig_level_changed.emit_deferred(level_name);
    }

    /* Bullet gravity is defined by the current level: on if the level section
     * contains a bullet_gravity key, off otherwise */
    void apply_cur_level_bullet_gravity() {
        gravity_for_bullets = cur_level && cur_level->bullet_gravity().has_value();
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

    void player_on_dead(player* player) {
        player->increase_deaths();
        player_spawn(player);
    }

    void player_spawn(player* player = nullptr) {
        if (!cur_level) {
            glog().warn("Can't spawn players: level was not loaded");
            return;
        }

        if (player) {
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

    void ui_update(ui_ctx& ui) {
        if (pconf_ui)
            pconf_ui->update(ui);
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
        vec2f         velocity;
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

    /* Owner may be gone (nullptr): only its group is skipped then */
    std::optional<hitscan_hit_t> hitscan(const vec2f& a, const vec2f& b, float rewind_s, const player* owner, int group) {
        std::optional<hitscan_hit_t> res;
        float                        best_t = std::numeric_limits<float>::max();
        auto                         d      = b - a;

        for (auto& [_, pl] : players) {
            if (pl.get() == owner)
                continue;
            if (group != -1 && pl->get_group() != -1 && pl->get_group() == group)
                continue;

            auto bb = pl->bb();
            if (rewind_s > 0.f)
                if (auto past = pl->position_trace_lookup(sim.current_update_time(), rewind_s)) {
                    bb.left = past->x;
                    bb.top  = past->y - bb.height;
                }

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
            if (slab(a.x, d.x, bb.left, bb.left + bb.width) && slab(a.y, d.y, bb.top, bb.top + bb.height) &&
                t0 < best_t) {
                best_t = t0;
                res    = hitscan_hit_t{pl.get(), a + d * t0};
            }
        }
        return res;
    }

    /**
     * Spawn a bullet of the owner's weapon. A lag-compensated (network) owner
     * shot lag seconds ago on its screen: the bullet spawns forwarded by the
     * distance it would have flown since, and the skipped segment is covered
     * by a hit-scan against the players as the owner saw them (rewound).
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
            cur_frame_bullets.push_back(
                {owner.name(), 0, key, start, start, velocity, mass, owner.tracer_color(), gravity, true, blt.physic()});
            return;
        }

        auto id = ++_next_bullet_id;

        auto [pos, vel] = ballistic_forward(start, velocity, gravity, lag);

        if (lag > 0.f) {
            if (auto hit = hitscan(start, pos, rewind, owner)) {
                hit->target->on_bullet_hit(velocity * mass, hit->position, bullet_ref{id, {}});
                cur_frame_bullets.push_back({owner.name(), id, key, start, start, velocity, mass, owner.tracer_color(), gravity, false, {}});
                return;
            }
        }

        auto& blt = blt_mgr.shot(sim, pos, mass, vel, owner.tracer_color(), gravity, owner.get_group(),
                                 player::player_group_getter, player::owner_checker(&owner));
        blt.net_id(id);
        if (lag > 0.f)
            blt.lag_compensate(owner.name(), rewind);
        cur_frame_bullets.push_back({owner.name(), id, key, start, pos, vel, mass, owner.tracer_color(), gravity, false, {}});
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
                hit->target->on_bullet_hit(ph->impulse(), hit->position, bullet_ref::of(ph.get()));
                ph->delete_later();
            }
        }
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

        if (on_game) {
            if (cur_level)
                dropped_guns.set_level_size(cur_level->level_size());

            cur_frame_bullets.clear();
            sim.update(60, game_speed); /* players_tick_update() on every tick */

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
        for (auto& [_, pl] : players) {
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
        for (auto& [_, pl] : players)
            if (!pl->server_driven() && pl->collision_box()->get_position().y > 2100.f)
                player_on_dead(pl.get());
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

        /* Drop period is defined by the level; no period means crates never drop */
        if (auto period = cur_level->crate_drop_period()) {
            if (crate_timer.getElapsedTime().asSeconds() >= *period) {
                crates_spawn();
                crate_timer.restart();
            }
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
                    pl->setup_pistol(*wpn);
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

        ai_mgr().provide_players(players, [gy = sim.gravity().y](const std::shared_ptr<player>& pl) {
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
    std::unique_ptr<player_configurator_ui>               pconf_ui;
    std::optional<cfg_value_control>                      cfgval_ctrl;
    cfg_watcher                                           conf_watcher;
    dropped_gun_mgr                                       dropped_guns;

    std::vector<std::unique_ptr<crate>> crates;
    sf::Clock crate_timer;
    u32   _next_crate_id    = 1;
    u32   _next_bullet_id   = 0;

    vec2f cam_pos = {0.f, 0.f};

    bool on_game             = false;
    bool debug_physics       = false;
    /* true on the multiplayer client: crates are driven by the server state */
    bool crates_networked    = false;
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
