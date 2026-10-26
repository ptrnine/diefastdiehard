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
 * jitter, which the interpolation delay has to cover, plus the longest
 * interval between the states (the next snapshot must be there: the
 * sender sends once per frame, which can run more than one tick).
 */
class server_clock {
public:
    static constexpr double window_s         = 2.0;
    /* Defaults of the interpolation delay: jitter + state interval + margin, at least min (see interp_settings) */
    static constexpr double min_interp_s       = 0.020;
    static constexpr double max_interp_s       = 0.3;
    static constexpr double interp_margin_s    = 0.010;
    static constexpr double default_interval_s = 0.016; /* the state interval before it is measured (a tick) */
    static constexpr double max_interval_s     = 0.1;   /* longer pauses (e.g. a paused game) are not a state rate */
    static constexpr double max_slew         = 0.1;  /* render clock speed deviation */
    static constexpr double snap_threshold_s = 0.5;

    void on_state(double local_s, double server_s) {
        /* Interval since the newest state so far (an older reordered one has none) */
        auto interval = 0.0;
        if (_newest_server_s && server_s > *_newest_server_s)
            interval = std::min(server_s - *_newest_server_s, max_interval_s);
        if (!_newest_server_s || server_s > *_newest_server_s)
            _newest_server_s = server_s;

        _samples.push_back({local_s, local_s - server_s, interval});
        while (!_samples.empty() && _samples.front().local_s < local_s - window_s)
            _samples.pop_front();

        _min_offset   = std::numeric_limits<double>::max();
        _max_offset   = std::numeric_limits<double>::lowest();
        _max_interval = 0.0;
        for (auto& smp : _samples) {
            _min_offset   = std::min(_min_offset, smp.offset_s);
            _max_offset   = std::max(_max_offset, smp.offset_s);
            _max_interval = std::max(_max_interval, smp.interval_s);
        }
    }

    [[nodiscard]]
    bool valid() const {
        return !_samples.empty();
    }

    /* Forget the samples (the offset jumped: e.g. the sender paused) */
    void reset() {
        _samples.clear();
        _newest_server_s.reset();
    }

    /** Spread of the offsets in the window: the latency jitter, s */
    [[nodiscard]]
    double jitter() const {
        return valid() ? _max_offset - _min_offset : 0.0;
    }

    /** Longest interval between the states in the window, s (a tick before there are two) */
    [[nodiscard]]
    double max_interval() const {
        return _max_interval > 0.0 ? _max_interval : default_interval_s;
    }

    /** Time the samples in the window cover, s */
    [[nodiscard]]
    double span() const {
        return valid() ? _samples.back().local_s - _samples.front().local_s : 0.0;
    }

    /** The interpolation delay: jitter + max_interval + margin_s, at least min_s */
    void interp_settings(double min_s, double margin_s) {
        _interp_min_s    = std::clamp(min_s, 0.0, max_interp_s);
        _interp_margin_s = std::clamp(margin_s, 0.0, max_interp_s);
    }

    [[nodiscard]]
    double interp_min() const {
        return _interp_min_s;
    }

    [[nodiscard]]
    double interp_margin() const {
        return _interp_margin_s;
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
        return std::clamp(jitter() + max_interval() + _interp_margin_s, _interp_min_s, max_interp_s);
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
        double interval_s; /* since the previous newest state */
    };

    std::deque<sample_t>  _samples;
    double                _min_offset   = 0.0;
    double                _max_offset   = 0.0;
    double                _max_interval = 0.0;
    std::optional<double> _newest_server_s;
    std::optional<double> _render_s;
    double                _render_local_s = 0.0;
    double                _interp_min_s    = min_interp_s;
    double                _interp_margin_s = interp_margin_s;
};

