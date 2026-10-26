#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <vector>

#include "base/types.hpp"
#include "base/vec2.hpp"
#include "base/vec_math.hpp"

namespace dfdh::net {

/**
 * Client-side estimate of the server clock.
 *
 * Every state datagram gives an offset sample local_time - server_time
 * (= clock difference + one-way latency). The smallest offset in a sliding
 * window is the least delayed datagram, so local - min_offset estimates the
 * server time; the spread of the offsets in the window is the latency
 * jitter, which the interpolation delay has to cover.
 */
class server_clock {
public:
    static constexpr double window_s         = 2.0;
    static constexpr double min_interp_s     = 0.033;
    static constexpr double max_interp_s     = 0.3;
    static constexpr double interp_margin_s  = 0.016 + 0.010; /* one broadcast frame + margin */
    static constexpr double max_slew         = 0.1;  /* render clock speed deviation */
    static constexpr double snap_threshold_s = 0.5;

    void on_state(double local_s, double server_s) {
        _samples.push_back({local_s, local_s - server_s});
        while (!_samples.empty() && _samples.front().local_s < local_s - window_s)
            _samples.pop_front();

        _min_offset = std::numeric_limits<double>::max();
        _max_offset = std::numeric_limits<double>::lowest();
        for (auto& smp : _samples) {
            _min_offset = std::min(_min_offset, smp.offset_s);
            _max_offset = std::max(_max_offset, smp.offset_s);
        }
    }

    [[nodiscard]]
    bool valid() const {
        return !_samples.empty();
    }

    [[nodiscard]]
    double server_now(double local_s) const {
        return local_s - _min_offset;
    }

    /**
     * How much longer than the fastest recent state the state sent at
     * server_s took to arrive (its share of the latency jitter)
     */
    [[nodiscard]]
    double excess_delay(double local_s, double server_s) const {
        return std::max(local_s - server_s - _min_offset, 0.0);
    }

    /** Remote players are shown this far in the past */
    [[nodiscard]]
    double interp_delay() const {
        if (!valid())
            return min_interp_s;
        return std::clamp(_max_offset - _min_offset + interp_margin_s, min_interp_s, max_interp_s);
    }

    /**
     * Server time to render remote players at. Advances monotonically with the
     * local clock, slewing towards server_now - interp_delay at most by max_slew
     * (snaps on large differences, e.g. at start)
     */
    double render_time(double local_s) {
        auto target = server_now(local_s) - interp_delay();

        if (!_render_s) {
            _render_s       = target;
            _render_local_s = local_s;
            return target;
        }

        auto dt   = std::max(local_s - _render_local_s, 0.0);
        auto next = *_render_s + dt;
        auto diff = target - next;

        if (std::abs(diff) > snap_threshold_s)
            next = target;
        else
            next += std::clamp(diff, -dt * max_slew, dt * max_slew);

        _render_s       = std::max(*_render_s, next);
        _render_local_s = local_s;
        return *_render_s;
    }

private:
    struct sample_t {
        double local_s;
        double offset_s;
    };

    std::deque<sample_t>  _samples;
    double                _min_offset = 0.0;
    double                _max_offset = 0.0;
    std::optional<double> _render_s;
    double                _render_local_s = 0.0;
};

/**
 * Time-stamped snapshots of a remote entity for interpolation.
 * T must provide static T interpolate(const T& a, const T& b, float f).
 * Samples may arrive out of order; sampling outside of the stored range
 * clamps to the oldest/newest snapshot
 */
template <typename T>
class snapshot_buffer {
public:
    static constexpr double keep_s = 1.0;

    void push(double time_s, const T& value) {
        auto it = std::lower_bound(_snaps.begin(), _snaps.end(), time_s,
                                   [](const snap_t& s, double t) { return s.time_s < t; });
        if (it != _snaps.end() && !(time_s < it->time_s))
            return; /* duplicate */
        _snaps.insert(it, snap_t{time_s, value});
    }

