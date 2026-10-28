#pragma once
/*
 * Keyframe animation: time-keyed values with per-key interpolation (hold, linear or
 * bezier with in/out handles). Adopted from fever_dream's src/grx/keyframe_animation.hpp,
 * adapted to dfdh::vec2f.
 */

#include <vector>

#include "base/vec2.hpp"

namespace dfdh::efx
{

enum class interpolation_t { hold = 0, linear, bezier };
inline constexpr interpolation_t interpolation_not_set{-1};

namespace math
{
inline auto lerp(auto v0, auto v1, float t) {
    return v0 * (1.f - t) + v1 * t;
}

inline float inverse_lerp(float x1, float x2, float value) {
    return (value - x1) / (x2 - x1);
}

inline float bezier3(float p0, float p1, float p2, float p3, float t) {
    auto inv_t  = 1.f - t;
    auto inv_t2 = inv_t * inv_t;
    auto inv_t3 = inv_t2 * inv_t;
    auto t2     = t * t;
    auto t3     = t2 * t;
    return p0 * inv_t3 + p1 * inv_t2 * t * 3.f + p2 * 3.f * inv_t * t2 + p3 * t3;
}

/*
 * Maps t through a 1D bezier in x, then through it in y, and lerps v0 -> v1 by y.
 */
inline auto cubic_bezier(auto v0, auto v1, float p1_x, float p1_y, float p2_x, float p2_y, float t) {
    /* Only the y mapping is applied (the x mapping is unused, as in the original). */
    (void)p1_x;
    (void)p2_x;
    auto y = bezier3(0.f, p1_y, p2_y, 1.f, t);
    return lerp(v0, v1, y);
}
} // namespace math

template <typename T>
struct anim_key {
    T value;
    float time;
    interpolation_t interpolation;
    vec2f in;
    vec2f out;
};

template <typename T>
class anim_key_sequence {
public:
    void push(anim_key<T> key) {
        keys.push_back(std::move(key));
    }

    void push_hold(const T& value, float time) {
        push(anim_key<T>(value, time, interpolation_t::hold));
    }

    void push_linear(const T& value, float time) {
        push(anim_key<T>{value, time, interpolation_t::linear});
    }

    void push_linear_to_bezier(const T& value, float time, const vec2f& out) {
        push(anim_key<T>{value, time, interpolation_t::bezier, {0, 0}, out});
    }

    void push_bezier_to_linear(const T& value, float time, const vec2f& in) {
        push(anim_key<T>{value, time, interpolation_t::bezier, in, {1, 1}});
    }

    void push_bezier(const T& value, float time, const vec2f& in = {0, 0}, const vec2f& out = {1, 1}) {
        push(anim_key<T>{value, time, interpolation_t::bezier, in, out});
    }

    void normalize_time() {
        if (keys.empty())
            return;

        auto& max = keys.back().time;
        for (auto& key : keys)
            key.time = key.time / max;
    }

    T lookup(float time) const {
        if (keys.empty())
            return {};

        if (keys.size() == 1)
            return keys.back().value;

        if (time <= keys.front().time)
            return keys.front().value;

        auto i = keys.begin();
        while (i != keys.end() && i->time < time)
            ++i;

        auto k2 = i;
        if (k2 == keys.end())
            return keys.back().value;

        anim_key<T> k1;
        if (k2 == keys.begin())
            k1 = anim_key<T>{{}, 0, interpolation_t::linear, {}, {}};
        else
            k1 = *(k2 - 1);

        return interpolate(k1, *k2, time);
    }

private:
    static T interpolate(const anim_key<T>& k1, const anim_key<T>& k2, float time) {
        if (k2.interpolation == interpolation_t::hold)
            return k1.value;

        auto t = math::inverse_lerp(k1.time, k2.time, time);

        unsigned f = (unsigned(k1.interpolation == interpolation_t::bezier) << 1) |
                     unsigned(k2.interpolation == interpolation_t::bezier);

        switch (f) {
        /* k1 and k2 are linear */
        case 0: return math::lerp(k1.value, k2.value, t);
        /* k1 is linear, k2 is bezier */
        case 1: return math::cubic_bezier(k1.value, k2.value, 0.f, 0.f, k2.in.x, k2.in.y, t);
        /* k1 is bezier, k2 is linear */
        case 2: return math::cubic_bezier(k1.value, k2.value, k1.out.x, k1.out.y, 1.f, 1.f, t);
        /* k1 and k2 are bezier */
        case 3: return math::cubic_bezier(k1.value, k2.value, k1.out.x, k1.out.y, k2.in.x, k2.in.y, t);
        }

        return {};
    }

private:
    std::vector<anim_key<T>> keys;
};
} // namespace dfdh::efx