/**
 * Time-stamped snapshots of a remote entity for interpolation.
 * T must provide static T interpolate(const T& a, const T& b, float f, float span_s)
 * (span_s: the time between a and b).
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
        return T::interpolate(prev->value, next->value, f, float(next->time_s - prev->time_s));
    }

    [[nodiscard]]
    size_t size() const {
        return _snaps.size();
    }

    [[nodiscard]]
    std::optional<T> newest() const {
        if (_snaps.empty())
            return {};
        return _snaps.back().value;
    }

    /** The oldest snapshot matching pred */
    template <typename F>
    [[nodiscard]]
    std::optional<T> first_where(F&& pred) const {
        for (auto& snap : _snaps)
            if (pred(snap.value))
                return snap.value;
        return {};
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
        drop_before(ack + 1);
    }

    /** Forget the ticks before seq */
    void drop_before(u32 seq) {
        while (!_entries.empty() && _entries.front().seq < seq)
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
 * Remote players are shown from server snapshots, so hits predicted on the
 * client are shown as an offset on top of them, simulated with the game's
 * own physics (the caller's stepper and hit application): two ghosts start
 * from the player's shown state, one with the hits applied and one without,
 * and the offset is their difference, so it follows the level geometry.
 * Hits in a row form one episode (they accumulate on the same ghost: the
 * physics is not additive).
 *
 * The server applies a lag-compensated hit where the shooter saw it and
 * re-simulates the victim (game_state::apply_hit_at), so the snapshots
 * that report the hit already show the motion predicted here: confirmed
 * hits are simply dropped from the prediction (the caller smooths the
 * small difference). Hits the server did not register fade out. The
 * offset is not limited: it follows the game physics, and a hard hit must
 * not stop in mid-air until it is confirmed.
 *
 * State needs pos, vel (vec2f) and lock_y (bool)
 */
template <typename Ref, typename State>
class knockback_offset {
public:
    static constexpr float fade_s       = 0.15f;
    static constexpr float remove_below = 0.05f; /* fade factor */

    /* Ghosts with and without the hits, started from the same state */
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
    };

    struct hit_t {
        Ref   ref;
        vec2f impulse;
    };

    struct episode_t {
        std::vector<hit_t> hits;
        pair_t             ghosts;
        float              age  = 0.f;
        float              fade = 1.f; /* < 1: fading out */

        [[nodiscard]]
        bool open() const {
            return fade >= 1.f;
        }

        [[nodiscard]]
        vec2f offset() const {
            return ghosts.offset() * fade;
        }

        [[nodiscard]]
        vec2f velocity() const {
            return ghosts.velocity() * fade;
        }
    };

    /**
     * A hit predicted now on the player shown in state shown; apply_hit(State&,
     * impulse) applies it to a ghost. Joins the open episode, if any
     */
    template <typename FApply>
    void predict(const Ref& ref, const vec2f& impulse, const State& shown, FApply&& apply_hit) {
        if (_episodes.empty() || !_episodes.back().open())
            _episodes.push_back({{}, {shown, shown}});
        auto& ep = _episodes.back();
        apply_hit(ep.ghosts.with, impulse);
        ep.hits.push_back({ref, impulse});
    }

    /**
     * The snapshots (showing the player in state snapshot) report the hits
     * matches(ref): they are no longer predicted. An episode with hits left
     * restarts from the snapshot state with them
     */
    template <typename F, typename FApply>
    void confirm(F&& matches, const State& snapshot, FApply&& apply_hit) {
        for (auto& ep : _episodes) {
            if (!ep.open())
                continue;
            auto confirmed = std::erase_if(ep.hits, [&](const hit_t& h) { return matches(h.ref); });
            if (confirmed == 0 || ep.hits.empty())
                continue;
            ep.ghosts = {snapshot, snapshot};
            for (auto& h : ep.hits)
                apply_hit(ep.ghosts.with, h.impulse);
        }
        std::erase_if(_episodes, [](const episode_t& ep) { return ep.open() && ep.hits.empty(); });
    }

    /**
     * Advance by dt in fixed physics steps of timestep (step_state(State&,
     * float timestep) steps one ghost). Episodes older than
     * unconfirmed_timeout_s were not registered by the server: they fade out
     */
    template <typename F>
    void step(float dt, float timestep, float unconfirmed_timeout_s, F&& step_state) {
        _accumulated += dt;
        while (_accumulated >= timestep) {
            _accumulated -= timestep;
            for (auto& ep : _episodes) {
                step_state(ep.ghosts.with, timestep);
                step_state(ep.ghosts.without, timestep);
            }
        }

        for (auto& ep : _episodes) {
            ep.age += dt;
            if (!ep.open() || ep.age > unconfirmed_timeout_s)
                ep.fade *= std::exp(-dt / fade_s);
        }

        std::erase_if(_episodes, [](const episode_t& ep) { return ep.fade < remove_below; });
        if (_episodes.empty())
            _accumulated = 0.f;
    }

    [[nodiscard]]
    vec2f offset() const {
        return offset_unless([](const Ref&) { return false; });
    }

    /* The offset of the episodes none of whose hits reported(ref) (a state that already shows them) */
    template <typename F>
    [[nodiscard]]
    vec2f offset_unless(F&& reported) const {
        vec2f o = {0.f, 0.f};
        for (auto& ep : _episodes)
            if (std::ranges::none_of(ep.hits, [&](const hit_t& h) { return reported(h.ref); }))
                o += ep.offset() + ep.velocity() * _accumulated; /* in between the fixed steps */
        return o;
    }

    [[nodiscard]]
    vec2f velocity() const {
        vec2f v = {0.f, 0.f};
        for (auto& ep : _episodes)
            v += ep.velocity();
        return v;
    }

    /* Ground contact of the shown motion: of the open episode's ghost, if any */
    [[nodiscard]]
    std::optional<bool> grounded() const {
        if (!_episodes.empty() && _episodes.back().open())
            return _episodes.back().ghosts.with.lock_y;
        return {};
    }

    [[nodiscard]]
    bool empty() const {
        return _episodes.empty();
    }

    /* Every predicted hit (e.g. to resolve references) */
    template <typename F>
    void for_each_hit(F&& f) {
        for (auto& ep : _episodes)
            for (auto& h : ep.hits)
                f(h);
    }

private:
    std::vector<episode_t> _episodes;
    float                  _accumulated = 0.f;
};

} // namespace dfdh::net
