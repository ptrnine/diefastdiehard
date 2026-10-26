#pragma once

#include <memory>
#include <cmath>
#include <any>
#include <functional>

#include <SFML/Graphics/Rect.hpp>

#include "base/types.hpp"
#include "base/vec2.hpp"
#include "base/vec_math.hpp"

namespace dfdh {

struct user_data_type {
    enum : u64 { player = 0xdeadf00d, adjustment_box = 0xdeaddead, bullet = 0xdeadbeef, crate = 0xdeadc47e, dropped_gun = 0xdead4742 };
};

class physic_group;

class physic_point {
public:
    friend class physic_group;
    friend class physic_simulation;

    static std::shared_ptr<physic_point> create(const vec2f& iposition        = {0.f, 0.f},
                                                const vec2f& idir             = {1.f, 0.f},
                                                float        iscalar_velocity = 0.f,
                                                float        imass            = 1.f,
                                                float        ielasticity      = 0.5f) {
        return std::make_shared<physic_point>(
            iposition, idir, iscalar_velocity, imass, ielasticity);
    }

    physic_point(const vec2f& iposition        = {0.f, 0.f},
                 const vec2f& idir             = {1.f, 0.f},
                 float        iscalar_velocity = 0.f,
                 float        imass            = 1.f,
                 float        ielasticity      = 0.5f):
        _position(iposition),
        _dir(idir),
        _scalar_velocity(iscalar_velocity),
        _mass(imass),
        _elasticity(ielasticity),
        _delete_later(false),
        _fixed(false),
        _enable_gravity(false),
        _lock_y(false),
        _allow_platform(false),
        _user_data(0),
        _distance(0.f) {
        _bb = {0.f, 0.f, 0.f, 0.f};
    }

    virtual ~physic_point() = default;

    virtual void update_bb(float timestep) {
        auto next = _position + get_velocity() * timestep;
        //_bb = sf::FloatRect(-0.5f, -0.5f, 1.f, 1.f);

        _bb.left   = std::min(_position.x, next.x);
        _bb.top    = std::min(_position.y, next.y);
        _bb.width  = std::max(_position.x, next.x) - std::min(_position.x, next.x);
        _bb.height = std::max(_position.y, next.y) - std::min(_position.y, next.y);

        if (_bb.width < 0.1f)
            _bb.width = 0.1f;
        if (_bb.height < 0.1f)
            _bb.height = 0.1f;

        record_dir_and_velocity();
    }

    virtual void move(float timestep) {
        /* TODO: move group if it is child */
        auto mov = get_velocity() * timestep;
        _position += mov;
        _distance += magnitude(mov);
    }

    virtual void apply_impulse(const vec2f& value) {
        /* TODO: apply to group if it is child */
        velocity(get_velocity() + value / get_mass());
    }

    [[nodiscard]]
    vec2f interpolated_pos(float timestep, float f) const {
        return _position + _prev_dir * _prev_scalar_velocity * timestep * f;
    }

    [[nodiscard]]
    vec2f g_force(float timestep) const {
        return ((_dir * _scalar_velocity) - (_prev_dir * _prev_scalar_velocity)) / timestep;
    }

protected:
    vec2f                       _position;
    vec2f                       _dir;
    float                       _scalar_velocity;
    float                       _mass;
    float                       _elasticity;
    std::weak_ptr<physic_group> _group;
    sf::FloatRect               _bb;
    bool                        _delete_later;
    bool                        _fixed;
    bool                        _enable_gravity;
    bool                        _lock_y;
    bool                        _allow_platform;
    u64                         _user_data;
    std::any                    _user_any;

    std::function<bool(const physic_point*)> _collide_allower;
    float                                    _distance;

    vec2f _prev_dir;
    float _prev_scalar_velocity;

public:
    virtual void user_any(std::any value) {
        _user_any = std::move(value);
    }

    [[nodiscard]]
    std::any& get_user_any() {
        return _user_any;
    }

    [[nodiscard]]
    const std::any& get_user_any() const {
        return _user_any;
    }

    virtual void record_dir_and_velocity() {
        _prev_dir = _dir;
        _prev_scalar_velocity = _scalar_velocity;
    }