    [[nodiscard]]
    std::optional<T> sample(double time_s) {
        if (_snaps.empty())
            return {};

        /* Drop what is too old, keeping one snapshot before time_s to interpolate from */
        while (_snaps.size() > 2 && _snaps[1].time_s < time_s - keep_s)
            _snaps.pop_front();

        if (time_s <= _snaps.front().time_s)
            return _snaps.front().value;
        if (time_s >= _snaps.back().time_s)
            return _snaps.back().value;

        auto next = std::lower_bound(_snaps.begin(), _snaps.end(), time_s,
                                     [](const snap_t& s, double t) { return s.time_s < t; });
        auto prev = std::prev(next);
        auto f    = float((time_s - prev->time_s) / (next->time_s - prev->time_s));
        return T::interpolate(prev->value, next->value, f);
    }

    [[nodiscard]]
    size_t size() const {
        return _snaps.size();
    }

    /**
     * The newest snapshot before the first one matching pred (e.g. the state
     * right before an event the snapshots report), with its time
     */
    template <typename F>
    [[nodiscard]]
    std::optional<std::pair<double, T>> last_before(F&& pred) const {
        for (size_t i = 1; i < _snaps.size(); ++i)
            if (pred(_snaps[i].value))
                return std::pair{_snaps[i - 1].time_s, _snaps[i - 1].value};
        return {};
    }

private:
    struct snap_t {
        double time_s;
        T      value;
    };

    std::deque<snap_t> _snaps;
};

/**
 * Client-side prediction history: the input applied at every predicted tick
 * and the predicted state after it, kept until the server acknowledges the
 * tick
 */
template <typename Input, typename State, typename Extra>
class input_history {
public:
    static constexpr size_t max_size = 512;

    struct entry_t {
        u32   seq;
        Input input;
        Extra extra;
        float timestep;
        State state = {};
    };

    void push(const entry_t& entry) {
        _entries.push_back(entry);
        if (_entries.size() > max_size)
            _entries.pop_front();
    }

    [[nodiscard]]
    entry_t* find(u32 seq) {
        if (_entries.empty() || seq < _entries.front().seq || seq > _entries.back().seq)
            return nullptr;
        auto& e = _entries[seq - _entries.front().seq];
        return e.seq == seq ? &e : nullptr;
    }

    /** Forget the acknowledged ticks (seq <= ack) */
    void drop_up_to(u32 ack) {
        while (!_entries.empty() && _entries.front().seq <= ack)
            _entries.pop_front();
    }

    [[nodiscard]]
    entry_t* back() {
        return _entries.empty() ? nullptr : &_entries.back();
    }

    auto begin() {
        return _entries.begin();
    }

    auto end() {
        return _entries.end();
    }

    [[nodiscard]]
    size_t size() const {
        return _entries.size();
    }

    void clear() {
        _entries.clear();
    }

private:
    std::deque<entry_t> _entries;
};

/**
 * Visual knockback of a remote player, predicted on the client.
 *
 * Remote players are shown from server snapshots, so a hit predicted on the
 * client is shown as an offset on top of them. The offset is simulated with
 * the game's own physics (the caller's stepper): two ghosts start from the
 * player's state at the hit, one with the hit applied and one without, and
 * the offset is their difference, so it follows the level geometry (slides
 * off platform edges, falls, lands). When the snapshots start to show the
 * real hit (confirm), a second pair starts from the snapshot state and is
 * subtracted: the sum converges while the authoritative motion takes over,
 * without pushing twice. An unconfirmed hit (the server did not register it)
 * fades out.
 *
 * State needs pos, vel (vec2f) and lock_y (bool)
 */
template <typename Ref, typename State>
class knockback_offset {
public:
    static constexpr float fade_s         = 0.1f;
    static constexpr float max_age_s      = 4.f;  /* e.g. knocked out of the level: never settles */
    static constexpr float settled_vel    = 1.f;
    static constexpr float remove_below   = 0.1f;

    /* Ghosts with and without the hit, started from the same state */
    struct pair_t {
        State with;
        State without;

        [[nodiscard]]
        vec2f offset() const {
            return with.pos - without.pos;
        }

