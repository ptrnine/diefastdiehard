#pragma once

#include <cmath>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "weapon.hpp"
#include "player.hpp"

namespace dfdh {

/**
 * A dropped gun: a free-falling weapon left by its owner.
 *
 * It has a physical bounding box (weapon config "dropped_bb") that can
 * collide with players only (never with players from the owner's group),
 * is affected by the same gravity as players and passes through platforms.
 * On collision it transfers its momentum to the player (impulse = mass *
 * velocity), bounces off losing double of its velocity, and is deleted
 * once it flies far enough out of the level's bounding box.
 *
 * The sprite rotates at "drop_angle_velocity" (degrees/sec); the rotation
 * only affects the sprite, not the physical bounding box.
 *
 * On a network client a gun thrown by the local player is predicted: it
 * flies right away with local physics and collides with the players where
 * they are shown (their knockback is predicted like a bullet hit), takes
 * over the id of the server's gun when it arrives and is then corrected
 * towards it (see net_client). The server tests the gun of a network
 * thrower against the players as the thrower saw them (rewind), so both
 * agree on the hits.
 */
class dropped_gun {
public:
    dropped_gun(physic_simulation& sim,
                const std::string& section,
                const vec2f&       position,
                bool               on_left,
                bool               long_shot,
                int                group,
                const vec2f&       velocity,
                float              angle_velocity,
                float              angle = 0.f):
        _on_left(on_left),
        _long_shot(long_shot),
        _group(group),
        _section(section),
        _angle_velocity(angle_velocity) {
        auto& wpn = weapon_mgr().load(section);
        _wpn      = &wpn;
        _size     = wpn.dropped_bb();
        _angle    = (long_shot ? wpn.long_shot_angle() : 0.f) + angle;

        auto w = _size.x * 0.5f;
        auto h = _size.y * 0.5f;

        auto mk_line = [](const vec2f& p, const vec2f& d) {
            auto l = physic_line::create(p, d);
            /* The dropped gun collides with players only */
            l->set_collide_allower([](const physic_point* p) {
                return p->get_user_data() == user_data_type::player;
            });
            return l;
        };

        /* Box centered on the spawn position */
        _box = physic_group::create();
        _box->append(mk_line({-w, -h}, {0.f, -_size.y}));
        _box->append(mk_line({0.f, -h}, {_size.x, 0.f}));
        _box->append(mk_line({_size.x, -h}, {0.f, _size.y}));
        _box->append(mk_line({_size.x, 0.f}, {-_size.x, 0.f}));

        _box->mass(wpn.mass());
        _box->position(position);
        _box->velocity(velocity);
        _box->enable_gravity();
        _box->user_data(user_data_type::dropped_gun);

        sim.add_primitive(_box);
    }

    ~dropped_gun() {
        _box->delete_later();
    }

    dropped_gun(const dropped_gun&) = delete;
    dropped_gun& operator=(const dropped_gun&) = delete;

    /** Advance the sprite rotation (does not affect the physical bb) */
    void update(float timestep) {
        _age += timestep;

        /* The correction offset decays (half-life ~50ms) */
        _render_offset *= std::exp2(-timestep / render_offset_half_life);
        if (magnitude(_render_offset) < 0.01f)
            _render_offset = {0.f, 0.f};

        /* Ghost (net-synced) guns get their angle from the server */
        if (!_net_synced)
            _angle = std::fmod(_angle + _angle_velocity * timestep, 360.f);
        if (_hit_cooldown > 0)
            --_hit_cooldown;
    }

    /**
     * AABB collision against players, with each player's box player_bb(player,
     * rewind) (rewound for a network thrower). The player receives the gun's
     * impulse (mass * velocity) through on_hit(player, impulse, position), the
     * gun bounces off losing double of its velocity and is pushed out of the
     * player. Returns true on a hit
     */
    template <typename FBB, typename FHit>
    bool collide_with(const std::map<player_name_t, std::shared_ptr<player>>& players, FBB&& player_bb, FHit&& on_hit) {
        /* Ghost (net-synced) guns have no local physics or collisions */
        if (_net_synced || _hit_cooldown > 0 || _box->ready_delete_later())
            return false;

        auto pos = _box->get_position();
        auto gbb = sf::FloatRect(pos.x - _size.x * 0.5f, pos.y - _size.y * 0.5f, _size.x, _size.y);

        for (auto& [_, p] : players) {
            /* Respect the owner's group: no collision with groupmates */
            if (_group != -1 && p->get_group() == _group)
                continue;

            auto pbb = player_bb(*p, _rewind);

            float ox1 = gbb.left + gbb.width  - pbb.left;
            float ox2 = pbb.left + pbb.width  - gbb.left;
            float oy1 = gbb.top  + gbb.height - pbb.top;
            float oy2 = pbb.top  + pbb.height - gbb.top;
            if (ox1 <= 0.f || ox2 <= 0.f || oy1 <= 0.f || oy2 <= 0.f)
                continue;

            auto imp = _box->get_velocity() * _box->get_mass(); /* mass * velocity */
            on_hit(*p, imp, pos);
            _last_hit_age = _age;

            /* The gun loses double of its velocity (perfect bounce) */
            auto vel = _box->get_velocity();
            _box->velocity(-vel * 0.5f);

            /* Push the gun out along the axis of least penetration */
            if (std::min(ox1, ox2) < std::min(oy1, oy2))
                pos.x += ox1 < ox2 ? -ox1 : ox2;
            else
                pos.y += oy1 < oy2 ? -oy1 : oy2;
            _box->position(pos);

            _hit_cooldown = 6; /* 0.1 s immunity while still overlapping */
            return true;
        }
        return false;
    }

