#pragma once

#include <deque>
#include <functional>

#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/Graphics/RenderTexture.hpp>
#include <SFML/Window/Event.hpp>
#include <SFML/Graphics/CircleShape.hpp>

#include "base/types.hpp"
#include "base/cfg.hpp"
#include "base/avg_counter.hpp"
#include "bullet.hpp"
#include "physic/physic_simulation.hpp"
#include "weapon.hpp"
#include "player_configurator.hpp"
#include "player_controller.hpp"
#include "net/net_protocol.hpp"

namespace dfdh {

class dropped_gun;

class player {
public:
    static vec2f default_size() {
        return {50.f, 90.f};
    }

    static vec2f sprite_size_adjust_factors() {
        return {1.25f, 1.08f};

    }

    static vec2f default_sprite_size() {
        auto adj = sprite_size_adjust_factors();
        auto sz = default_size();
        return {sz.x * adj.x, sz.y * adj.y};
    }

    static void default_hand_setup(sf::CircleShape& hand, const vec2f& size) {
        hand.setOutlineColor(sf::Color(0, 0, 0));
        hand.setOutlineThickness(size.x * 0.045f);
        hand.setRadius(size.x * 0.14f);
        hand.setOrigin(size.x * 0.14f, size.x * 0.14f);
    }

    static void default_leg_setup(sf::CircleShape& leg, const vec2f& size) {
        leg.setOutlineColor(sf::Color(0, 0, 0));
        leg.setOutlineThickness(size.x * 0.045f);
        leg.setRadius(size.x * 0.2f);
        leg.setOrigin(size.x * 0.2f, size.x * 0.35f);
    }

    static std::shared_ptr<player> create(const player_name_t& name, physic_simulation& sim, const vec2f& size) {
        return std::make_shared<player>(name, sim, size);
    }

    static std::shared_ptr<player> create(physic_simulation& sim, const vec2f& size) {
        return std::make_shared<player>(player_name_t{}, sim, size);
    }

    player(const player_name_t& name, physic_simulation& sim, const vec2f& size):
        _name(name), _size(size) {
        _collision_box = physic_group::create();
        _collision_box->append(physic_line::create({0.f, 0.f}, {0.f, -size.y}));
        _collision_box->append(physic_line::create({0.f, -size.y}, {size.x, 0.f}));
        _collision_box->append(physic_line::create({size.x, -size.y}, {0.f, size.y}));
        _collision_box->append(physic_line::create({size.x, 0.f}, {-size.x, 0.f}));

        _collision_box->enable_gravity();
        _collision_box->allow_platform(true);
        _collision_box->user_data(user_data_type::player);
        _collision_box->user_any(this);

        sim.add_primitive(_collision_box);

        default_hand_setup(_left_hand, _size);
        default_hand_setup(_right_hand, _size);
        default_leg_setup(_left_leg, size);
        default_leg_setup(_right_leg, size);

        jumps_left = max_jumps;
    }

    player(physic_simulation& sim, const vec2f& size): player({}, sim, size) {}

    ~player() {
        _collision_box->delete_later();
    }

    bool update_input(const player_controller& ks, sf::Event evt) {
        bool update_was = false;

        if (evt.type == sf::Event::KeyPressed) {
            auto c = evt.key.code;
            if (c == ks.up && !_jump_pressed) {
                jump();
                _jump_pressed = true;
                update_was = true;
            }
            else if (c == ks.down && !_jump_down_pressed) {
                jump_down();
                _jump_down_pressed = true;
                update_was = true;
            }
            else if (c == ks.left && is_float_zero(_cur_x_accel_l)) {
                _cur_x_accel_l = -_x_accel;
                _dir = dir_left;
                update_was = true;
            }
            else if (c == ks.right && is_float_zero(_cur_x_accel_r)) {
                _cur_x_accel_r = _x_accel;
                _dir = dir_right;
                update_was = true;
            }
            else if (c == ks.shot) {
                update_was = shot();
            }
            else if (c == ks.drop) {
                update_was = drop_gun() != nullptr;
            }
            else if (c == ks.adjust_up) {
                _long_shot_enabled = true;
            }
        }
        else if (evt.type == sf::Event::KeyReleased) {
            auto c = evt.key.code;
            if (c == ks.left && !is_float_zero(_cur_x_accel_l)) {
                _cur_x_accel_l = 0.f;
                update_was = true;
            }
            else if (c == ks.right && !is_float_zero(_cur_x_accel_r)) {
                _cur_x_accel_r = 0.f;
                update_was = true;
            }
            else if (c == ks.shot) {
                update_was = relax();
            }
            else if (c == ks.up && _jump_pressed) {
                _jump_pressed = false;
                update_was = true;
            }
            else if (c == ks.down && _jump_down_pressed) {
                _jump_down_pressed = false;
                update_was = true;
            }
            else if (c == ks.adjust_up) {
                _long_shot_enabled = false;
            }
        }

        if (update_was)
            ++_evt_counter;

        return update_was;
    }

    /**
     * Apply the movement part of a per-tick input state: held movement and
     * long shot, jump edges. Free of side effects outside of the player's
     * physic state, so it is safe to re-apply while re-simulating
     */
    void apply_movement_input(const net::player_input_state& in) {
        if (in.left && !in.right)
            move_left();
        else if (in.right && !in.left)
            move_right();
        else
            stop();

        if (in.jump)
            jump();
        if (in.jump_down)
            jump_down();
        enable_long_shot(in.long_shot);
    }

