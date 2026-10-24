#pragma once

#include <SFML/Graphics/RenderWindow.hpp>
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

private:
    std::shared_ptr<physic_point> _ph;
    sf::Color _color;
    int _group;
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

    void draw(sf::RenderWindow& wnd, float interpolation_factor, float timestep) {
        for (auto i = _bullets.begin(); i != _bullets.end();) {
            if (i->physic()->ready_delete_later()) {
                _bullets.erase(i++);
                continue;
            }

            if (i->physic()->get_distance() > 5000.f)
                i->physic()->delete_later();

            auto pos = i->physic()->get_position();
            auto next_pos = pos + i->physic()->get_velocity() * timestep;
            pos = lerp(pos, next_pos, interpolation_factor);

            auto dir = i->physic()->get_direction();
            auto angle = std::atan2(dir.y, dir.x);

            auto x_sz = std::min(i->physic()->get_distance(), bullet_sprite_cache::bullet_x_max);
            auto xf   = lerp(0.f,
                           x_sz / bullet_sprite_cache::bullet_x_max,
                           std::clamp(i->physic()->scalar_velocity() / 2100.f, 0.f, 1.f)) *
                      bullet_sprite().scale_f().x;
            auto yf = std::pow(i->physic()->get_mass(), 0.4f) * bullet_sprite().scale_f().y;

            auto& blt_sprite = bullet_sprite().sprite(i->color());
            blt_sprite.setScale(xf, yf);
            blt_sprite.setPosition(pos);
            blt_sprite.setRotation(angle * 180.f / M_PIf32);
            wnd.draw(blt_sprite);

            ++i;
        }
    }

    [[nodiscard]]
    const std::list<bullet>& bullets() const {
        return _bullets;
    }

private:
    std::string       _name;
    std::list<bullet> _bullets;
};
}
