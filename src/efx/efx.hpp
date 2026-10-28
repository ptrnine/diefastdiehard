#pragma once
/*
 * Effects: a set of drawables plus named handlers (animations, gravity, ...) applied
 * to them while the effect plays, with a duration. Adopted from fever_dream's
 * src/grx/efx.hpp: efx_instance/efx_mgr are adapted to draw directly to a
 * sf::RenderTarget instead of fever_dream's scene batches.
 */

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include <SFML/Graphics/RenderStates.hpp>
#include <SFML/Graphics/RenderTarget.hpp>
#include <SFML/Graphics/Transform.hpp>
#include <SFML/System/Angle.hpp>

#include "base/log.hpp"
#include "base/vec_math.hpp"
#include "keyframe_animation.hpp"
#include "sfml_types.hpp"

namespace dfdh::efx
{

struct efx_state {
    const std::vector<drawable_t>* drawables;
    float timestep;
    float timestep_coef;
    float time_elapsed;
    float time_elapsed_coef;
    u32 idx;
};

class efx {
public:
    friend class efx_instance;

    class handler_t {
    public:
        using index_t = u32;

        template <typename F>
        handler_t(F&& function): handler(std::forward<F>(function)) {}

        void set_affected_indices(std::initializer_list<index_t> indices) {
            affects_all = false;
            affected_indices.assign(indices);
        }

        void set_affected_indices(std::vector<index_t> indices) {
            affects_all = false;
            affected_indices = std::move(indices);
        }

        void set_affects_all(bool value = true) {
            affects_all = value;
        }

        const auto& get_affected_indices() const {
            return affected_indices;
        }

        void operator()(std::vector<drawable_t>& drawables, efx_state state) const {
            if (affects_all) {
                for (size_t idx = 0; idx < drawables.size(); ++idx) {
                    state.idx = u32(idx);
                    handler(drawables[idx], state);
                }
            }
            else {
                for (auto idx : affected_indices) {
                    state.idx = idx;
                    handler(drawables[idx], state);
                }
            }
        }

    private:
        std::vector<index_t>                               affected_indices;
        std::function<void(drawable_t&, const efx_state&)> handler;
        bool                                               affects_all = true;
    };

    template <typename F>
    handler_t& add_handler(const std::string& name, F&& function) {
        return handlers
            .insert_or_assign(name,
                              [f = std::forward<F>(function)](drawable_t& drawable, const efx_state& state) mutable {
                                  downcast(drawable, std::forward<F>(f), state);
                              })
            .first->second;
    }

    handler_t* get_handler(const std::string& name) {
        auto bucket = handlers.find(name);
        if (bucket == handlers.end())
            return nullptr;
        return &bucket->second;
    }

    const handler_t* get_handler(const std::string& name) const {
        auto bucket = handlers.find(name);
        if (bucket == handlers.end())
            return nullptr;
        return &bucket->second;
    }

    auto& get_elements() {
        return elements;
    }

    const auto& get_elements() const {
        return elements;
    }

    void set_duration(float value) {
        duration = value;
    }

    float get_duration() const {
        return duration;
    }

    template <typename T>
    T& create_element(T&& element) {
        elements.emplace_back(std::forward<T>(element));
        return std::get<std::decay_t<T>>(elements.back());
    }

    drawable_t& push_element(drawable_t element) {
        elements.push_back(std::move(element));
        return elements.back();
    }

private:
    std::vector<drawable_t>          elements;
    std::map<std::string, handler_t> handlers;
    float                            duration;
};

static inline constexpr float duration_endless = std::numeric_limits<float>::infinity();

/*
 * A running instance of an effect: its own copy of the drawables, the effect's
 * handlers applied to them every update, and a transform (move/scale) for the whole
 * instance.
 */
class efx_instance {
public:
    explicit efx_instance(efx& iefx): e(&iefx), elements(iefx.elements), duration(e->get_duration()) {
        for (auto&& [_, handler] : e->handlers) handlers.push_back(handler);
    }

    void update(float timestep) {
        for (auto&& handler : handlers)
            handler(elements,
                    {
                        .drawables             = &elements,
                        .timestep              = timestep,
                        .timestep_coef         = timestep / duration,
                        .time_elapsed          = time_elapsed,
                        .time_elapsed_coef     = time_elapsed / duration,
                        .idx                   = 0,
                    });
        time_elapsed += timestep;
    }

    bool timeout() const {
        return time_elapsed >= duration;
    }

    void move(const vec2f& movement) {
        transform.translate(movement);
    }

    void scale(const vec2f& scale) {
        transform.scale(scale, calc_center());
    }

    void draw(sf::RenderTarget& target, const sf::RenderStates& render_states = sf::RenderStates::Default) const {
        if (elements.empty())
            return;

        auto rs = render_states;
        rs.transform.combine(transform);
        for (const auto& element : elements)
            downcast(element, [&](const sf::Drawable& drawable) { target.draw(drawable, rs); });
    }