    /**
     * Apply a per-tick input state (network driven): movement, the trigger
     * as a held state and the drop edge
     */
    void apply_input(const net::player_input_state& in) {
        apply_movement_input(in);

        if (in.shot)
            shot();
        else
            relax();
        if (in.drop)
            drop_gun();

        increment_evt_counter();
    }

    /* A hit predicted on the client, waiting for the server's confirmation */
    struct predicted_hit_t {
        bullet_ref ref;
        vec2f      impulse;
        bool       grounded; // the player stood on a platform when hit
    };

    /**
     * The part of the player state that the physics tick evolves; a
     * snapshot of it is enough to restore and re-simulate the player
     */
    struct physic_state {
        vec2f pos;
        vec2f vel;
        bool  lock_y        = false;
        u32   jumps_left    = 0;
        float accel_left_f  = 1.f;
        float accel_right_f = 1.f;
    };

    [[nodiscard]]
    physic_state get_physic_state() const {
        return {get_position(), get_velocity(), _collision_box->is_lock_y(), jumps_left, _accel_left_f, _accel_right_f};
    }

    void restore_physic_state(const physic_state& st) {
        position(st.pos);
        velocity(st.vel);
        if (st.lock_y)
            _collision_box->lock_y();
        else
            _collision_box->unlock_y();
        jumps_left     = st.jumps_left;
        _accel_left_f  = st.accel_left_f;
        _accel_right_f = st.accel_right_f;
    }

    /**
     * Replay mode (re-simulation of already simulated ticks): no position
     * trace samples, no landing sounds
     */
    void replaying(bool value) {
        _replaying = value;
    }

    /**
     * Impulse applied outside of the physics tick (weapon recoil), accumulated
     * so the client-side prediction can re-apply it while re-simulating
     */
    void apply_logged_impulse(const vec2f& impulse) {
        _collision_box->apply_impulse(impulse);
        _pending_impulse += impulse;
    }

    void apply_impulse(const vec2f& impulse) {
        _collision_box->apply_impulse(impulse);
    }

    vec2f take_pending_impulse() {
        return std::exchange(_pending_impulse, vec2f{0.f, 0.f});
    }

    /**
     * Visual-only displacement after a prediction correction: the drawn
     * player moves smoothly to the corrected position. Large corrections
     * (e.g. respawn) snap
     */
    void add_render_offset(const vec2f& offset) {
        _render_offset = magnitude(_render_offset + offset) > 150.f ? vec2f{0.f, 0.f} : _render_offset + offset;
    }

    [[nodiscard]]
    const vec2f& render_offset() const {
        return _render_offset;
    }

    /* Put the legs under the body at its current position (after moving it outside of the physics) */
    void place_legs() {
        leg_animation_update(0.f, get_position(), get_velocity());
    }

    /* Leg positions (absolute, set by the physics update) */
    [[nodiscard]]
    std::pair<vec2f, vec2f> leg_positions() const {
        return {_left_leg.getPosition(), _right_leg.getPosition()};
    }

    void reset_render_offset() {
        _render_offset = {0.f, 0.f};
    }

    /**
     * Instantly move the player (spawn, respawn): stops it and counts the
     * teleport, so network clients jump to the new position instead of
     * interpolating or smoothing across the map. The position trace (lag
     * compensation history) restarts: there is no path between the positions
     */
    void teleport(const vec2f& pos) {
        position(pos);
        velocity({0.f, 0.f});
        _position_trace.clear();
        _render_offset = {0.f, 0.f};
        ++_teleports;
        place_legs();
    }

    /** Number of teleports so far (wraps around) */
    [[nodiscard]]
    u16 teleports() const {
        return _teleports;
    }

    /**
     * Lag compensation of a remote (network) player's shots, seconds:
     * lag    - time between the shot on the client and its processing on the server;
     *          bullets are spawned forwarded by vel * lag
     * rewind - how far in the past the client saw the other players at the shot
     */
    struct lag_compensation_t {
        float lag    = 0.f;
        float rewind = 0.f;
    };

    void lag_compensation(const lag_compensation_t& value) {
        _lag_comp = value;
    }

    [[nodiscard]]
    const lag_compensation_t& lag_compensation() const {
        return _lag_comp;
    }

    void stop() {
        _cur_x_accel_l = 0.f;
        _cur_x_accel_r = 0.f;
    }

    void move_left() {
        _cur_x_accel_r = 0.f;
        _cur_x_accel_l = -_x_accel;
        _dir = dir_left;
    }

    void move_right() {
        _cur_x_accel_l = 0.f;
        _cur_x_accel_r = _x_accel;
        _dir = dir_right;
    }

    bool shot() {
        if (!_pistol)
            return false;

        auto pulled = _pistol.pull_trigger(_tracer_color);
        /* Network-driven: deterministic dispersion keyed by the input tick of the pull */
        if (pulled && _last_input_seq != 0)
            _pistol.seed_shots(_last_input_seq);
        return pulled;
    }

