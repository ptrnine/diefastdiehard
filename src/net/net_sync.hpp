#pragma once

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>

#include "base/types.hpp"

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

} // namespace dfdh::net
