#pragma once

#include <SFML/Graphics/RenderWindow.hpp>
#include <algorithm>
#include <filesystem>
#include <list>
#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Texture.hpp>

#include "base/types.hpp"
#include "physic/physic_simulation.hpp"

namespace dfdh {

/*
 * Global bullet gravity flag: when false, newly spawned bullets ignore the
 * physics simulation gravity. game_state::gravity_for_bullets references
 * this variable, so it can be toggled from Lua via GS.gravity_for_bullets
 * (existing bullets keep the gravity flag they were spawned with).
 */
inline bool bullet_gravity_enabled = false;

class bullet_sprite_cache {
public:
    static constexpr float bullet_x_max = 150.f;
    static constexpr float bullet_y = 19.f;

    static bullet_sprite_cache& instance() {
        static bullet_sprite_cache inst;
        return inst;
    }

    bullet_sprite_cache(const bullet_sprite_cache&) = delete;
    bullet_sprite_cache& operator=(const bullet_sprite_cache&) = delete;

    sf::Sprite& sprite(const sf::Color& color) {
        auto found = _sprites.find(color);
        if (found != _sprites.end())
            return found->second;
        else {
            sf::Sprite sprite;
            sprite.setTexture(_txtr);
            sprite.setColor(color);
            auto sz = _txtr.getSize();
            sprite.setOrigin(float(sz.x), float(sz.y) / 2.f);
            sprite.setScale(_xf, _yf);

            return _sprites.emplace(color, std::move(sprite)).first->second;
        }
    }

    [[nodiscard]]
    vec2f scale_f() const {
        return {_xf, _yf};
    }

private:
    bullet_sprite_cache() {
        _txtr.loadFromFile(std::filesystem::current_path() / "data/textures/bullet.png");
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

    std::map<sf::Color, sf::Sprite, comparator_t> _sprites;
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
};

class bullet_mgr {
public:
    template <typename F>
    bullet_mgr(const std::string& name, physic_simulation& sim, F hit_callback): _name("bm_" + name) {
        sim.add_collide_callback(_name, std::move(hit_callback));
    }

    template <typename F>
    auto& shot(physic_simulation& sim,
               const vec2f&       position,
               float              mass,
               const vec2f&       velocity,
               sf::Color          color,
               bool               enabled_gravity,
               int                group,
               F                  player_group_getter) {
        auto& res = _bullets.emplace_back(sim, position, mass, velocity, color, enabled_gravity, group);
        auto& blt = _bullets.back();
        blt.physic()->user_data(user_data_type::bullet);
        if (group != -1)
            blt.physic()->set_collide_allower([=](const physic_point* p) {
                if (p->get_user_data() == user_data_type::player) {
                    int grp = player_group_getter(p);
                    return grp == -1 || grp != group;
                }
                return false;
            });
        return res;
    }

    auto& shot(physic_simulation& sim,
               const vec2f&       position,
               float              mass,
               const vec2f&       velocity,
               sf::Color          color,
               bool               enabled_gravity) {
        auto& res = _bullets.emplace_back(sim, position, mass, velocity, color, enabled_gravity, -1);
        auto& blt = _bullets.back();
        blt.physic()->user_data(user_data_type::bullet);

        return res;
        /*
        if (group != -1)
            blt.physic()->set_collide_allower([](const physic_point* p) {
                if (p->get_user_data() == user_data_type::player) {
                    auto plr = std::any_cast<player*>(player_grp->get_user_any());
                }
            });
        */
    }

    /* Remove bullets flagged with delete_later (the draw loop does this every frame) */
    void remove_dead() {
        for (auto it = _bullets.begin(); it != _bullets.end();) {
            if (it->physic()->ready_delete_later())
                it = _bullets.erase(it);
            else
                ++it;
        }
    }

    /**
     * Remove network-synced bullets (net_id != 0) whose id is not in
     * keep_ids. The client uses this to drop bullets the server already
     * removed (e.g. they hit a player whose local position diverged)
     */
    void remove_not_in(const std::vector<u32>& keep_ids) {
        for (auto it = _bullets.begin(); it != _bullets.end();) {
            if (it->net_id() != 0 &&
                std::find(keep_ids.begin(), keep_ids.end(), it->net_id()) == keep_ids.end()) {
                it->physic()->delete_later();
                it = _bullets.erase(it);
                continue;
            }

            ++it;
        }
    }

    void draw(sf::RenderWindow& wnd, float interpolation_factor, float timestep) {
        remove_dead();

        for (auto& b : _bullets) {
            auto& ph = b.physic();
            if (ph->get_distance() > 5000.f)
                ph->delete_later();

            auto pos = ph->get_position();
            auto next_pos = pos + ph->get_velocity() * timestep;
            pos = lerp(pos, next_pos, interpolation_factor);

            auto dir = ph->get_direction();
            auto angle = std::atan2(dir.y, dir.x);

            auto x_sz = std::min(ph->get_distance(), bullet_sprite_cache::bullet_x_max);
            auto xf   = lerp(0.f,
                           x_sz / bullet_sprite_cache::bullet_x_max,
                           std::clamp(ph->scalar_velocity() / 2100.f, 0.f, 1.f)) *
                      bullet_sprite().scale_f().x;
            auto yf = std::pow(ph->get_mass(), 0.4f) * bullet_sprite().scale_f().y;

            auto& blt_sprite = bullet_sprite().sprite(b.color());
            blt_sprite.setScale(xf, yf);
            blt_sprite.setPosition(pos);
            blt_sprite.setRotation(angle * 180.f / M_PIf32);
            wnd.draw(blt_sprite);
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