    /**
     * Description of a gun to be dropped: it spawns at the same position,
     * orientation and mirroring as the gun drawn in the player's hands.
     * velocity is the starting x velocity ("drop_velocity" mirrored when the
     * player faces left), angle_velocity rotates the sprite only
     * ("drop_angle_velocity", also mirrored when the player faces left)
     */
    struct drop_gun_spec_t {
        std::string section;
        vec2f       position;
        bool        on_left;
        bool        long_shot;
        int         group;
        vec2f       velocity;
        float       angle_velocity;
    };

    using drop_gun_fn_t = std::function<dropped_gun*(const drop_gun_spec_t&)>;

    void set_drop_gun_cb(drop_gun_fn_t callback) {
        _drop_gun_cb = std::move(callback);
    }

    /**
     * Drop the currently held gun: a dropped gun spawns at the arm position
     * and the gun disappears from the player's hands. Returns the spawned
     * dropped gun, or nullptr if the player has no gun or no callback set
     */
    dropped_gun* drop_gun() {
        if (!_pistol || !_drop_gun_cb)
            return nullptr;

        /*
         * Server-authoritative player (client side): don't spawn the gun
         * locally. The drop request goes to the server; the dropped gun
         * arrives with the state broadcast, and the pistol section sync
         * removes the gun from the hands
         */
        if (server_driven())
            return nullptr;

        auto  pos = arm_position(get_position());
        auto* wpn = _pistol.get_weapon();
        auto lf   = _on_left ? -1.f : 1.f;

        auto drop_dir       = normalize(vec2f{lf, -0.5f});
        auto start_velocity = get_velocity() + wpn->drop_velocity() * drop_dir;

        auto* gun = _drop_gun_cb(drop_gun_spec_t{.section        = _pistol_section,
                                                 .position       = pos,
                                                 .on_left        = _on_left,
                                                 .long_shot      = _long_shot_enabled,
                                                 .group          = _group,
                                                 .velocity       = start_velocity,
                                                 /* angle_velocity is passed unmirrored: weapon::draw inverts the
                                                  * angle (and flips the x scale) when on_left, which is exactly
                                                  * the mirror of the right-facing rotation */
                                                 .angle_velocity = wpn->drop_angle_velocity()});

        if (gun)
            clear_pistol();

        return gun;
    }

    bool relax() {
        if (_pistol)
            return _pistol.relax_trigger();
        return false;
    }

    void enable_long_shot(bool value = true) {
        _long_shot_enabled = value;
    }

    void disable_long_shot() {
        _long_shot_enabled = false;
    }

    static int player_group_getter(const physic_point* p) {
        return std::any_cast<player*>(p->get_user_any())->get_group();
    }

    template <typename F = void (*)(const vec2f&, const vec2f&, float)>
    void game_update(physic_simulation& sim,
                     bullet_mgr&        bm,
                     const vec2f&       cam_position,
                     bool               gravity_for_bullets   = false,
                     bool               spawn_bullet          = true,
                     F&&                bullet_spawn_callback = nullptr) {
        /* Hit player sounds */
        if (_hit_sound_info) {
            sound_mgr().play(_hit_sound_info->pwned ? "player/headshot0.wav"s
                                                    : ("player/bullethit"s + rand_num('0', '1') + ".wav"),
                             _group,
                             (_hit_sound_info->position - cam_position),
                             sim.last_speed());
            _hit_sound_info.reset();
        }

        /* Steps and fall sounds */
        if (_collision_box->is_lock_y() && std::abs(_collision_box->get_velocity().x) > 20.f) {
            if (!_step_sound_active) {
                _step_sound_timer.restart();
                _step_sound_active = true;
            }
        }
        else if (_step_sound_active) {
            _step_sound_active = false;
        }
        if (_step_sound_active) {
            float period = 65.f / (std::abs(_collision_box->get_velocity().x) * sim.last_speed());
            if (_step_sound_timer.getElapsedTime().asSeconds() > period) {
                sound_mgr().play("player/step"s + rand_num('0', '2') + ".wav",
                                 _group,
                                 get_position() - cam_position,
                                 sim.last_speed());
                _step_sound_timer.restart();
            }
        }
        if (_fall_velocity) {
            auto volume = inverse_lerp(0.f, 1800.f, std::clamp(*_fall_velocity, 0.f, 1800.f));
            volume *= volume * 100.f;
            sound_mgr().play("player/fall"s + rand_num('0', '1') + ".wav",
                             _group,
                             get_position() - cam_position,
                             sim.last_speed(),
                             volume);
            _fall_velocity.reset();
        }

        /* Update weapon */
        if (_pistol) {
            if (_last_input_seq != 0)
                _pistol.input_tick(_last_input_seq);
            auto dir = facing_dir();
            if (auto recoil = _pistol.update(arm_position(get_position()),
                                             cam_position,
                                             dir,
                                             gravity_for_bullets && _long_shot_enabled,
                                             bm,
                                             sim,
                                             gravity_for_bullets,
                                             spawn_bullet,
                                             std::forward<F>(bullet_spawn_callback),
                                             _group,
                                             player_group_getter))
                apply_logged_impulse(-dir * *recoil);
        }
    }