        [[nodiscard]]
        vec2f velocity() const {
            return with.vel - without.vel;
        }

        [[nodiscard]]
        bool settled() const {
            return magnitude(with.vel) < settled_vel && magnitude(without.vel) < settled_vel && with.lock_y &&
                   without.lock_y;
        }
    };

    struct hit_t {
        Ref                   ref;
        vec2f                 impulse;
        pair_t                predicted;
        std::optional<pair_t> authoritative;
        float                 age    = 0.f;
        float                 fade   = 1.f; /* < 1: fading out */
        bool                  missed = false;

        [[nodiscard]]
        vec2f offset() const {
            auto o = predicted.offset();
            if (authoritative)
                o -= authoritative->offset();
            return o * fade;
        }

        [[nodiscard]]
        vec2f velocity() const {
            auto v = predicted.velocity();
            if (authoritative)
                v -= authoritative->velocity();
            return v * fade;
        }

        [[nodiscard]]
        bool active() const {
            return !missed && fade >= 1.f;
        }
    };

    /* A hit predicted now: ghosts started from the player's state with (with) and without (without) the hit */
    void predict(const Ref& ref, const vec2f& impulse, const State& without, const State& with) {
        _hits.push_back({ref, impulse, pair_t{with, without}, {}});
    }

    /**
     * The snapshots show the hit (matches(ref)): start the opposite pair,
     * make_pair(impulse) -> pair_t from the snapshot state
     */
    template <typename F, typename FMakePair>
    void confirm(F&& matches, FMakePair&& make_pair) {
        for (auto& h : _hits)
            if (!h.authoritative && !h.missed && matches(h.ref))
                h.authoritative = make_pair(h.impulse);
    }

    /* Hits not confirmed for this long were not registered by the server */
    void miss_older_than(float age_s) {
        for (auto& h : _hits)
            if (!h.authoritative && h.age > age_s)
                h.missed = true;
    }

    /**
     * Advance by dt in fixed physics steps of timestep;
     * step_state(State&, float timestep) steps one ghost
     */
    template <typename F>
    void step(float dt, float timestep, F&& step_state) {
        _accumulated += dt;
        while (_accumulated >= timestep) {
            _accumulated -= timestep;
            for (auto& h : _hits) {
                if (!h.active())
                    continue;
                step_state(h.predicted.with, timestep);
                step_state(h.predicted.without, timestep);
                if (h.authoritative) {
                    step_state(h.authoritative->with, timestep);
                    step_state(h.authoritative->without, timestep);
                }
            }
        }

        for (auto& h : _hits) {
            h.age += dt;
            /* Missed, both motions are over (fade what is left), or too long */
            bool done = h.missed || h.age > max_age_s ||
                        (h.authoritative && h.predicted.settled() && h.authoritative->settled());
            if (done)
                h.fade *= std::exp(-dt / fade_s);
        }

        std::erase_if(_hits, [](const hit_t& h) {
            return !h.active() && magnitude(h.offset()) < remove_below && magnitude(h.velocity()) < remove_below;
        });
        if (_hits.empty())
            _accumulated = 0.f;
    }

    [[nodiscard]]
    vec2f offset() const {
        vec2f o = {0.f, 0.f};
        for (auto& h : _hits)
            o += h.offset();
        return o;
    }

    [[nodiscard]]
    vec2f velocity() const {
        vec2f v = {0.f, 0.f};
        for (auto& h : _hits)
            v += h.velocity();
        return v;
    }

    /* Ground contact of the shown motion: of the newest active hit's ghost, if any */
    [[nodiscard]]
    std::optional<bool> grounded() const {
        for (auto it = _hits.rbegin(); it != _hits.rend(); ++it)
            if (it->active())
                return it->predicted.with.lock_y;
        return {};
    }

    [[nodiscard]]
    bool empty() const {
        return _hits.empty();
    }

    auto& hits() {
        return _hits;
    }

private:
    std::vector<hit_t> _hits;
    float              _accumulated = 0.f;
};

} // namespace dfdh::net
