#pragma once

#include <SFML/Graphics/RenderWindow.hpp>
#include <algorithm>
#include <filesystem>
#include <list>
#include <optional>
#include "efx/efx.hpp"
#include "sprite.hpp"
#include <SFML/Graphics/Texture.hpp>

#include "base/types.hpp"
#include "base/fixed_string.hpp"
#include "physic/physic_simulation.hpp"

namespace dfdh {

/*
 * Global bullet gravity flag: when false, newly spawned bullets ignore the
 * physics simulation gravity. game_state::gravity_for_bullets references
 * this variable, so it can be toggled from Lua via GS.gravity_for_bullets
 * (existing bullets keep the gravity flag they were spawned with).
 */
inline bool bullet_gravity_enabled = false;

/* Identifies a bullet of a network player's shot on both the client and the server */
struct shot_key_t {
    u32 pull_seq   = 0; // input tick of the trigger pull
    u32 shot_index = 0; // shot number since the pull
    u32 pellet     = 0; // pellet number in the shot

    bool operator==(const shot_key_t&) const = default;
};

/**
 * Network identity of a bullet: the server id, or (own bullet predicted on
 * the client, not confirmed yet) its shot key. Stored on the bullet's
 * physic point, so hits can be matched between the client and the server
 */
struct bullet_ref {
    u32                       id = 0;
    std::optional<shot_key_t> key;
    player_name_t             owner = {}; /* the shooter (or thrower), local only: not sent */

    [[nodiscard]]
    static bullet_ref of(const physic_point* ph) {
        auto ref = std::any_cast<bullet_ref>(&ph->get_user_any());
        return ref ? *ref : bullet_ref{};
    }
};

class bullet_sprite_cache {
public:
    static constexpr float bullet_x_max = 150.f;
    static constexpr float bullet_y = 19.f;

    static bullet_sprite_cache& instance() {
        /*
         * Intentionally leaked: destroying the cached Texture at process exit runs
         * sf::Texture destructors after the window/GL context is already gone,
         * which crashes in the GL driver (on Windows, inside gdi32full.dll).
         */
        static bullet_sprite_cache* inst = new bullet_sprite_cache; // NOLINT
        return *inst;
    }

    bullet_sprite_cache(const bullet_sprite_cache&) = delete;
    bullet_sprite_cache& operator=(const bullet_sprite_cache&) = delete;

    sprite_t& sprite(const sf::Color& color) {
        auto [it, inserted] = _sprites.try_emplace(color);
        if (inserted) {
            auto& sprite = it->second;
            sprite.setTexture(_txtr);
            sprite.setColor(color);
            auto sz = _txtr.getSize();
            sprite.setOrigin({float(sz.x), float(sz.y) / 2.f});
            sprite.setScale({_xf, _yf});
        }
        return it->second;
    }

    [[nodiscard]]
    vec2f scale_f() const {
        return {_xf, _yf};
    }

private:
    bullet_sprite_cache() {
        (void)_txtr.loadFromFile(std::filesystem::current_path() / "data/textures/bullet.png"); // SFML reports the failure
        _txtr.setSmooth(true);
        auto sz = _txtr.getSize();
        _xf = bullet_x_max / float(sz.x);
        _yf = bullet_y / float(sz.y);
    }

    ~bullet_sprite_cache() = default;

private:
    struct comparator_t {
        bool operator()(const sf::Color& l, const sf::Color& r) const {
            return l.toInteger() < r.toInteger();
        }
    };

    std::map<sf::Color, sprite_t, comparator_t> _sprites;
    sf::Texture _txtr;
    float       _xf;
    float       _yf;
};


inline bullet_sprite_cache& bullet_sprite() {
    return bullet_sprite_cache::instance();
}

class bullet {
public:
    bullet(physic_simulation& sim,
           const vec2f&       position,
           float              mass,
           const vec2f&       velocity,
           sf::Color          color,
           bool               enabled_gravity,
           int                group):
        _color(color), _group(group) {
        _ph = physic_point::create(position, normalize(velocity), magnitude(velocity), mass);
        _ph->enable_gravity(enabled_gravity);
        sim.add_primitive(_ph);
    }

    bullet(const bullet&) = delete;
    bullet& operator=(const bullet&) = delete;
    bullet(bullet&&) = delete;
    bullet& operator=(bullet&&) = delete;

    [[nodiscard]]
    auto& physic() {
        return _ph;
    }

    [[nodiscard]]
    auto& physic() const {
        return _ph;
    }

    [[nodiscard]]
    int group() const {
        return _group;
    }

    [[nodiscard]]
    sf::Color color() const {
        return _color;
    }