    auto& get_elements() {
        return elements;
    }

    const auto& get_elements() const {
        return elements;
    }

private:
    vec2f calc_center() const {
        vec2f center{0, 0};
        for (const auto& element : elements)
            center +=
                std::visit([](const auto& obj) { return vec2f(obj.getPosition()); }, element) *
                (1.f / float(elements.size()));
        return center;
    }

private:
    std::vector<efx::handler_t>   handlers;
    efx*                          e;
    std::vector<drawable_t>       elements;
    sf::Transform                 transform    = sf::Transform::Identity;
    float                         duration;
    float                         time_elapsed = 0.f;
};

/*
 * Manages a set of named effects and their running instances.
 */
class efx_mgr {
public:
    void add_effect(const std::string& name, efx effect) {
        effects.insert_or_assign(name, std::move(effect));
    }

    bool play(const std::string& name,
              const vec2f&       position = {0, 0},
              const vec2f&       scale    = {1.f, 1.f}) {
        auto found = effects.find(name);
        if (found == effects.end()) {
            glog().error("effect '{}' not exists", name);
            return false;
        }

        running_effects.push_back(efx_instance(found->second));
        auto& instance = running_effects.back();
        instance.move(position);
        instance.scale(scale);

        return true;
    }

    void update(float timestep) {
        for (auto i = running_effects.begin(); i != running_effects.end();) {
            if (i->timeout())
                running_effects.erase(i++);
            else
                i++->update(timestep);
        }
    }

    void draw(sf::RenderTarget& target, const sf::RenderStates& render_states = sf::RenderStates::Default) const {
        for (const auto& effect : running_effects)
            effect.draw(target, render_states);
    }

    void clear() {
        effects.clear();
        running_effects.clear();
    }

private:
    std::map<std::string, efx> effects;
    std::list<efx_instance>    running_effects;
};

namespace efx_handlers
{
    inline auto position(const anim_key_sequence<vec2f>& keys) {
        return [keys = keys](sf::Transformable& obj, const efx_state& state) {
            obj.setPosition(keys.lookup(state.time_elapsed_coef));
        };
    }

    /*
     * Scales the drawable around the given anchor: a 0-1 point of the drawable's
     * local bounds (0,0 = top-left, 0.5/0.5 = center). The drawable's position is
     * adjusted so that the anchor point stays fixed in the world space.
     */
    inline auto scale(const anim_key_sequence<vec2f>& keys, const vec2f& anchor = {0.5f, 0.5f}) {
        return [keys = keys, anchor = anchor](auto& obj, const efx_state& state) {
            auto scale_new = keys.lookup(state.time_elapsed_coef);
            auto scale_old = obj.getScale();

            /* The drawable's local bounds (its geometry space, before the origin shift). */
            vec2f bounds_min{0, 0};
            vec2f bounds_size{};

            /*
             * Circle and rect geometry starts at the drawable's position (top-left
             * corner), not centered, so bounds_min stays {0, 0}.
             */
            if constexpr (std::is_same_v<std::decay_t<decltype(obj)>, sf::CircleShape>) {
                bounds_size = {2.f * obj.getRadius(), 2.f * obj.getRadius()};
            }
            else if constexpr (std::is_same_v<std::decay_t<decltype(obj)>, sf::RectangleShape>) {
                bounds_size = vec2f(obj.getSize());
            }
            else if constexpr (std::is_same_v<std::decay_t<decltype(obj)>, sf::ConvexShape>) {
                if (auto count = obj.getPointCount(); count > 0) {
                    auto min = obj.getPoint(0);
                    auto max = min;
                    for (std::size_t i = 1; i < count; ++i) {
                        auto point = obj.getPoint(i);
                        min.x       = std::min(min.x, point.x);
                        min.y       = std::min(min.y, point.y);
                        max.x       = std::max(max.x, point.x);
                        max.y       = std::max(max.y, point.y);
                    }
                    bounds_min  = vec2f(min);
                    bounds_size = vec2f(max) - bounds_min;
                }
            }
            else {
                /* sprite */
                auto rect = obj.getTextureRect();
                bounds_min  = {float(rect.position.x), float(rect.position.y)};
                bounds_size = {float(rect.size.x), float(rect.size.y)};
            }

            auto anchor_local = bounds_min + vec2f{anchor.x * bounds_size.x, anchor.y * bounds_size.y} - vec2f(obj.getOrigin());
            auto delta         = vec2f{
                (scale_old.x - scale_new.x) * anchor_local.x,
                (scale_old.y - scale_new.y) * anchor_local.y
            };

            obj.setPosition(obj.getPosition() + delta);
            obj.setScale(scale_new);
        };
    }