    [[nodiscard]]
    const vec2f& prev_dir() const {
        return _prev_dir;
    }

    [[nodiscard]]
    float prev_scalar_velocity() const {
        return _prev_scalar_velocity;
    }

    virtual void user_data(u64 value) {
        _user_data = value;
    }

    [[nodiscard]]
    u64 get_user_data() const {
        return _user_data;
    }

    template <typename F>
    void set_collide_allower(F callback) {
        _collide_allower = std::function{std::move(callback)};
    }

    [[nodiscard]]
    const auto& collide_allower() const {
        return _collide_allower;
    }

    [[nodiscard]]
    bool allow_test_with(const physic_point* pp) {
        return (collide_allower() ? collide_allower()(pp) : true) &&
               (pp->collide_allower() ? pp->collide_allower()(this) : true);
    }

    virtual void position(const vec2f& value) {
        _position = value;
    }

    [[nodiscard]]
    const vec2f& get_position() const {
        return _position;
    }

    virtual void direction(const vec2f& direction) {
        auto d = normalize(direction);
        if (!std::isnan(d.x) && !std::isnan(d.y))
            _dir = d;
    }

    [[nodiscard]]
    const vec2f& get_direction() const {
        return _dir;
    }

    virtual void scalar_velocity(float value) {
        _scalar_velocity = value;
    }

    [[nodiscard]]
    float scalar_velocity() const {
        if (fixed())
            return 0.f;
        return _scalar_velocity;
    }

    virtual void velocity(const vec2f& value) {
        auto m = magnitude(value);
        _scalar_velocity = m;
        direction(value);
    }

    [[nodiscard]]
    vec2f get_velocity() const {
        auto vel = _dir * scalar_velocity();
        if (_lock_y)
            vel.y = 0.f;
        return vel;
    }

    /* The velocity as stored: with the vertical part get_velocity() hides while locked on a platform */
    [[nodiscard]]
    vec2f get_raw_velocity() const {
        return _dir * scalar_velocity();
    }

    virtual void impulse(const vec2f& value) {
        velocity(value / get_mass());
    }

    [[nodiscard]]
    vec2f impulse() const {
        return get_velocity() * get_mass();
    }

    virtual void scalar_impulse(float value) {
        scalar_velocity(value / get_mass());
    }

    [[nodiscard]]
    float scalar_impulse() const {
        return scalar_velocity() * get_mass();
    }

    virtual void mass(float value) {
        _mass = value;
    }

    [[nodiscard]]
    float get_mass() const {
        if (_fixed)
            return 999999.f;
        return _mass;
    }

    virtual void elasticity(float value) {
        _elasticity = value;
    }

    [[nodiscard]]
    float elasticity() const {
        return _elasticity;
    }

    virtual void fixed(bool value) {
        _fixed = value;
    }

    [[nodiscard]]
    bool fixed() const {
        return _fixed;
    }

    [[nodiscard]]
    bool is_group_operated() const {
        return _group.lock() != nullptr;
    }

    void delete_later(bool value = true) {
        _delete_later = value;
    }

    [[nodiscard]]
    bool ready_delete_later() const {
        return _delete_later;
    }

    [[nodiscard]]
    const sf::FloatRect& bb() const {
        return _bb;
    }

    virtual void enable_gravity(bool value = true) {
        _enable_gravity = value;
    }

    [[nodiscard]]
    bool is_gravity_enabled() const {
        return _enable_gravity;
    }

    void allow_platform(bool value) {
        _allow_platform = value;
    }

    [[nodiscard]]
    bool allow_platform() const {
        return _allow_platform;
    }

    [[nodiscard]]
    virtual bounding_box pos_bb() const {
        return sf::FloatRect(get_position().x, get_position().y, 0.f, 0.f);
    }

    [[nodiscard]]
    bool is_lock_y() const {
        return _lock_y;
    }

    void lock_y() {
        _lock_y = true;
    }

    void unlock_y() {
        _lock_y = false;
    }

    [[nodiscard]]
    virtual bool line_only() const {
        return false;
    }

    [[nodiscard]]
    float get_distance() const {
        return _distance;
    }
};
}