    /*
     * Unique network id assigned by the server to the bullets it spawns;
     * the client stores it on the bullet it created from the broadcast so
     * it can drop the bullet when the server no longer lists it as alive.
     * 0 = not network-synced
     */
    void net_id(u32 id) {
        _net_id = id;
        _ph->user_any(bullet_ref{id, {}, _owner});
    }

    /**
     * Lag-compensated bullet (server, network shooter): it hits the players
     * as they were rewind_s ago, as the shooter saw them, for its whole
     * flight. Such a bullet does not collide with players physically, the
     * game sweeps it against the rewound players every tick
     */
    void lag_compensate(const player_name_t& owner_name, float rewind_s) {
        owner(owner_name);
        _rewind     = rewind_s;
        _swept_from = _ph->get_position();
        _ph->set_collide_allower([](const physic_point*) { return false; });
    }

    [[nodiscard]]
    float rewind() const {
        return _rewind;
    }

    /* Muzzle trajectory of a bullet simulated in the client's predicted frame */
    struct framed_t {
        vec2f start;
        vec2f velocity;
        bool  gravity;
        float rewind;        // the server tests it against the players this long ago (0: collides physically), s
        float flight;        // flight time along the trajectory now, s
        float shown_flight;  // flight time the shown players were tested up to, s
        bool  own = false;   // the local player's predicted bullet: swept against the remote players only
        std::optional<double> view = {}; // own: the server time shown at the shot's tick (net::input::view_base)
    };

    /* Drawn behind its position, catching up: drawn this much faster until caught up */
    static constexpr float catch_up_speed = 2.f;

    /*
     * Spawned ahead of the muzzle (server: a network shooter's bullet,
     * forwarded by the shooter's lag; client: a server bullet in the local
     * frame, see net_client::spawn_server_bullet): drawn behind seconds
     * earlier in its flight, from the muzzle, catching up at catch_up_speed
     * (updated every tick: game_state::bullets_catch_up_update)
     */
    struct catch_up_t {
        float behind;                // s
        vec2f gap      = {0.f, 0.f}; // drawn position minus its position
        vec2f gap_vel  = {0.f, 0.f}; // drawn velocity minus its velocity (under gravity: earlier in the flight)
        vec2f step_vel = {0.f, 0.f}; // how fast the catch-up moves it on until the next tick
    };

    [[nodiscard]]
    std::optional<catch_up_t>& catch_up() {
        return _catch_up;
    }

    [[nodiscard]]
    const std::optional<catch_up_t>& catch_up() const {
        return _catch_up;
    }

    /**
     * Server bullet on a client, simulated in the frame of the predicted
     * local player: it is where the server will have it when it tests the
     * local player's current tick, ahead of the remote players (shown in the
     * past). It does not collide with players physically, the client sweeps
     * it against the local player and, delayed along the muzzle trajectory,
     * against the remote players every tick (net_client). The first sweep
     * starts at from
     */
    void frame_on_client(const framed_t& value, const vec2f& from) {
        _framed     = value;
        _swept_from = from;
        _ph->set_collide_allower([group = _group](const physic_point* p) {
            return p->get_user_data() != user_data_type::player && group == -1;
        });
    }

    [[nodiscard]]
    std::optional<framed_t>& framed() {
        return _framed;
    }

    [[nodiscard]]
    const std::optional<framed_t>& framed() const {
        return _framed;
    }

    /* The next sweep starts where the bullet is now (it was moved, e.g. corrected) */
    void restart_sweep() {
        _swept_from = _ph->get_position();
    }

    [[nodiscard]]
    const player_name_t& owner() const {
        return _owner;
    }

    /* The shooter, credited for the hits of the bullet (see player::attacker) */
    void owner(const player_name_t& value) {
        _owner = value;
        if (auto ref = std::any_cast<bullet_ref>(&_ph->get_user_any()))
            ref->owner = value;
        else
            _ph->user_any(bullet_ref{0, {}, value});
    }

    /* Path since the last sweep (lag-compensated and framed bullets) */
    std::pair<vec2f, vec2f> take_swept_segment() {
        auto from   = _swept_from;
        _swept_from = _ph->get_position();
        return {from, _swept_from};
    }

    /* Predicted own bullet (client): referenced by its shot key until confirmed */
    void predicted_key(const shot_key_t& key) {
        _ph->user_any(bullet_ref{0, key, _owner});
    }

    [[nodiscard]]
    u32 net_id() const {
        return _net_id;
    }

private:
    std::shared_ptr<physic_point> _ph;
    sf::Color _color;
    int _group;
    u32 _net_id = 0;