    void physic_update(const physic_simulation& sim, float timestep) {
        _accel_left_f  = std::min(_accel_left_f + timestep * 2.f, 1.f);
        _accel_right_f = std::min(_accel_right_f + timestep * 2.f, 1.f);

        auto  vel          = _collision_box->get_velocity();
        auto  moving_left  = !is_float_zero(_cur_x_accel_l);
        auto  moving_right = !is_float_zero(_cur_x_accel_r);
        bool  moving       = moving_left || moving_right;
        float move_accel_l = _cur_x_accel_l;
        float move_accel_r = _cur_x_accel_r;

        if (moving_left && moving_right) {
            moving_left  = false;
            moving_right = false;
            move_accel_l = 0.f;
            move_accel_r = 0.f;
        }

        float accel = 0.f;
        if (_collision_box->is_lock_y() && !moving) {
            if (vel.x > _max_speed || (vel.x > 0.f && !moving_right))
                accel = -_x_slowdown;
            else if (vel.x < -_max_speed || (vel.x < 0.f && !moving_left))
                accel = _x_slowdown;
        }

        if (vel.x < _max_speed) {
            auto mov_accel = move_accel_r * _accel_left_f;
            auto new_vel   = vel.x + mov_accel * timestep;
            if (new_vel < _max_speed)
                accel += mov_accel;
            else
                vel.x = _max_speed;
        }
        if (vel.x > -_max_speed) {
            auto mov_accel = move_accel_l * _accel_right_f;
            auto new_vel   = vel.x + mov_accel * timestep;
            if (new_vel > -_max_speed)
                accel += mov_accel;
            else
                vel.x = -_max_speed;
        }

        float prev_v = vel.x;
        vel.x += accel * timestep;
        if (!moving && ((prev_v < 0.f && vel.x > 0.f) || (prev_v > 0.f && vel.x < 0.f))) {
            vel.x                 = 0.f;
            _movement_acceleration = {0.f, 0.f};
        }
        else {
            _movement_acceleration = {accel, 0.f};
        }

        _collision_box->velocity(vel);

        physic_jump_update();
        physic_weapon_bobbing_update(sim);
        leg_animation_update(timestep, _collision_box->get_position(), vel);

        if (!_replaying) {
            if (_position_trace.size() > 300)
                _position_trace.pop_front();
            _position_trace.emplace_back(sim.current_update_time(), get_position());
        }
    }

    void jump() {
        _jump_scheduled = true;
    }

    void jump_down() {
        if (_collision_box->is_lock_y()) {
            _jump_down_scheduled = true;
        }
    }

    static void draw_to_texture_target(sf::RenderTexture&  target,
                                       const vec2f& size,
                                       float               scale,
                                       sf::Sprite&         body,
                                       sf::Sprite&         face,
                                       sf::CircleShape&    hand_or_leg,
                                       weapon*             wpn) {
        float icon_x_size =
            wpn ? size.x * wpn->arm_position_factors(false).x + wpn->barrel_pos().x * scale
                : size.y;

        target.create(u32(icon_x_size), u32(size.y));
        /* New FBO texture contents are undefined until cleared (driver may keep stale pixels) */
        target.clear({0, 0, 0, 0});

        auto body_txtr_size = body.getTexture()->getSize();
        auto face_txtr_size = face.getTexture()->getSize();
        auto body_f_x       = size.x / float(body_txtr_size.x);
        auto body_f_y       = size.y / float(body_txtr_size.y);
        auto face_f_x       = size.x / float(face_txtr_size.x);
        auto face_f_y       = size.y / float(face_txtr_size.y);

        body.setScale(body_f_x, body_f_y);
        face.setScale(face_f_x, face_f_y);
        body.setPosition(0.f, 0.f);
        face.setPosition(0.f, 0.f);

        target.setView(sf::View(sf::FloatRect{0.f, size.y, icon_x_size, -size.y}));
        target.draw(body);
        target.draw(face);

        auto sz = size;
        sz.x /= sprite_size_adjust_factors().x;
        sz.y /= sprite_size_adjust_factors().y;

        if (wpn) {
            auto pos = size;
            auto f = wpn->arm_position_factors(false);
            pos.x *= f.x;
            pos.y *= f.y;
            pos.y += size.y;
            wpn->draw(pos, false, 0.f, target, scale);

            default_hand_setup(hand_or_leg, sz);
            hand_or_leg.setPosition(pos + wpn->arm_idle_pos() * scale);
            target.draw(hand_or_leg);

            hand_or_leg.move(wpn->arm2_idle_pos() * scale);
            target.draw(hand_or_leg);
        }


        default_leg_setup(hand_or_leg, sz);
        hand_or_leg.setPosition(size.x * 0.3f, size.y);
        target.draw(hand_or_leg);

        hand_or_leg.setPosition(size.x * 0.7f, size.y);
        target.draw(hand_or_leg);

        target.display();
    }

