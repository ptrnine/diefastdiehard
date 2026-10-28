#pragma once
/*
 * Effects: a set of drawables plus named handlers (animations, gravity, ...) applied
 * to them while the effect plays, with a duration. Adopted from fever_dream's
 * src/grx/efx.hpp: efx_instance/efx_mgr are adapted to draw directly to a
 * sf::RenderTarget instead of fever_dream's scene batches.
 */

#include <functional>
#include <limits>
#include <list>
#include <map>
#include <string>
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

    inline auto scale(const anim_key_sequence<vec2f>& keys) {
        return [keys = keys](sf::Transformable& obj, const efx_state& state) {
            obj.setScale(keys.lookup(state.time_elapsed_coef));
        };
    }

    inline auto rotation(const anim_key_sequence<float>& keys) {
        return [keys = keys](sf::Transformable& obj, const efx_state& state) {
            obj.setRotation(sf::degrees(keys.lookup(state.time_elapsed_coef)));
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