    /** True if the gun has flown past the level's bounding box by the margin */
    [[nodiscard]]
    bool out_of_bounds(const vec2f& level_size) const {
        constexpr float margin = 200.f;
        if (std::abs(level_size.x) < 1.f || std::abs(level_size.y) < 1.f)
            return false;

        auto pos = _box->get_position();
        return pos.x < -margin || pos.x > level_size.x + margin ||
               pos.y < -margin || pos.y > level_size.y + margin;
    }

    void draw(sf::RenderWindow& wnd, float interpolation_factor, float timestep) const {
        auto pos      = _box->get_position();
        auto next_pos = pos + _box->get_velocity() * timestep;
        pos = lerp(pos, next_pos, interpolation_factor);
        _wpn->draw(pos + _render_offset, _on_left, _angle, wnd);
    }

    [[nodiscard]]
    const std::shared_ptr<physic_group>& box() const {
        return _box;
    }

    [[nodiscard]]
    int group() const {
        return _group;
    }

    /** Unique id for network sync (assigned by the manager) */
    void id(u32 value) {
        _id = value;
    }

    [[nodiscard]]
    u32 id() const {
        return _id;
    }

    [[nodiscard]]
    const std::string& section() const {
        return _section;
    }

    [[nodiscard]]
    bool on_left() const {
        return _on_left;
    }

    [[nodiscard]]
    bool long_shot() const {
        return _long_shot;
    }

    /** Sprite rotation in degrees, pre-mirror (as stored server-side) */
    [[nodiscard]]
    float angle() const {
        return _angle;
    }

    /** Overwrite the sprite rotation (network sync) */
    void sync_angle(float value) {
        _angle = std::fmod(value, 360.f);
    }

    [[nodiscard]]
    vec2f get_position() const {
        return _box->get_position();
    }

    [[nodiscard]]
    vec2f get_velocity() const {
        return _box->get_velocity();
    }

    void position(const vec2f& value) {
        _box->position(value);
    }

    void velocity(const vec2f& value) {
        _box->velocity(value);
    }

    /**
     * Network-synced (ghost) gun: no local gravity, no collisions, no sprite
     * rotation. Position, velocity and angle are driven by the server state
     */
    void net_sync(bool value) {
        _net_synced = value;
        _box->enable_gravity(!value);
    }

    [[nodiscard]]
    bool net_synced() const {
        return _net_synced;
    }

    /* Player who threw the gun (empty if none) */
    void owner(const player_name_t& value) {
        _owner = value;
    }

    [[nodiscard]]
    const player_name_t& owner() const {
        return _owner;
    }

    /* Flight time since the spawn (simulation seconds) */
    [[nodiscard]]
    float age() const {
        return _age;
    }

    [[nodiscard]]
    float angle_velocity() const {
        return _angle_velocity;
    }

    /** Client-side prediction of the local player's throw */
    void predicted(bool value) {
        _predicted = value;
    }

    [[nodiscard]]
    bool predicted() const {
        return _predicted;
    }

    /** The server's gun arrived: the prediction takes its id */
    void confirm(u32 server_id) {
        _id        = server_id;
        _confirmed = true;
    }

    [[nodiscard]]
    bool confirmed() const {
        return _confirmed;
    }

    /** Move the gun to the corrected state, the jump is smoothed by the render offset */
    void correct(const vec2f& pos, const vec2f& vel, float angle) {
        _render_offset += _box->get_position() - pos;
        _box->position(pos);
        _box->velocity(vel);
        sync_angle(angle);
    }

    [[nodiscard]]
    const vec2f& render_offset() const {
        return _render_offset;
    }

    /* Flight time of the last player hit (bounce), if any */
    [[nodiscard]]
    const std::optional<float>& last_hit_age() const {
        return _last_hit_age;
    }

    /* Server, gun of a network thrower: collide with the players as they were rewind seconds ago */
    void rewind(float value) {
        _rewind = value;
    }

    [[nodiscard]]
    float rewind() const {
        return _rewind;
    }

    /*
     * Id of the gun's hits, reported like bullet ids (state_player::hits):
     * tagged to never match a bullet id, an unconfirmed prediction (client
     * side id) also never matches a server gun
     */
    static constexpr u32 hit_ref_tag            = 0x80000000u;
    static constexpr u32 hit_ref_predicted_tag  = 0x40000000u;

