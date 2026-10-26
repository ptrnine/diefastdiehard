#pragma once

#include <chrono>

namespace dfdh {

/**
 * The clock of the game time (simulation ticks, network timing and
 * latency simulation): std::chrono::steady_clock, or a manually advanced
 * one (tests run the real-time code paths without waiting). Same
 * time_point type as steady_clock
 */
struct game_clock {
    using base       = std::chrono::steady_clock;
    using rep        = base::rep;
    using period     = base::period;
    using duration   = base::duration;
    using time_point = base::time_point;
    static constexpr bool is_steady = true;

    static time_point now() {
        return _manual ? _manual_now : base::now();
    }

    /* Manual mode: the time only moves with advance() (starts from the current time) */
    static void manual(bool value) {
        if (value && !_manual)
            _manual_now = base::now();
        _manual = value;
    }

    [[nodiscard]]
    static bool manual() {
        return _manual;
    }

    static void advance(duration d) {
        _manual_now += d;
    }

private:
    static inline bool       _manual     = false;
    static inline time_point _manual_now = {};
};

/* Manual game_clock for the lifetime of the guard */
struct game_clock_manual_guard {
    game_clock_manual_guard() {
        game_clock::manual(true);
    }
    ~game_clock_manual_guard() {
        game_clock::manual(false);
    }
    game_clock_manual_guard(const game_clock_manual_guard&)            = delete;
    game_clock_manual_guard& operator=(const game_clock_manual_guard&) = delete;
};

/* Elapsed game time since the last restart */
class game_timer {
public:
    void restart() {
        _start = game_clock::now();
    }

    [[nodiscard]]
    float elapsed() const {
        return std::chrono::duration<float>(game_clock::now() - _start).count();
    }

private:
    game_clock::time_point _start = game_clock::now();
};

} // namespace dfdh