    inline auto rotation(const anim_key_sequence<float>& keys) {
        return [keys = keys](sf::Transformable& obj, const efx_state& state) {
            obj.setRotation(sf::degrees(keys.lookup(state.time_elapsed_coef)));
        };
    }

    /*
     * Animates the drawable's color alpha: 1.0 is fully visible, 0.0 is invisible.
     * For shapes it keeps the current fill color and replaces its alpha;
     * for sprites it replaces the color multiplier's alpha.
     */
    inline auto opacity(const anim_key_sequence<float>& keys) {
        return [keys = keys](auto& obj, const efx_state& state) {
            auto alpha = static_cast<unsigned char>(keys.lookup(state.time_elapsed_coef) * 255.f + 0.5f);

            if constexpr (std::is_base_of_v<sf::Shape, std::decay_t<decltype(obj)>>) {
                auto color = obj.getFillColor();
                color.a = alpha;
                obj.setFillColor(color);
            }
            else {
                auto color = obj.getColor();
                color.a = alpha;
                obj.setColor(color);
            }
        };
    }

    /*
     * Each drawable attracts the others (masses/velocities are per index, 1.0/{0,0}
     * by default), accumulating velocity and moving the drawable with it.
     */
    inline auto gravity(const std::vector<float>& masses = {}, const std::vector<vec2f>& velocities = {}) {
        return [masses = masses, velocities = velocities](sf::Transformable& obj, const efx_state& state) mutable {
            auto idx = state.idx;

            if (!state.drawables)
                return;

            auto& bodies = *state.drawables;

            if (bodies.size() > masses.size())
                masses.resize(bodies.size(), 1.f);
            if (bodies.size() > velocities.size())
                velocities.resize(bodies.size(), {0, 0});

            vec2f accel{0, 0};

            for (size_t i = 0; i < bodies.size(); ++i) {
                if (i == idx)
                    continue;
                auto mass = masses[i];
                auto pos  = std::visit([](const auto& obj) { return obj.getPosition(); }, bodies[i]);
                auto dir  = normalize(vec2f(pos - obj.getPosition()));
                accel += dir * mass;
            }

            auto& velocity = velocities[idx];
            velocity += accel * state.timestep;
            obj.move(velocity * state.timestep);
        };
    }

    inline auto velocity_random(
        float angle_min_deg, float angle_max_deg, float speed_min, float speed_max, const vec2f& acceleration = {}) {
        return [angle_min_deg,
                angle_max_deg,
                speed_min,
                speed_max,
                velocities = std::optional<std::vector<vec2f>>{},
                acceleration](sf::Transformable& obj, const efx_state& state) mutable {
            if (!state.drawables)
                return;

            if (!velocities) {
                auto& bodies = *state.drawables;
                velocities.emplace(bodies.size());
                for (auto& v : *velocities) {
                    auto theta = sf::radians(rand_float(angle_min_deg, angle_max_deg));
                    auto speed = rand_float(speed_min, speed_max);
                    v          = {std::cos(theta.asRadians()) * speed, std::sin(theta.asRadians()) * speed};
                }
            }

            auto& v = (*velocities)[state.idx];
            v += acceleration * state.timestep;
            obj.move(v * state.timestep);
        };
    }

    /*
     * Shifts each drawable's position by a random offset within the given
     * ranges. Applied once, on the instance's first update (i.e. on creation).
     */
    inline auto position_random(const std::array<float, 2>& x_range, const std::array<float, 2>& y_range) {
        return [x_range, y_range,
                moved = std::optional<std::vector<bool>>{}](sf::Transformable& obj, const efx_state& state) mutable {
            if (!state.drawables)
                return;

            auto& bodies = *state.drawables;

            if (!moved)
                moved.emplace(bodies.size(), false);

            if ((*moved)[state.idx])
                return;

            (*moved)[state.idx] = true;
            obj.move({rand_float(x_range[0], x_range[1]), rand_float(y_range[0], y_range[1])});
        };
    }
} // namespace efx_handlers
} // namespace dfdh::efx

namespace dfdh {
class efx_mgr_singleton {
public:
    efx_mgr_singleton(const efx_mgr_singleton&)            = delete;
    efx_mgr_singleton& operator=(const efx_mgr_singleton&) = delete;
    efx_mgr_singleton(efx_mgr_singleton&&)                 = delete;
    efx_mgr_singleton& operator=(efx_mgr_singleton&&)      = delete;

    static efx::efx_mgr& instance() {
        static efx_mgr_singleton inst;
        return inst._mgr;
    }

private:
    efx_mgr_singleton()  = default;
    ~efx_mgr_singleton() = default;

    efx::efx_mgr _mgr;
};

static inline efx::efx_mgr& efxmgr() {
    return efx_mgr_singleton::instance();
}
} // namespace dfdh