    void draw(sf::RenderWindow& wnd, float interpolation_factor, float timestep, bool gravity_for_bullets = false) {
        auto pos      = _collision_box->get_position();
        auto next_pos = pos + _collision_box->get_velocity() * timestep;

        pos = lerp(pos, next_pos, interpolation_factor);

        /* Decay the correction offset (half-life ~50ms) */
        _render_offset *= std::exp2(-_render_offset_clock.restart().asSeconds() / 0.05f);
        if (magnitude(_render_offset) < 0.01f)
            _render_offset = {0.f, 0.f};
        pos += _render_offset;

        auto adj = sprite_size_adjust_factors();
        auto dif = vec2f((_size.x * adj.x - _size.x) * 0.5f, _size.y * adj.y);

        if (_dir == dir_left && !_on_left)
            on_left(true);
        if (_dir == dir_right)
            on_left(false, true);

        if (_on_left)
            dif.x = -dif.x - _size.x;

        for (auto* sprite : {&_body, &_face, &_hat}) {
            sprite->setPosition(pos - dif);
            wnd.draw(*sprite);
        }

        /* Legs are positioned in absolute coordinates by the physics: shift them with the body */
        static constexpr auto draw_leg_lerp =
            [](auto&& wnd, float timestep, float factor, auto&& leg, const vec2f& vel, const vec2f& offset) {
                vec2f pos      = leg.getPosition();
                auto  next_pos = pos + vel * timestep;
                leg.setPosition(lerp(pos, next_pos, factor) + offset);
                wnd.draw(leg);
                leg.setPosition(pos);
            };
        draw_leg_lerp(wnd, timestep, interpolation_factor, _left_leg, _collision_box->get_velocity(), _render_offset);
        draw_leg_lerp(wnd, timestep, interpolation_factor, _right_leg, _collision_box->get_velocity(), _render_offset);

        if (_pistol) {
            auto [lh, rh] = _pistol.draw(arm_position(pos),
                                        _on_left,
                                        gravity_for_bullets && _long_shot_enabled,
                                        wnd,
                                        _collision_box->get_velocity());
            _left_hand.setPosition(lh);
            _right_hand.setPosition(rh);
        }
        else {
            _left_hand.setPosition(pos.x, pos.y - _size.y / 2.5f);
            _right_hand.setPosition(pos.x + _size.x, pos.y - _size.y / 2.5f);
        }
        wnd.draw(_left_hand);
        wnd.draw(_right_hand);
    }

    void set_body(const std::string& path, const sf::Color& color = {255, 255, 255}) {
        _body_txtr_path = path;
        _body_color = color;

        set_some(path, _body_txtr, _body, _size, color);
        _left_hand.setFillColor(color);
        _right_hand.setFillColor(color);
        _left_leg.setFillColor(color);
        _right_leg.setFillColor(color);
    }

    void set_face(const std::string& path, const sf::Color& color = {255, 255, 255}) {
        _face_txtr_path = path;
        set_some(path, _face_txtr, _face, _size, color);
    }

    void set_hat(const std::string& path, const sf::Color& color = {255, 255, 255}) {
        set_some(path, _hat_txtr, _hat, _size, color);
    }

    void setup_pistol(const std::string& section) {
        _pistol = weapon_instance(section);
        _pistol_section = section;
    }

    [[nodiscard]]
    const std::string& pistol_section() const {
        return _pistol_section;
    }

    /** Remove the pistol from the hands (e.g. synced pistol loss) */
    void clear_pistol() {
        _pistol = {};
        _pistol_section.clear();
    }

    [[nodiscard]]
    const std::string& face_texture_path() const {
        return _face_txtr_path;
    }

    [[nodiscard]]
    const std::string& body_texture_path() const {
        return _body_txtr_path;
    }

    [[nodiscard]]
    const sf::Color& body_color() const {
        return _body_color;
    }

    [[nodiscard]]
    const weapon_instance& get_gun() const {
        return _pistol;
    }

    [[nodiscard]]
    weapon_instance& get_gun() {
        return _pistol;
    }

    [[nodiscard]]
    bool shotting() const {
        return _pistol && _pistol.on_shot();
    }

    /**
     * Client-side mirror of a server player: the server is authoritative, so
     * the weapon spawns no bullets locally (they arrive from the server),
     * bullet hits produce no impulse and there is no local death or gun drop
     */
    void set_server_driven(bool value) {
        _server_driven = value;
    }

    [[nodiscard]]
    bool server_driven() const {
        return _server_driven;
    }

    /**
     * Client-predicted server-driven player (the client's local player): its
     * own bullets are spawned locally right away and matched to the server's
     * authoritative ones later, bullet hits push it right away (the
     * prediction reconciles them with the server)
     */
    void set_predicted(bool value) {
        _predicted = value;
    }

    [[nodiscard]]
    bool predicted() const {
        return _predicted;
    }

    /**
     * Seq of the network input tick being applied to this player (set before
     * the input is applied); trigger pulls are keyed by it, so predicted and
     * authoritative bullets can be matched
     */
    void last_input_seq(u32 value) {
        _last_input_seq = value;
    }

    [[nodiscard]]
    u32 last_input_seq() const {
        return _last_input_seq;
    }

    /**
     * Marked for players driven by the network server (no local controller needed
     * so the host keyboard does not drive them); they get full simulation,
     * including local bullet spawning
     */
    void set_net_controlled(bool value) {
        _net_controlled = value;
    }

    [[nodiscard]]
    bool net_controlled() const {
        return _net_controlled;
    }

    [[nodiscard]]
    sf::Color tracer_color() const {
        return _tracer_color;
    }

    void tracer_color(sf::Color value) {
        _tracer_color = value;
    }

