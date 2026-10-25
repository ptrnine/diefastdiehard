#pragma once

#include <cmath>
#include <memory>
#include <map>
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
                float              angle_velocity):
        _on_left(on_left),
        _group(group),
        _angle(long_shot ? weapon_mgr().load(section).long_shot_angle() : 0.f),
        _angle_velocity(angle_velocity) {
        auto& wpn = weapon_mgr().load(section);
        _wpn      = &wpn;
        _size     = wpn.dropped_bb();

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
        _angle = std::fmod(_angle + _angle_velocity * timestep, 360.f);
        if (_hit_cooldown > 0)
            --_hit_cooldown;
    }

    /**
     * AABB collision against players. The player receives the gun's impulse
     * (mass * velocity), the gun bounces off losing double of its velocity
     * and is pushed out of the player. Returns true on a hit
     */
    bool collide_with(const std::map<player_name_t, std::shared_ptr<player>>& players) {
        if (_hit_cooldown > 0 || _box->ready_delete_later())
            return false;

        auto pos = _box->get_position();
        auto gbb = sf::FloatRect(pos.x - _size.x * 0.5f, pos.y - _size.y * 0.5f, _size.x, _size.y);

        for (auto& [_, p] : players) {
            /* Respect the owner's group: no collision with groupmates */
            if (_group != -1 && p->get_group() == _group)
                continue;

            auto ppos = p->get_position();
            auto psz  = p->get_size();
            auto pbb  = sf::FloatRect(ppos.x, ppos.y - psz.y, psz.x, psz.y);

            float ox1 = gbb.left + gbb.width  - pbb.left;
            float ox2 = pbb.left + pbb.width  - gbb.left;
            float oy1 = gbb.top  + gbb.height - pbb.top;
            float oy2 = pbb.top  + pbb.height - gbb.top;
            if (ox1 <= 0.f || ox2 <= 0.f || oy1 <= 0.f || oy2 <= 0.f)
                continue;

            auto& pbox = *p->collision_box();
            auto  imp  = _box->get_velocity() * _box->get_mass(); /* mass * velocity */
            if (pbox.is_lock_y() && imp.y < -180.f)
                pbox.unlock_y();
            pbox.apply_impulse(imp);

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
        _wpn->draw(pos, _on_left, _angle, wnd);
    }

    [[nodiscard]]
    const std::shared_ptr<physic_group>& box() const {
        return _box;
    }

    [[nodiscard]]
    int group() const {
        return _group;
    }

private:
    std::shared_ptr<physic_group> _box;
    weapon*                       _wpn      = nullptr;
    vec2f                         _size;
    bool                          _on_left;
    int                           _group;
    float                         _angle;
    float                         _angle_velocity;
    u32                           _hit_cooldown = 0;
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
                       float              angle_velocity) {
        auto g   = std::make_unique<dropped_gun>(_sim,
                                                 section,
                                                 position,
                                                 on_left,
                                                 long_shot,
                                                 group,
                                                 velocity,
                                                 angle_velocity);
        auto* ptr = g.get();
        _guns.push_back(std::move(g));
        return ptr;
    }

    void set_level_size(const vec2f& value) {
        _level_size = value;
    }

    void update(float timestep) {
        for (auto& g : _guns)
            g->update(timestep);

        for (auto& g : _guns)
            g->collide_with(_players);

        for (auto it = _guns.begin(); it != _guns.end();) {
            if ((*it)->box()->ready_delete_later() || (*it)->out_of_bounds(_level_size))
                it = _guns.erase(it);
            else
                ++it;
        }
    }

    void draw(sf::RenderWindow& wnd, float interpolation_factor, float timestep) const {
        for (auto& g : _guns)
            g->draw(wnd, interpolation_factor, timestep);
    }

    [[nodiscard]]
    std::size_t size() const {
        return _guns.size();
    }

private:
    physic_simulation&                              _sim;
    std::map<player_name_t, std::shared_ptr<player>>& _players;
    vec2f                                           _level_size = {0.f, 0.f};
    std::vector<std::unique_ptr<dropped_gun>>       _guns;
};

} // namespace dfdh
