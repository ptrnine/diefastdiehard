#pragma once

#include <array>
#include <memory>
#include <cmath>

#include <SFML/System/Vector2.hpp>
#include <SFML/System/Vector3.hpp>
#include <SFML/Graphics/Rect.hpp>

#include "physic_point.hpp"

namespace dfdh {

class physic_group;

class physic_line : public physic_point {
public:
    physic_line(const vec2f& iposition        = {0.f, 0.f},
                const vec2f& idisplacement    = {1.f, 1.f},
                const vec2f& idir             = {1.f, 0.f},
                float        iscalar_velocity = 0.f,
                float        imass            = 1.f,
                float        ielasticity      = 0.5f):
        physic_point(iposition, idir, iscalar_velocity, imass, ielasticity),
        displacement(idisplacement) {}

    static std::shared_ptr<physic_line> create(const vec2f& iposition        = {0.f, 0.f},
                                               const vec2f& idisplacement    = {1.f, 1.f},
                                               const vec2f& idir             = {1.f, 0.f},
                                               float        iscalar_velocity = 0.f,
                                               float        imass            = 1.f,
                                               float        ielasticity      = 0.5f) {
        return std::make_shared<physic_line>(
            iposition, idisplacement, idir, iscalar_velocity, imass, ielasticity);
    }

    vec2f displacement = {1.f, 1.f};

public:
    void update_bb(float timestep) override {
        if (_fixed) {
            //_bb = sf::FloatRect(-0.5f, -0.5f, 1.f, 1.f);
            return;
        }

        auto pos2 = _position + displacement;

        auto next1 = _position  + get_velocity() * timestep;
        auto next2 = next1 + displacement;

        std::array xs = {_position.x, pos2.x, next1.x, next2.x};
        std::array ys = {_position.y, pos2.y, next1.y, next2.y};

        std::sort(xs.begin(), xs.end());
        std::sort(ys.begin(), ys.end());

        //_bb = sf::FloatRect(-0.5f, -0.5f, 1.f, 1.f);
        _bb.position.x   = xs.front();
        _bb.position.y    = ys.front();
        _bb.size.x  = xs.back() - xs.front();
        _bb.size.y = ys.back() - ys.front();

        if (_bb.size.x < 0.1f)
            _bb.size.x = 0.1f;
        if (_bb.size.y < 0.1f)
            _bb.size.y = 0.1f;

        record_dir_and_velocity();
    }

    [[nodiscard]]
    std::pair<vec2f, vec2f> interpolated_pos2(float timestep, float f) const {
        auto displ = _prev_dir * _prev_scalar_velocity * timestep * f;
        return std::pair{_position + displ, _position + displacement + displ};
    }

    [[nodiscard]]
    /**
     * The line at f of the tick, normalized: (nx, ny, c) with the signed
     * distance nx * x + ny * y + c. The normal is oriented by the line's own
     * direction (p1 -> p2), so it keeps its side while the line moves: a
     * point crossing it changes the sign. (Oriented towards the origin, it
     * flipped when the line crossed the origin: x = 0 for a vertical side,
     * so a bullet passed through a player crossing x = 0 in that tick)
     */
    sf::Vector3f equation(float timestep, float f) const {
        auto [p1, p2] = interpolated_pos2(timestep, f);
        auto a = p2.y - p1.y;
        auto b = p2.x - p1.x;
        auto c = p2.x * p1.y - p1.x * p2.y;
        auto d = 1.f / std::sqrt(a * a + b * b);
        return sf::Vector3f(-a, b, -c) * d;
    }

    [[nodiscard]]
    bounding_box pos_bb() const override {
        auto r1 = sf::FloatRect(get_position(), {0.f, 0.f});
        auto r2 = sf::FloatRect(r1.position + displacement, {0.f, 0.f});
        auto b = bounding_box::maximized();
        b.merge(r1);
        b.merge(r2);
        return b;
    }

    [[nodiscard]]
    bool line_only() const override {
        return true;
    }
};
}