    [[nodiscard]]
    const vec2f& movement_acceleration() const {
        return _movement_acceleration;
    }

    [[nodiscard]]
    bool is_walking() const {
        return !is_float_zero(_cur_x_accel_l) ^ !is_float_zero(_cur_x_accel_r);
    }


    [[nodiscard]]
    std::optional<vec2f> position_trace_lookup(std::chrono::steady_clock::time_point now, float latency_offset) const {
        auto time_dur = [](auto now, auto last) {
            return std::chrono::duration_cast<std::chrono::duration<float>>(now - last).count();
        };

        auto pos = _position_trace.begin();
        for (; pos != _position_trace.end(); ++pos)
            if (time_dur(now, pos->first) < latency_offset)
                break;

        if (pos == _position_trace.end())
            return {};

        /* Interpolate between the last sample older than now - latency_offset and the first newer one */
        if (pos != _position_trace.begin()) {
            auto before     = pos - 1;
            auto age_before = time_dur(now, before->first);
            auto age        = time_dur(now, pos->first);
            auto f          = (age_before - latency_offset) / (age_before - age);

            return lerp(before->second, pos->second, f);
        }

        return pos->second;
    }

    void set_from_config() {
        player_configurator pc{_name};
        set_body(pc.body_texture_path(), pc.body_color);
        set_face(pc.face_texture_path());
        setup_pistol(pc.pistol);
        tracer_color(pc.tracer_color);
    }

    struct hit_sound_info {
        vec2f position;
        bool  pwned;
    };
    void play_hit_sound(const vec2f& hit_position, bool pwned) {
        _hit_sound_info.emplace(hit_sound_info{hit_position, pwned});
    }

private:
    static constexpr float bobbing_coef = 0.1f;
    static constexpr float bobbing_minmax = 0.2f;

    void physic_weapon_bobbing_update(const physic_simulation& sim) {
        auto gforce = _collision_box->g_force(sim.prev_timestep());
        if (_collision_box->is_lock_y())
            gforce += sim.gravity();

        auto  c1       = std::pow(float(sim.last_rps()) / sim.last_speed(), 1.5f) * 0.010758287f;
        auto  c2       = c1 / (c1 + 1.f);
        auto  scalar_g = magnitude(sim.gravity());
        float minmax   = bobbing_minmax * scalar_g / bobbing_coef;

        _bobbing_accel = clamp(_bobbing_accel * c2 + gforce / c1, vec2f::filled(-minmax), vec2f::filled(minmax));

        bool bobbing_x_on = std::abs(_bobbing_accel.x) > 0.5f;
        bool bobbing_y_on = std::abs(_bobbing_accel.y) > 0.5f;

        auto bobbing_f = (1.f / magnitude(sim.gravity())) * -bobbing_coef;
        _bobbing.x = bobbing_x_on ? _bobbing_accel.x * bobbing_f : 0.f;
        _bobbing.y = bobbing_y_on ? _bobbing_accel.y * bobbing_f : 0.f;
    }

    void physic_jump_update() {
        if (_jump_scheduled) {
            if (_collision_box->is_lock_y() || jumps_left) {
                auto vel = _collision_box->get_velocity();
                vel.y    = -_jump_speed;
                _collision_box->velocity(vel);

                if (!_collision_box->is_lock_y())
                    --jumps_left;
                else
                    _collision_box->unlock_y();
            }
            _jump_scheduled = false;
        }

        if (_jump_down_scheduled) {
            if (_collision_box->is_lock_y()) {
                _collision_box->unlock_y();
                auto vel = _collision_box->get_velocity();
                vel.y += 100.f;
                _collision_box->velocity(vel);
            }
            _jump_down_scheduled = false;
        }
    }

    void leg_animation_update(float timestep, const vec2f& pos, const vec2f& vel) {
        auto vx = vel.x;
        float jump_f = 0.2f;
        if (!_collision_box->is_lock_y()) {
            jump_f = std::clamp(jump_f + -vel.y * 0.001f, 0.f, 1.f);
            vx = 0.f;
            if (_leg_timer < 0.5f) {
                _leg_timer -= timestep * 2.f;
                if (_leg_timer < 0.f)
                    _leg_timer = 0.f;
            } else {
                _leg_timer += timestep * 2.f;
                if (_leg_timer > 0.9999f)
                    _leg_timer = 0.9999f;
            }
        }

        auto pos1 = pos;
        auto pos2 = pos;
        pos1.x += _size.x * 0.25f;
        pos2.x += _size.x * 0.75f;

        _leg_timer = std::fmod(_leg_timer + (std::fabs(vx) * timestep) / (_size.x * 2.f), 1.f);
        auto p1 = lerp(pos1.x, pos2.x, _leg_timer);
        auto p2 = lerp(pos1.x, pos2.x, 1.f - _leg_timer);

        pos1.x = p1;
        pos2.x = p2;

        auto jump_y = _size.x * 0.4f * (jump_f - 0.2f);
        pos1.y += jump_y;
        pos2.y += jump_y;

        _left_leg.setPosition(pos1);
        _right_leg.setPosition(pos2);
    }