    [[nodiscard]]
    static u32 hit_ref_id_of(u32 gun_id, bool unconfirmed_prediction = false) {
        return hit_ref_tag | (unconfirmed_prediction ? hit_ref_predicted_tag : 0u) | (gun_id & ~(hit_ref_tag | hit_ref_predicted_tag));
    }

    [[nodiscard]]
    u32 hit_ref_id() const {
        return hit_ref_id_of(_id, _predicted && !_confirmed);
    }

private:
    static constexpr float render_offset_half_life = 0.05f;

    std::shared_ptr<physic_group> _box;
    weapon*                       _wpn      = nullptr;
    vec2f                         _size;
    bool                          _on_left;
    bool                          _long_shot;
    int                           _group;
    std::string                   _section;
    float                         _angle;
    float                         _angle_velocity;
    u32                           _id           = 0;
    bool                          _net_synced   = false;
    u32                           _hit_cooldown = 0;
    player_name_t                 _owner;
    float                         _age          = 0.f;
    bool                          _predicted    = false;
    bool                          _confirmed    = false;
    vec2f                         _render_offset = {0.f, 0.f};
    float                         _rewind        = 0.f;
    std::optional<float>          _last_hit_age;
};

class dropped_gun_mgr {
public:
    dropped_gun_mgr(physic_simulation&                            sim,
                    std::map<player_name_t, std::shared_ptr<player>>& players):
        _sim(sim), _players(players) {
        _sim.add_update_callback("dropped_gun", [this](const physic_simulation&, float timestep) {
            update(timestep);
        });
    }

    dropped_gun_mgr(const dropped_gun_mgr&) = delete;
    dropped_gun_mgr& operator=(const dropped_gun_mgr&) = delete;

    dropped_gun* spawn(const std::string& section,
                       const vec2f&       position,
                       bool               on_left,
                       bool               long_shot,
                       int                group,
                       const vec2f&       velocity,
                       float              angle_velocity,
                       float              angle = 0.f) {
        auto* gun = _guns
                        .emplace_back(std::make_unique<dropped_gun>(
                            _sim, section, position, on_left, long_shot, group, velocity, angle_velocity, angle))
                        .get();
        gun->id(_next_id++);
        return gun;
    }

    /**
     * Create a network-synced ghost gun (client side only): no local
     * physics, driven by the server state broadcast
     */
    dropped_gun* spawn_net(u32               id,
                           const std::string& section,
                           const vec2f&       position,
                           bool               on_left,
                           bool               long_shot) {
        auto* gun = spawn(section, position, on_left, long_shot, -1, {0.f, 0.f}, 0.f);
        gun->id(id);
        gun->net_sync(true);
        return gun;
    }

    void set_level_size(const vec2f& value) {
        _level_size = value;
    }

    void update(float timestep) {
        for (auto& g : _guns)
            g->update(timestep);

        for (auto& g : _guns)
            g->collide_with(_players, _player_bb, [&](player& p, const vec2f& impulse, const vec2f& pos) {
                _on_hit(*g, p, impulse, pos);
            });

        /* Ghost and predicted guns are removed by the client when the server drops them */
        std::erase_if(_guns, [this](const auto& g) {
            return g->box()->ready_delete_later() ||
                   (!g->net_synced() && !g->predicted() && g->out_of_bounds(_level_size));
        });
    }

    void draw(sf::RenderWindow& wnd, float interpolation_factor, float timestep) const {
        for (auto& g : _guns)
            g->draw(wnd, interpolation_factor, timestep);
    }

    [[nodiscard]]
    std::size_t size() const {
        return _guns.size();
    }

    using player_bb_fn_t = std::function<sf::FloatRect(const player&, float rewind)>;
    using on_hit_fn_t    = std::function<void(dropped_gun&, player&, const vec2f& impulse, const vec2f& position)>;

    /* Player boxes for the collisions (rewound ones) and what a gun hit does to the player */
    void set_collision_handlers(player_bb_fn_t player_bb, on_hit_fn_t on_hit) {
        _player_bb = std::move(player_bb);
        _on_hit    = std::move(on_hit);
    }

    [[nodiscard]]
    auto& guns() {
        return _guns;
    }

private:
    physic_simulation&                              _sim;
    std::map<player_name_t, std::shared_ptr<player>>& _players;
    vec2f                                           _level_size = {0.f, 0.f};
    std::vector<std::unique_ptr<dropped_gun>>       _guns;
    u32                                             _next_id = 1;

    player_bb_fn_t _player_bb = [](const player& p, float) { return p.bb(); };
    on_hit_fn_t    _on_hit    = [](dropped_gun&, player& p, const vec2f& impulse, const vec2f& pos) {
        p.on_bullet_hit(impulse, pos);
    };
};

} // namespace dfdh