    player_name_t _owner;
    float         _rewind     = 0.f;
    vec2f         _swept_from = {0.f, 0.f};

    std::optional<framed_t>   _framed;
    std::optional<catch_up_t> _catch_up;
};

class bullet_mgr {
public:
    template <typename F>
    bullet_mgr(const std::string& name, physic_simulation& sim, F hit_callback): _name("bm_" + name) {
        sim.add_collide_callback(_name, std::move(hit_callback));
    }

    /**
     * A bullet of a player: never hits its owner (is_owner(physic point)),
     * nor the players of the owner's group. A grouped bullet hits players only
     */
    template <typename F, typename FOwner = std::nullptr_t>
    bullet& shot(physic_simulation& sim,
               const vec2f&       position,
               float              mass,
               const vec2f&       velocity,
               sf::Color          color,
               bool               enabled_gravity,
               int                group,
               F                  player_group_getter,
               FOwner             is_owner = nullptr) {
        auto& res = shot(sim, position, mass, velocity, color, enabled_gravity, group);
        if (group != -1 || !std::is_same_v<FOwner, std::nullptr_t>)
            res.physic()->set_collide_allower([=](const physic_point* p) {
                if (p->get_user_data() != user_data_type::player)
                    return group == -1;
                if constexpr (!std::is_same_v<FOwner, std::nullptr_t>)
                    if (is_owner(p))
                        return false;
                int grp = player_group_getter(p);
                return group == -1 || grp == -1 || grp != group;
            });
        return res;
    }

    bullet& shot(physic_simulation& sim,
               const vec2f&       position,
               float              mass,
               const vec2f&       velocity,
               sf::Color          color,
               bool               enabled_gravity,
               int                group = -1) {
        auto& res = _bullets.emplace_back(sim, position, mass, velocity, color, enabled_gravity, group);
        res.physic()->user_data(user_data_type::bullet);
        return res;
    }

    /* Remove bullets flagged with delete_later (the draw loop does this every frame) */
    void remove_dead() {
        _bullets.remove_if([](const bullet& b) { return b.physic()->ready_delete_later(); });
    }

    /**
     * Remove network-synced bullets (net_id != 0) whose id keep(id) rejects.
     * The client uses this to drop bullets the server already removed (e.g.
     * they hit a player whose local position diverged)
     */
    template <typename F>
    void remove_not_in(F&& keep) {
        _bullets.remove_if([&](const bullet& b) {
            if (b.net_id() == 0 || keep(b.net_id()))
                return false;
            b.physic()->delete_later();
            return true;
        });
    }

    void draw(sf::RenderWindow& wnd, float interpolation_factor, float timestep) {
        remove_dead();

        auto& sprites = bullet_sprite();
        auto  scale_f = sprites.scale_f();

        for (auto& b : _bullets) {
            auto& ph = b.physic();
            if (ph->get_distance() > 5000.f)
                ph->delete_later();

            auto pos = ph->get_position();
            auto next_pos = pos + ph->get_velocity() * timestep;
            pos = lerp(pos, next_pos, interpolation_factor);
            auto dir = ph->get_direction();
            /* Spawned ahead of the muzzle: drawn behind, catching up */
            if (auto& cu = b.catch_up()) {
                pos += cu->gap + cu->step_vel * (interpolation_factor * timestep);
                if (auto drawn_vel = ph->get_velocity() + cu->gap_vel; magnitude(drawn_vel) > 1.f)
                    dir = normalize(drawn_vel);
            }

            auto angle = std::atan2(dir.y, dir.x);

            auto x_sz = std::min(ph->get_distance(), bullet_sprite_cache::bullet_x_max);
            auto xf   = lerp(0.f,
                           x_sz / bullet_sprite_cache::bullet_x_max,
                           std::clamp(ph->scalar_velocity() / 2100.f, 0.f, 1.f)) *
                      scale_f.x;
            auto yf = std::pow(ph->get_mass(), 0.4f) * scale_f.y;

            auto& blt_sprite = sprites.sprite(b.color());
            blt_sprite.setScale({xf, yf});
            blt_sprite.setPosition(pos);
            blt_sprite.setRotation(sf::radians(angle));
            wnd.draw(blt_sprite);

            if (magnitude(ph->impulse()) > 2200.f) {
                efxmgr().play("bullet_trail", ph->get_position());
            }
        }
    }

    [[nodiscard]]
    const std::list<bullet>& bullets() const {
        return _bullets;
    }

    [[nodiscard]]
    std::list<bullet>& bullets() {
        return _bullets;
    }

private:
    std::string       _name;
    std::list<bullet> _bullets;
};
}