    void set_some(const std::string&  path,
                  sf::Texture&        txtr,
                  sf::Sprite&         sprite,
                  const vec2f& size,
                  const sf::Color&    color) {
        txtr = texture_mgr().load(path);
        txtr.setSmooth(true);
        sprite.setTexture(txtr);
        sprite.setColor(color);

        auto sz = txtr.getSize();
        auto adj = sprite_size_adjust_factors();
        float xf = (size.x * adj.x) / float(sz.x);
        float yf = (size.y * adj.y) / float(sz.y);
        sprite.setScale(_on_left ? -xf : xf, yf);
    }

private:
    player_name_t _name;
    std::string   _pistol_section;
    std::string   _body_txtr_path;
    std::string   _face_txtr_path;
    sf::Color     _body_color;

    std::shared_ptr<physic_group> _collision_box;
    sf::Sprite  _body;
    sf::Sprite  _face;
    sf::Sprite  _hat;
    sf::Texture _body_txtr;
    sf::Texture _face_txtr;
    sf::Texture _hat_txtr;
    sf::CircleShape _left_hand;
    sf::CircleShape _right_hand;
    sf::CircleShape _left_leg;
    sf::CircleShape _right_leg;

    std::deque<std::pair<std::chrono::steady_clock::time_point, vec2f>> _position_trace;

    sf::Color _tracer_color = {255, 255, 0};

    float _leg_timer = 0.f;

    vec2f _size;

    float _accel_left_f     = 1.f;
    float _accel_right_f    = 1.f;
    float _max_speed        = 280.f;
    float _x_accel          = 1550.f; // 2250.f;
    float _x_slowdown       = 700.f;
    float _jump_speed       = 620.f;

    float _cur_x_accel_l = 0.f;
    float _cur_x_accel_r = 0.f;

    vec2f _movement_acceleration = {0.f, 0.f};
    vec2f _bobbing_accel         = {0.f, 0.f};
    vec2f _bobbing               = {0.f, 0.f};

    u32 jumps_left = 1;
    u32 max_jumps = 1;

    u32 _deaths = 0;
    u64 _evt_counter = 0;

    weapon_instance _pistol;

    enum player_dir_t { dir_none = 0, dir_left, dir_right } _dir;
    bool _on_left            = false;
    bool _server_driven      = false;
    bool _predicted          = false;
    u32  _last_input_seq     = 0;
    bool _replaying          = false;
    bool _net_controlled     = false;
    bool _jump_pressed       = false;
    bool _jump_down_pressed  = false;
    bool _long_shot_enabled  = false;

    bool _jump_scheduled      = false;
    bool _jump_down_scheduled = false;

    int _group = 0;

    bool _on_hit_event = false;
    std::optional<hit_sound_info> _hit_sound_info;

    bool                 _step_sound_active = false;
    std::optional<float> _fall_velocity;
    sf::Clock _step_sound_timer;
    sf::Clock _render_offset_clock;

    drop_gun_fn_t _drop_gun_cb;

    vec2f              _pending_impulse = {0.f, 0.f};
    u16                _teleports       = 0;

    std::vector<predicted_hit_t>                                  _predicted_hits;
    std::deque<std::pair<u32, std::chrono::steady_clock::time_point>> _recent_hits;
    vec2f              _render_offset   = {0.f, 0.f};
    lag_compensation_t _lag_comp;

public:
    u64 last_state_packet_id = 0;

    [[nodiscard]]
    const player_name_t& name() const {
        return _name;
    }

    [[nodiscard]]
    vec2f barrel_pos() const {
        if (_pistol)
            return arm_position(get_position()) + _pistol.shot_displacement(facing_dir());
        else
            return _collision_box->get_position();
    }

    void increase_deaths() {
        ++_deaths;
    }

    [[nodiscard]]
    u32 get_deaths() const {
        return _deaths;
    }

    void group(int value) {
        _group = value;
    }

    [[nodiscard]]
    int get_group() const {
        return _group;
    }

    void on_left(bool value, bool force = false) {
        if (value == _on_left && !force)
            return;

        _on_left = value;
        _dir     = dir_none;

        /* Flip the body sprites to match the new facing direction */
        for (auto* sprite : {&_body, &_face, &_hat}) {
            auto scale_x = std::fabs(sprite->getScale().x);
            sprite->setScale(value ? -scale_x : scale_x, sprite->getScale().y);
        }
    }

    [[nodiscard]]
    bool get_on_left() const {
        return _on_left;
    }

    void reset_accel_f(bool left_only) {
        (left_only ? _accel_left_f : _accel_right_f) = 0.f;
    }

    void smooth_velocity_set(const vec2f& value, float smooth_factor = 0.25f) {
        auto vel = get_velocity();
        velocity(vel + (value - vel) * smooth_factor);
    }

    void smooth_position_set(const vec2f& value, float smooth_factor = 0.25f) {
        auto pos = get_position();
        position(pos + (value - pos) * smooth_factor);
    }

    void position(const vec2f& value) {
        _collision_box->position(value);
    }

    [[nodiscard]]
    const vec2f& get_position() const {
        return _collision_box->get_position();
    }

    void velocity(const vec2f& value) {
        _collision_box->velocity(value);
    }

    [[nodiscard]]
    vec2f get_velocity() const {
        return _collision_box->get_velocity();
    }

    [[nodiscard]]
    const vec2f& get_size() const {
        return _size;
    }

