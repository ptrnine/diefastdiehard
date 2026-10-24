#pragma once

#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/Graphics/Rect.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "base/types.hpp"
#include "base/cfg.hpp"
#include "base/log.hpp"
#include "base/vec_math.hpp"
#include "player.hpp"
#include "weapon.hpp"
#include "texture_mgr.hpp"

namespace dfdh {

/**
 * Crate loot table (data/crate_table.txt).
 * Format: one "<weapon_name> <weight>" per line, '#' starts a comment.
 * Weight defines the relative spawn chance (e.g. "1" and "2" -> 1/3 and 2/3).
 * Pistol-class weapons are never picked, and only weapons listed in the
 * table can be spawned.
 */
struct crate_loot_table {
    struct entry {
        std::string weapon_section;
        float       weight;
    };

    static std::vector<entry> load() {
        std::vector<entry> table;

        auto path = std::filesystem::current_path() / "data/crate_table.txt";
        std::ifstream file(path);
        if (!file) {
            glog().error("can't open crate loot table {}", path.string());
            return table;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.starts_with("#"))
                continue;

            std::istringstream ss(line);
            std::string name;
            float weight;
            if (!(ss >> name >> weight) || weight <= 0.f)
                continue;

            if (!name.starts_with("wpn_"))
                name = "wpn_" + name;

            if (!weapon_mgr().is_exists(name)) {
                glog().warn("crate loot table: unknown weapon {}", name);
                continue;
            }

            if (auto sect = cfg::global().try_get_section(name)) {
                auto class_opt = sect->try_get<std::string>("class");
                if (class_opt && class_opt->value() == "pistol")
                    continue;
            }

            table.push_back(entry{name, weight});
        }

        return table;
    }

    static std::optional<std::string> random_pick() {
        auto table = load();
        if (table.empty())
            return {};

        float sum = 0.f;
        for (auto& e : table)
            sum += e.weight;

        float roll = rand_float(0.f, sum);
        for (auto& e : table) {
            roll -= e.weight;
            if (roll <= 0.f)
                return e.weapon_section;
        }
        return table.back().weapon_section;
    }
};

/**
 * A falling loot crate.
 *
 * Spawns above the map, falls under gravity, passes through every platform
 * until it reaches its target platform, where it snaps down and becomes
 * unmovable (fixed). It is immune to bullets. When a player's bounding box
 * overlaps the crate's bounding box, the crate is taken and the player
 * receives a random weapon from the crate loot table.
 */
class crate {
public:
    /* Crate is a square with the side of half a player's height */
    static vec2f default_size() {
        auto psize = player::default_size();
        return {psize.y / 2.f, psize.y / 2.f};
    }

    crate(physic_simulation& sim, const vec2f& spawn_pos, float target_y):
        _size(default_size()), _target_y(target_y) {
        auto sz = _size;

        auto mk_line = [](const vec2f& p, const vec2f& d) {
            auto l = physic_line::create(p, d);
            /* Crates are not affected by bullets */
            l->set_collide_allower([](const physic_point* p) {
                return p->get_user_data() != user_data_type::bullet;
            });
            return l;
        };

        _box = physic_group::create();
        _box->append(mk_line({0.f, 0.f}, {0.f, -sz.y}));
        _box->append(mk_line({0.f, -sz.y}, {sz.x, 0.f}));
        _box->append(mk_line({sz.x, -sz.y}, {0.f, sz.y}));
        _box->append(mk_line({sz.x, 0.f}, {-sz.x, 0.f}));

        _box->position(spawn_pos);
        _box->enable_gravity();
        _box->user_data(user_data_type::crate);
        _box->user_any(this);

        sim.add_primitive(_box);

        _txtr = texture_mgr().load("items/crate.png");
        auto txtr_sz = _txtr.getSize();
        _sprite.setTexture(_txtr);
        _sprite.setScale(sz.x / float(txtr_sz.x), sz.y / float(txtr_sz.y));
    }

    ~crate() {
        _box->delete_later();
    }

    crate(const crate&) = delete;
    crate& operator=(const crate&) = delete;

    /**
     * Mark the crate as server-synchronized (client side). A net-synced crate
     * has no local physics: position and velocity are driven by the server
     * state broadcast, it never lands locally and can't be picked up
     * locally (the server is authoritative for pickup)
     */
    void net_sync(bool value) {
        _net_synced = value;
        _box->enable_gravity(!value);
    }

    [[nodiscard]]
    bool net_synced() const {
        return _net_synced;
    }

    void id(u32 value) {
        _id = value;
    }

    [[nodiscard]]
    u32 id() const {
        return _id;
    }

    void position(const vec2f& value) {
        _box->position(value);
    }

    [[nodiscard]]
    vec2f get_position() const {
        return _box->get_position();
    }

    void velocity(const vec2f& value) {
        _box->velocity(value);
    }

    [[nodiscard]]
    vec2f get_velocity() const {
        return _box->get_velocity();
    }

    /** Snap the crate onto its target platform once it has reached it */
    void update() {
        if (_landed || _net_synced)
            return;

        auto pos = _box->get_position();
        if (pos.y >= _target_y) {
            pos.y = _target_y + 0.001f;
            _box->position(pos);
            _box->velocity({0.f, 0.f});
            _box->fixed(true); /* unmovable */
            _landed = true;
            glog().info("crate landed on ({}, {})", pos.x, pos.y);
        }
    }

    [[nodiscard]]
    bool landed() const {
        return _landed;
    }

    /** Crate AABB (bottom-left origin, SFML y-down) */
    [[nodiscard]]
    sf::FloatRect bb() const {
        auto p = _box->get_position();
        return sf::FloatRect(p.x, p.y - _size.y, _size.x, _size.y);
    }

    void draw(sf::RenderWindow& wnd, float interpolation_factor, float timestep) {
        auto pos      = _box->get_position();
        auto next_pos = pos + _box->get_velocity() * timestep;
        pos = lerp(pos, next_pos, interpolation_factor);

        _sprite.setPosition(pos.x, pos.y - _size.y);
        wnd.draw(_sprite);
    }

private:
    std::shared_ptr<physic_group> _box;
    vec2f                         _size;
    float                         _target_y;
    bool                          _landed      = false;
    bool                          _net_synced  = false;
    u32                           _id          = 0;

    sf::Texture _txtr;
    sf::Sprite  _sprite;
};

} // namespace dfdh