    /** Player AABB (bottom-left origin, SFML y-down) */
    [[nodiscard]]
    sf::FloatRect bb() const {
        auto& pos = get_position();
        return {pos.x, pos.y - _size.y, _size.x, _size.y};
    }

    [[nodiscard]]
    vec2f facing_dir() const {
        return {_on_left ? -1.f : 1.f, 0.f};
    }

    /** Weapon arm joint for the player located at pos */
    [[nodiscard]]
    vec2f arm_position(const vec2f& pos) const {
        auto f = _pistol.arm_position_factors(_on_left, _bobbing);
        return pos + vec2f(_size.x * f.x, _size.y * f.y);
    }

    [[nodiscard]]
    float get_max_speed() const {
        return _max_speed;
    }

    void max_speed(float value) {
        _max_speed = value;
    }

    [[nodiscard]]
    float calc_max_jump_y_dist(float gravity_y) const {
        auto t = _jump_speed / gravity_y;
        return (_jump_speed * t - gravity_y * t * t * 0.5f) * float(get_available_jumps());
    }

    [[nodiscard]]
    float get_jump_speed() const {
        return _jump_speed;
    }

    void jump_speed(float value) {
        _jump_speed = value;
    }

    [[nodiscard]]
    float get_x_accel() const {
        return _x_accel;
    }

    void x_accel(float value) {
        _x_accel = value;
    }

    [[nodiscard]]
    float get_x_slowdown() const {
        return _x_slowdown;
    }

    void x_slowdown(float value) {
        _x_slowdown = value;
    }

    [[nodiscard]]
    const physic_group* collision_box() const {
        return _collision_box.get();
    }

    [[nodiscard]]
    physic_group* collision_box() {
        return _collision_box.get();
    }

    void enable_double_jump() {
        jumps_left = max_jumps;
    }

    void on_ground(float fall_velocity = 0.f) {
        enable_double_jump();
        if (!_replaying)
            _fall_velocity = fall_velocity;
    }

    void mass(float value) {
        _collision_box->mass(value);
    }

    [[nodiscard]]
    u32 get_available_jumps() const {
        return _collision_box->is_lock_y() ? jumps_left + 1 : jumps_left;
    }

    [[nodiscard]]
    bool long_shot_enabled() const {
        return _long_shot_enabled;
    }

    void increment_evt_counter() {
        ++_evt_counter;
    }

    void evt_counter(u64 value) {
        _evt_counter = value;
    }

    [[nodiscard]]
    u64 evt_counter() const {
        return _evt_counter;
    }

    /* Knockback of a bullet hit: lifts the player off the ground if hit from below hard enough */
    void apply_hit_physics(const vec2f& impulse) {
        if (_collision_box->is_lock_y() && impulse.y < -180.f)
            _collision_box->unlock_y();
        _collision_box->apply_impulse(impulse);
        reset_accel_f(impulse.x < 0.f);
    }

    /**
     * Bullet (or hit-scan) hit: knockback with the bullet impulse, plus hit
     * feedback.
     * - authoritative player: knockback, the bullet id is remembered for a
     *   while (reported to the clients, see recent_hit_ids)
     * - predicted local player (client): knockback right away, queued for
     *   the prediction history
     * - remote player mirror (client): queued only, the client shows it as a
     *   visual knockback until the server state has it
     */
    void on_bullet_hit(const vec2f& impulse, const vec2f& hit_position, const bullet_ref& ref = {}) {
        if (!_server_driven) {
            apply_hit_physics(impulse);
            if (ref.id != 0)
                _recent_hits.push_back({ref.id, std::chrono::steady_clock::now()});
        }
        else {
            if (_predicted)
                apply_hit_physics(impulse);
            _predicted_hits.push_back({ref, impulse, _collision_box->is_lock_y()});
        }

        set_on_hit_event();
        play_hit_sound(hit_position, magnitude(impulse) > 2200.f);
    }

    std::vector<predicted_hit_t> take_predicted_hits() {
        return std::exchange(_predicted_hits, {});
    }

    /* Ids of the bullets that hit this (authoritative) player in the last second */
    [[nodiscard]]
    std::vector<u32> recent_hit_ids() {
        auto now = std::chrono::steady_clock::now();
        while (!_recent_hits.empty() && now - _recent_hits.front().second > std::chrono::seconds(1))
            _recent_hits.pop_front();

        std::vector<u32> ids;
        for (auto& [id, _] : _recent_hits)
            ids.push_back(id);
        return ids;
    }

    void set_on_hit_event() {
        _on_hit_event = true;
    }

    bool pop_on_hit_event() {
        auto res = _on_hit_event;
        _on_hit_event = false;
        return res;
    }
};

inline void player_hit_callback(physic_point*            bullet_pnt,
                                physic_group*            player_grp,
                                collision_result) {
    if (bullet_pnt->get_user_data() == user_data_type::bullet &&
        player_grp->get_user_data() == user_data_type::player && !bullet_pnt->ready_delete_later()) {

        bullet_pnt->delete_later();
        std::any_cast<player*>(player_grp->get_user_any())
            ->on_bullet_hit(bullet_pnt->impulse(), bullet_pnt->get_position(), bullet_ref::of(bullet_pnt));
    }
}
} // namespace dfdh
