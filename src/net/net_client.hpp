#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <set>
#include <string>

#include <SFML/Network/IpAddress.hpp>

#include "base/types.hpp"
#include "base/log.hpp"
#include "base/signals.hpp"
#include "net/net_protocol.hpp"
#include "net/net_udp.hpp"
#include "net/net_sync.hpp"
#include "game_state.hpp"

namespace dfdh::net {

/* Interpolated state of a remote player */
struct remote_player_snapshot {
    player::physic_state ph;
    bool                 on_left  = false;
    bool                 shooting = false;
    bool                 throwing = false;
    std::vector<u32>     hits; // bullets that hit the player in the last second
    u16                  teleports = 0;

    static remote_player_snapshot
    interpolate(const remote_player_snapshot& a, const remote_player_snapshot& b, float f, float span_s) {
        auto res = f < 1.f ? a : b;
        /* Teleported in between: no path between the positions, jump at the next snapshot */
        if (a.teleports != b.teleports)
            return res;

        /*
         * b reports hits a does not: the server applied them where the shooter
         * saw them (retroactive hits), so the path up to b was rewritten and
         * a is a stale position. Moving from a to b would cross the whole
         * knockback within one snapshot interval: take b, moved back along
         * its velocity instead (with its hits, which confirm the predictions)
         */
        auto new_hit = std::ranges::any_of(b.hits, [&](u32 id) { return std::ranges::find(a.hits, id) == a.hits.end(); });
        if (new_hit) {
            res     = b;
            auto vel = b.ph.lock_y ? vec2f{b.ph.vel.x, 0.f} : b.ph.vel;
            res.ph.pos -= vel * ((1.f - f) * span_s);
            return res;
        }

        res.ph.pos = lerp(a.ph.pos, b.ph.pos, f);
        res.ph.vel = lerp(a.ph.vel, b.ph.vel, f);
        return res;
    }
};

/* Wrapping counter a is ahead of b */
inline bool counter_newer(u16 a, u16 b) {
    auto diff = u16(a - b);
    return diff != 0 && diff < 0x8000;
}

inline player::physic_state to_physic_state(const state_player& sp) {
    return {{sp.pos_x, sp.pos_y},
            {sp.vel_x, sp.vel_y},
            (sp.flags & state_player::flag_lock_y) != 0,
            sp.jumps_left,
            unpack_unit(sp.accel_lf),
            unpack_unit(sp.accel_rf)};
}

inline game_mode to_game_mode(const game_mode_info& gmi) {
    return {.section           = gmi.section,
            .bullet_gravity    = gmi.bullet_gravity,
            .crate_drop_period = gmi.crate_drop_period,
            .magazines_count   = gmi.magazines_count,
            .score_system      = gmi.score_system == u8(game_mode::score_system_t::lives) ? game_mode::score_system_t::lives
                                                                                         : game_mode::score_system_t::kills,
            .start_lives       = gmi.start_lives};
}

inline weapon_instance::sync_state to_weapon_state(const weapon_sync& ws) {
    return {ws.ammo,
            ws.anim,
            ws.anim_time,
            ws.since_shot,
            (ws.flags & weapon_sync::flag_reloading) != 0,
            (ws.flags & weapon_sync::flag_trigger) != 0,
            ws.shot_index,
            ws.seed_base,
            ws.reserve == weapon_sync::unlimited_reserve ? weapon_instance::unlimited_ammo : u32(ws.reserve)};
}

/**
 * Multiplayer client.
 *
 * The local player is predicted: the input is applied locally every tick and
 * kept in a history until the server acknowledges it. When the server state
 * for an acknowledged tick differs from the prediction, the local player is
 * reset to the server state and the unacknowledged inputs are re-simulated
 * (rollback). Remote players are interpolated between server snapshots,
 * slightly in the past (server_clock::interp_delay).
 *
 * Sends HELLO on creation, call update(game_state) once per game frame
 * (from the main thread).
 */
class net_client {
public:
    net_client(const sf::IpAddress& server_ip,
               u16                  server_port,
               const player_name_t& name,
               const std::string&   pistol,
               u32                  face_id,
               u32                  body_id,
               const sf::Color&     body_color,
               const sf::Color&     tracer_color):
        _server_ip(server_ip), _server_port(server_port), _name(name) {
        if (!_sock.connect(server_ip, server_port)) {
            glog().error("net client: can't connect to {}:{} (udp)", server_ip, server_port);
            return;
        }

        glog().info("net client: connecting to {}:{} as {}", server_ip, server_port, name);
#if defined(DFDH_DEBUG_PING)
        glog().info("net client: DFDH_DEBUG_PING is enabled, delaying every sent and received datagram by 80..170ms (~500ms ping between two such ends)");
#endif

        hello h;
        h.name         = name;
        h.pistol       = pistol;
        h.face_id      = face_id;
        h.body_id      = body_id;
        h.body_color   = pack_color(body_color);
        h.tracer_color = pack_color(tracer_color);
        _sock.send(encode_msg(msg_type::hello, h));
    }

    net_client(const net_client&) = delete;
    net_client& operator=(const net_client&) = delete;

    /** The game_state passed to update() must outlive the client (it hooks into the simulation ticks) */
    ~net_client() {
        leave();
        if (_gs) {
            _gs->sim.remove_tick_begin_callback("net_client");
            _gs->sim.remove_tick_end_callback("net_client");
        }
    }

    [[nodiscard]]
    bool is_ready() const {
        return _sock.is_open();
    }

    [[nodiscard]]
    bool joined() const {
        return _joined;
    }

    [[nodiscard]]
    const player_name_t& name() const {
        return _name;
    }

    /** Fired (deferred) when the server accepted the client */
    signal<void(player_name_t)> sig_joined;
    /** Fired (deferred) when the server rejected the client */
    signal<void(std::string)>    sig_error;

    using input_source_t = std::function<player_input_state()>;

    /**
     * Source of the local player's input, sampled once per simulation tick
     * (e.g. net_player_controller::take_input_state)
     */
    void input_source(input_source_t source) {
        _input_source = std::move(source);
    }

    /** Smoothed round-trip time to the server, seconds */
    [[nodiscard]]
    float rtt() const {
        return _rtt_s;
    }

    /** Number of prediction corrections (rollbacks) so far */
    [[nodiscard]]
    u64 corrections() const {
        return _corrections;
    }

    /** Largest position change caused by a correction so far, px */
    [[nodiscard]]
    float max_correction() const {
        return _max_correction;
    }

    /** Number of weapon state (ammo, reload, fire rate) corrections so far */
    [[nodiscard]]
    u64 weapon_corrections() const {
        return _weapon_corrections;
    }

    void reset_correction_stats() {
        _corrections        = 0;
        _max_correction     = 0.f;
        _weapon_corrections = 0;
    }

    [[nodiscard]]
    const server_clock& clock_estimate() const {
        return _clock;
    }

    void update(game_state& gs) {
        if (!is_ready())
            return;

        if (!_gs) {
            _gs = &gs;
            gs.sim.add_tick_begin_callback("net_client", [this](const physic_simulation&, float timestep) {
                _tick_dt = timestep;
                sweep_framed_bullets(*_gs);
                sample_input(*_gs, timestep);
            });
            /* After the game's tick end callbacks (player updates: shots, recoil) */
            gs.sim.add_tick_end_callback(
                "net_client",
                [this](const physic_simulation&, float) { record_prediction(*_gs); },
                100);
        }

        register_predicted_bullets(gs);

        for (auto& datagram : _sock.recv(std::chrono::milliseconds(2))) {
            net_msg msg;
            try {
                msg = decode_msg({datagram.data.data(), datagram.data.size()});
            }
            catch (const std::exception& e) {
                glog().warn("net client: bad datagram: {}", e.what());
                continue;
            }

            switch (msg.type) {
            case msg_type::welcome: handle_welcome(msg); break;
            case msg_type::state:   handle_state(gs, msg); break;
            case msg_type::error:   handle_error(msg); break;
            default: break;
            }
        }

        expire_predicted_bullets(gs);
        apply_remote_snapshots(gs);
        send_input_window();
    }

    /** Largest distance between a predicted own bullet and its confirmed trajectory so far, px */
    [[nodiscard]]
    float max_bullet_correction() const {
        return _max_bullet_correction;
    }

    /** Largest correction of a predicted own dropped gun towards the server's so far, px */
    [[nodiscard]]
    float max_gun_correction() const {
        return _max_gun_correction;
    }

    /**
     * Server time at which the server simulates the local player's current
     * input tick (it runs ahead of the server by the input's trip and its
     * wait in the server's queue). Unknown until the server acknowledged an
     * input
     */
    [[nodiscard]]
    std::optional<double> local_frame_time() const {
        if (!_tick_time_offset)
            return {};
        return *_tick_time_offset + double(_input_seq) * double(_tick_dt);
    }

    /** Newest input tick of the local player */
    [[nodiscard]]
    u32 input_seq() const {
        return _input_seq;
    }

    /** Ids of the recent own bullets predicted to hit remote players (once the server confirmed the bullets) */
    [[nodiscard]]
    const std::vector<u32>& own_hit_ids() const {
        return _own_hit_ids;
    }

    /** Own bullets predicted locally and not confirmed by the server yet */
    [[nodiscard]]
    size_t pending_predicted_bullets() const {
        return _predicted.size();
    }

    /* For tests: the channel (e.g. to simulate latency) */
    udp_channel& channel() {
        return _sock;
    }

    void leave() {
        if (!is_ready() || leave_sent)
            return;

        leave_sent = true;
        _sock.send(encode_msg(msg_type::leave, net::leave{_name}));
        glog().info("net client: left the game");
    }

private:
    using clock = game_clock;

    /*
     * Per predicted tick, besides the input: predicted bullet hits (before the
     * movement), shot recoil (after it) and the weapon state after the tick
     */
    struct tick_extra {
        vec2f                                        impulse;
        std::vector<std::pair<bullet_ref, vec2f>>    hits;
        std::optional<weapon_instance::sync_state>   weapon;
    };

    using history_t = input_history<player_input_state, player::physic_state, tick_extra>;


    [[nodiscard]]
    u32 time_ms() const {
        return u32(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - _start_time).count());
    }

    [[nodiscard]]
    double time_s() const {
        return std::chrono::duration<double>(clock::now() - _start_time).count();
    }

    [[nodiscard]]
    player* local_player(game_state& gs) const {
        auto found = gs.players.find(_name);
        return found == gs.players.end() ? nullptr : found->second.get();
    }

    /* Simulation tick begin: sample the local input and apply it to the local player */
    void sample_input(game_state& gs, float timestep) {
        if (!_joined || !_input_source)
            return;

        auto pl = local_player(gs);
        if (!pl)
            return;

        auto in = _input_source();
        ++_input_seq;
        pl->set_predicted(true);
        pl->last_input_seq(_input_seq);
        pl->apply_input(in);

        _sent_inputs.push_back(in.to_bits());
        if (_sent_inputs.size() > input::max_window)
            _sent_inputs.pop_front();

        _history.push({_input_seq, in, {}, timestep});
    }

    /*
     * Simulation tick end (after the player update): remember the predicted
     * state, the bullet hits predicted since the previous tick (applied before
     * the tick's movement) and the recoil of the tick's shots (after it)
     */
    void record_prediction(game_state& gs) {
        auto entry = _history.back();
        auto pl    = local_player(gs);
        if (!entry || !pl || entry->seq != _input_seq)
            return;

        entry->extra.impulse = pl->take_pending_impulse();
        for (auto& hit : pl->take_predicted_hits())
            entry->extra.hits.emplace_back(hit.ref, hit.impulse);
        entry->state = pl->get_physic_state();
        if (pl->get_gun())
            entry->extra.weapon = pl->get_gun().get_sync_state();
    }

    /* Direction of the recoil of a replayed shot: the tick's movement input, else the recorded recoil or the facing */
    template <typename Entry>
    [[nodiscard]]
    static vec2f recoil_dir(const player& pl, const Entry& e) {
        if (e.input.left != e.input.right)
            return {e.input.left ? -1.f : 1.f, 0.f};
        if (!exactly_equal(e.extra.impulse.x, 0.f))
            return {e.extra.impulse.x > 0.f ? -1.f : 1.f, 0.f};
        return pl.facing_dir();
    }

    [[nodiscard]]
    static bool diverged(const player::physic_state& a, const player::physic_state& b) {
        constexpr float pos_eps = 1.f;   /* px */
        constexpr float vel_eps = 10.f;  /* px/s */
        return magnitude(a.pos - b.pos) > pos_eps || magnitude(a.vel - b.vel) > vel_eps ||
               a.lock_y != b.lock_y || a.jumps_left != b.jumps_left;
    }

    /**
     * Reconcile the local player with the server state of the tick ack_seq:
     * on divergence restore it and re-simulate the unacknowledged ticks.
     * The weapon (ammo, reload, fire rate) is reconciled the same way with
     * server_wpn, its server state (if it holds the same gun)
     */
    void reconcile(game_state& gs, player& pl, const player::physic_state& server_st, u32 ack_seq,
                   const std::vector<u32>& server_hits, u16 teleports,
                   const std::optional<weapon_instance::sync_state>& server_wpn) {
        /* Teleported on the server (respawn): jump there right away, even without a new ack */
        bool teleported = !_local_teleports || counter_newer(teleports, *_local_teleports);

        /* Reordered older state, or the same ack again (already reconciled) */
        if (!teleported && (ack_seq < _last_ack || (ack_seq == _last_ack && ack_seq != 0)))
            return;
        _last_ack = std::max(_last_ack, ack_seq);
        if (teleported)
            _local_teleports = teleports;

        /*
         * A lag-compensated hit predicted on an acknowledged tick: the server
         * registers it retroactively, when it sweeps the bullet rewind_ticks
         * later. Its states until then lack the hit: compare with a later one
         */
        std::erase_if(_rewound_hits, [&](const auto& hit) {
            return ack_seq >= hit.second.confirm_seq || std::ranges::find(server_hits, hit.first) != server_hits.end();
        });
        if (!teleported &&
            std::ranges::any_of(_rewound_hits, [&](const auto& hit) { return hit.second.seq <= ack_seq; }))
            return;

        /*
         * Hits the server already applied are in the server state: never replay
         * the prediction of them again (it was predicted later than the server
         * had it, otherwise its tick would be acknowledged already)
         */
        for (auto& e : _history)
            if (e.seq > ack_seq)
                std::erase_if(e.extra.hits, [&](const auto& hit) {
                    return hit.first.id != 0 && std::ranges::find(server_hits, hit.first.id) != server_hits.end();
                });

        auto predicted = _history.find(ack_seq);

        /* The weapon fires and reloads differently than predicted: replay it too (its shots' recoil included) */
        auto& gun          = pl.get_gun();
        bool  replay_weapon = server_wpn && gun && ack_seq != 0 &&
                              (teleported || !predicted || !predicted->extra.weapon ||
                               predicted->extra.weapon->diverged(*server_wpn));

        if (replay_weapon || teleported || !predicted || diverged(predicted->state, server_st)) {
            auto old_pos = pl.get_position();

            pl.restore_physic_state(server_st);
            if (replay_weapon) {
                gun.restore_sync_state(*server_wpn, ack_seq);
                ++_weapon_corrections;
                glog().detail("net client: weapon correction at tick {}: ammo {} (reserve {})", ack_seq, server_wpn->ammo,
                              server_wpn->reserve);
            }

            replay_history(gs, pl, ack_seq, replay_weapon);

            auto correction = old_pos - pl.get_position();
            if (teleported)
                pl.reset_render_offset();
            else
                pl.add_render_offset(correction);
            ++_corrections;
            _max_correction = std::max(_max_correction, magnitude(correction));
            glog().detail("net client: prediction correction at tick {}: {} px", ack_seq, magnitude(correction));
        }

        _history.drop_up_to(ack_seq);
        _ack_state = {ack_seq, server_st};
    }

    /**
     * Re-simulate the local player from its current state over the history
     * ticks after seq: movement input, predicted hits, the physics and the
     * recoil. With replay_weapon the weapon is replayed too (its recoil
     * replaces the recorded one)
     */
    void replay_history(game_state& gs, player& pl, u32 seq, bool replay_weapon) {
        auto& gun = pl.get_gun();
        pl.replaying(true);
        for (auto& e : _history) {
            if (e.seq <= seq)
                continue;
            pl.apply_movement_input(e.input);
            if (replay_weapon) {
                pl.last_input_seq(e.seq);
                if (e.input.shot)
                    pl.shot();
                else
                    pl.relax();
            }
            for (auto& [_, impulse] : e.extra.hits)
                pl.apply_hit_physics(impulse);
            gs.sim.step_isolated(pl.collision_box(), e.timestep, [&](float dt) { pl.physic_update(gs.sim, dt); });
            if (replay_weapon) {
                gun.input_tick(e.seq);
                auto recoil      = gun.replay_tick(e.timestep);
                e.extra.impulse  = recoil ? -recoil_dir(pl, e) * *recoil : vec2f{0.f, 0.f};
                e.extra.weapon   = gun.get_sync_state();
            }
            /* Only a real impulse, like the live tick: applying one rebuilds the velocity without its hidden part */
            if (!exactly_equal(e.extra.impulse.x, 0.f) || !exactly_equal(e.extra.impulse.y, 0.f))
                pl.apply_impulse(e.extra.impulse);
            e.state = pl.get_physic_state();
        }
        pl.last_input_seq(_input_seq);
        pl.replaying(false);
    }

    /* The local player's predicted state after the tick seq (or the server's one at the acknowledged tick) */
    [[nodiscard]]
    const player::physic_state* tick_state(u32 seq) {
        if (auto e = _history.find(seq))
            return &e->state;
        if (_ack_state && _ack_state->first == seq)
            return &_ack_state->second;
        return nullptr;
    }

    /* How a server bullet tests the players (see tick_hit) */
    struct hit_test_t {
        bool  physical     = true;
        float rewind_lead  = 0.f; /* ticks */
        u32   rewind_ticks = 0;

        /**
         * A bullet tested against the players rewind seconds ago: on the
         * server's tick grid, so it is a whole number of ticks (rewind_ticks)
         * ahead of the tested players, which are rewind_lead of a tick
         * before a tick's end
         */
        static hit_test_t of(float rewind, float dt) {
            if (rewind <= 0.f)
                return {};
            auto ticks = std::ceil(rewind / dt - 1e-3f);
            return {false, std::max(ticks - rewind / dt, 0.f), u32(ticks)};
        }

        [[nodiscard]]
        float ahead_s(float dt) const {
            return float(rewind_ticks) * dt;
        }
    };

    struct tick_hit_t {
        u32   seq;      /* the tick to apply the hit in */
        u32   swept_at; /* the tick the bullet path was flown in */
        vec2f position;
    };

    /**
     * Where the bullet path a -> b, flown in the tick seq, hits the local
     * player, like the server tests it:
     * - a lag-compensated bullet (rewound_bullets_tick_update) against the
     *   player's position trace rewind_lead of a tick before the tick's end:
     *   the hit goes before the movement of the tick after the last one
     *   ended by then (apply_hit_at);
     * - a physical one (the physic simulation's continuous test,
     *   physical_hit) against the player moving through the tick: the hit
     *   goes before the movement of the tick itself
     */
    [[nodiscard]]
    std::optional<tick_hit_t> tick_hit(const player& pl, u32 seq, const vec2f& a, const vec2f& b,
                                       const hit_test_t& test) {
        auto end = tick_state(seq);
        if (!end)
            return {};
        auto size = pl.get_size();

        if (!test.physical) {
            auto pos      = end->pos;
            auto hit_tick = seq + 1;
            if (test.rewind_lead > 1e-3f) {
                auto begin = tick_state(seq - 1);
                if (!begin)
                    return {};
                pos      = lerp(end->pos, begin->pos, test.rewind_lead);
                hit_tick = seq;
            }
            if (auto t = game_state::segment_hit(a, b, {pos.x, pos.y - size.y, size.x, size.y}))
                return tick_hit_t{hit_tick, seq, a + (b - a) * *t};
            return {};
        }

        auto begin = tick_state(seq - 1);
        if (!begin)
            return {};
        auto vel = begin->lock_y ? vec2f{begin->vel.x, 0.f} : begin->vel;
        if (auto t = physical_hit(begin->pos, vel * _tick_dt, size, a, b))
            return tick_hit_t{seq, seq, a + (b - a) * *t};
        return {};
    }

    /**
     * The simulation's continuous test (physic_simulation::analyze) of a
     * point flying a -> b in a tick against the player's box at pos (its
     * bottom left corner), moved by the tick's start velocity (move): a side
     * whose swept bounding box meets the point's is hit when the point
     * crosses its line. Both move linearly, so the distance to the line does
     * too. Returns the point's fraction of the path at the hit
     */
    [[nodiscard]]
    static std::optional<float> physical_hit(const vec2f& pos, const vec2f& move, const vec2f& size, const vec2f& a,
                                             const vec2f& b) {
        auto bounds = [](std::initializer_list<vec2f> points) {
            auto lo = *points.begin(), hi = lo;
            for (auto& p : points) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
            }
            return sf::FloatRect{lo.x, lo.y, std::max(hi.x - lo.x, 0.1f), std::max(hi.y - lo.y, 0.1f)};
        };

        /* The player's collision lines: start, displacement */
        const std::array<std::pair<vec2f, vec2f>, 4> sides = {{{{0.f, 0.f}, {0.f, -size.y}},
                                                               {{0.f, -size.y}, {size.x, 0.f}},
                                                               {{size.x, -size.y}, {0.f, size.y}},
                                                               {{size.x, 0.f}, {-size.x, 0.f}}}};
        auto point_bb = bounds({a, b});
        for (auto& [offset, disp] : sides) {
            auto p0 = pos + offset;
            auto p1 = p0 + move;
            if (!bounds({p0, p0 + disp, p1, p1 + disp}).intersects(point_bb))
                continue;

            auto dist = [&](const vec2f& point, const vec2f& line) {
                return disp.x * (point.y - line.y) - disp.y * (point.x - line.x);
            };
            auto d0 = dist(a, p0);
            auto d1 = dist(b, p1);
            if ((d0 >= 0.f) != (d1 >= 0.f))
                return d0 / (d0 - d1);
        }
        return {};
    }

    /* Whether a bullet of the owner may hit the local player: never its owner nor the owner's group */
    [[nodiscard]]
    static bool may_hit(const player& local, const player* owner, int group) {
        return &local != owner && (group == -1 || local.get_group() == -1 || local.get_group() != group);
    }

    /**
     * A predicted hit on the local player in the tick hit.seq (already
     * simulated, or the next one): it goes before the tick's movement, like
     * the server's (apply_hit_at), the ticks from there on are re-simulated.
     * A lag-compensated hit is registered by the server rewind_ticks after
     * the tick its bullet was swept in: its states until then lack it
     * (see reconcile)
     */
    void apply_tick_hit(game_state& gs, player& pl, const tick_hit_t& hit, const vec2f& impulse, const bullet_ref& ref,
                        const hit_test_t& test) {
        if (!test.physical && ref.id != 0)
            _rewound_hits[ref.id] = {hit.seq, hit.swept_at + test.rewind_ticks};

        auto seq = hit.seq;
        auto pos = hit.position;
        if (seq > _input_seq) {
            pl.on_bullet_hit(impulse, pos, ref); /* recorded with the next tick */
            return;
        }

        auto e     = _history.find(seq);
        auto begin = tick_state(seq - 1);
        if (!e || !begin)
            return;
        e->extra.hits.emplace_back(ref, impulse);
        auto old_pos = pl.get_position();
        pl.restore_physic_state(*begin);
        replay_history(gs, pl, seq - 1, false);
        pl.add_render_offset(old_pos - pl.get_position());
        pl.hit_feedback(impulse, pos, ref);
    }

    /**
     * A server bullet arrives after it already crossed the local player's
     * frame for a while (the input's trip, the server queue, the state's
     * trip): test its path against the predicted ticks it flew through.
     * At the end of tick j it is flight - (current tick - j) * dt into its
     * flight. A hit there (before the flight max_flight) is applied where
     * the server applies it (tick_hit, apply_tick_hit). The current tick is
     * left to sweep_framed_bullets
     */
    bool predict_past_hit(game_state& gs, player& pl, const vec2f& start, const vec2f& vel, bool gravity, float flight,
                          float mass, float max_flight, const hit_test_t& test, const bullet_ref& ref) {
        for (auto& e : _history) {
            if (e.seq >= _input_seq)
                break;
            auto f = flight - float(_input_seq - e.seq) * _tick_dt;
            if (f <= 0.f)
                continue;
            if (f > max_flight)
                break;

            auto a       = gs.ballistic_forward(start, vel, gravity, std::max(f - _tick_dt, 0.f)).first;
            auto [b, bv] = gs.ballistic_forward(start, vel, gravity, f);
            if (auto hit = tick_hit(pl, e.seq, a, b, test)) {
                apply_tick_hit(gs, pl, *hit, bv * mass, ref, test);
                return true;
            }
        }
        return false;
    }

    /**
     * Spawn a server bullet in the local simulation, in the predicted local
     * player's frame (see bullet::frame_on_client): the server simulates the
     * local player's current tick frame_ahead seconds after the bullet's
     * state, and tests a lag-compensated bullet against it the shooter's
     * rewind later (on its tick grid: rewind_ticks). So it spawns at
     * start + vel * (shooter lag + frame_ahead + rewind ticks). The path from
     * the muzzle to where the remote players (shown in the past) see it is
     * hit-scanned against them, the path through the ticks the local player
     * already simulated against those ticks (predict_past_hit), the next ones
     * are swept every tick (sweep_framed_bullets). Hits are predictions: the
     * server decides them.
     * Without frame_ahead (no input acknowledged yet) the bullet is forwarded
     * by the age of its state (state_age, server present time) and collides
     * physically. Bullets already dead on the server (consumed by the server
     * hit-scan) are not spawned
     */
    void spawn_server_bullet(game_state& gs, const shot_info& shot, size_t pellet, bool alive, float lag,
                             std::optional<float> frame_ahead) {
        auto owner_cfg = _configs.find(shot.owner);
        auto owner     = owner_cfg != _configs.end() ? gs.players.find(owner_cfg->second.name) : gs.players.end();
        auto id        = shot.first_id + u32(pellet);
        auto key       = shot_key_t{shot.pull_seq, shot.shot_index, u32(shot.first_pellet + pellet)};
        auto gravity   = (shot.flags & shot_info::flag_gravity) != 0;
        auto start     = vec2f{shot.start_x, shot.start_y};
        auto vel       = vec2f{shot.pellets[pellet].vel_x, shot.pellets[pellet].vel_y};
        auto test     = hit_test_t::of(shot.rewind_s(), _tick_dt);
        auto flight   = frame_ahead ? shot.forward_s() + *frame_ahead + test.ahead_s(_tick_dt) : shot.forward_s() + lag;
        auto [pos, fwd_vel] = gs.ballistic_forward(start, vel, gravity, flight);

        if (_joined && owner_cfg != _configs.end() && owner_cfg->second.name == _name &&
            confirm_predicted_bullet(gs, key, id, start, vel, gravity, alive))
            return;

        auto local        = frame_ahead ? local_player(gs) : nullptr;
        auto shown_flight = std::max(flight - test.rewind_lead * _tick_dt - shown_delay(), 0.f);
        std::optional<game_state::hitscan_hit_t> shown_hit;
        auto shown_hit_flight = std::numeric_limits<float>::max();
        if (owner != gs.players.end()) {
            auto shown_pos = frame_ahead ? gs.ballistic_forward(start, vel, gravity, shown_flight).first : pos;
            auto shown     = [local](const player& p) { return &p != local; };
            shown_hit = gs.hitscan_if(start, shown_pos, 0.f, owner->second.get(), owner->second->get_group(), shown);
            if (shown_hit)
                shown_hit_flight =
                    shown_flight * magnitude(shown_hit->position - start) / std::max(magnitude(shown_pos - start), 1e-3f);
        }

        /* Whichever the bullet reaches first in its flight. Like on the server: never its owner nor the owner's group */
        auto owner_pl = owner != gs.players.end() ? owner->second.get() : nullptr;
        if (local && may_hit(*local, owner_pl, owner_pl ? owner_pl->get_group() : -1) &&
            predict_past_hit(gs, *local, start, vel, gravity, flight, shot.hit_mass, shown_hit_flight, test,
                             bullet_ref{id, {}, owner_pl ? owner_pl->name() : player_name_t{}}))
            return;

        if (shown_hit) {
            shown_hit->target->on_bullet_hit(vel * shot.hit_mass, shown_hit->position, bullet_ref{id, {}});
            return;
        }

        if (!alive)
            return;

        /* Like on the server: never hits its owner nor the owner's group */
        auto& blt = owner != gs.players.end()
                        ? gs.blt_mgr.shot(gs.sim, pos, shot.hit_mass, fwd_vel, owner->second->tracer_color(), gravity,
                                          owner->second->get_group(), player::player_group_getter,
                                          player::owner_checker(owner->second.get()))
                        : gs.blt_mgr.shot(gs.sim, pos, shot.hit_mass, fwd_vel, sf::Color::White, gravity);
        blt.net_id(id);
        if (frame_ahead) {
            if (owner != gs.players.end())
                blt.owner(owner->second->name());
            /* The local player is swept from the tick it is at now on */
            auto from = gs.ballistic_forward(start, vel, gravity, std::max(flight - _tick_dt, 0.f)).first;
            blt.frame_on_client({start, vel, gravity, shot.rewind_s(), flight, shown_flight}, from);
        }
    }

    /*
     * How far the remote players are shown behind the local player's frame
     * (the server time of its tick minus the shown server time), s
     */
    [[nodiscard]]
    float shown_delay() const {
        auto local = local_frame_time();
        if (!local || !_shown_time)
            return 0.f;
        return std::clamp(float(*local - *_shown_time), 0.f, game_state::max_lag_compensation);
    }

    /**
     * Where an own predicted bullet flying a -> b in the last tick, flight
     * seconds after the shot, hits a remote player, like the server tests it
     * (game_state::spawn_bullet, rewound_bullets_tick_update): the server
     * applies the shot tick k at S_k, forwarded by lag (the skipped path is
     * hit-scanned against the players at S_k - rewind), then sweeps each
     * tick's path against the players rewind ago. So the players are tested
     * at S_k - rewind + max(flight - lag, 0), where
     * S_k = local_frame_time() - flight: about the shown time, so it is in
     * the snapshots. Plus the knockbacks predicted on top of them
     */
    std::optional<std::pair<player*, vec2f>> own_bullet_hit(game_state& gs, const vec2f& a, const vec2f& b, float flight,
                                                            const player* local, int group) {
        auto frame = local_frame_time();
        if (!frame && !_shown_time)
            return {};
        auto t = frame ? *frame - double(_own_rewind_s + std::min(flight, _own_lag_s)) : *_shown_time;

        std::optional<std::pair<player*, vec2f>> res;
        auto best = std::numeric_limits<float>::max();
        for (auto& [name, snaps] : _remote) {
            auto found = gs.players.find(name);
            if (found == gs.players.end() || found->second.get() == local)
                continue;
            auto& pl = *found->second;
            if (group != -1 && pl.get_group() != -1 && pl.get_group() == group)
                continue;
            auto snap = snaps.sample(t);
            if (!snap)
                continue;

            /*
             * Teleported since: the server clears the position trace on a
             * teleport, so rewinding across it finds the first position after
             * the teleport (the server has it by the time it sweeps)
             */
            auto newest = snaps.newest();
            if (newest->teleports != snap->teleports)
                snap = snaps.first_where([&](const remote_player_snapshot& s) { return s.teleports == newest->teleports; });

            /* The knockbacks predicted on top of the shown snapshots (they are dropped on a teleport) */
            auto pos = snap->ph.pos;
            if (auto kb = _knockbacks.find(name); kb != _knockbacks.end() && _remote_teleports[name] == snap->teleports)
                pos += kb->second.offset();
            auto size = pl.get_size();
            if (auto hit = game_state::segment_hit(a, b, {pos.x, pos.y - size.y, size.x, size.y}); hit && *hit < best) {
                best = *hit;
                res  = std::pair{&pl, a + (b - a) * *hit};
            }
        }
        return res;
    }

    /**
     * Tick begin: sweep the path every framed bullet flew last tick against
     * the local player (like the server tests it: tick_hit, apply_tick_hit),
     * and the delayed path against the remote players where they are shown.
     * Own predicted bullets are swept against the remote players
     * (own_bullet_hit). Then the bullets fly the tick starting now
     */
    void sweep_framed_bullets(game_state& gs) {
        auto local = local_player(gs);
        auto delay = shown_delay();
        for (auto& blt : gs.blt_mgr.bullets()) {
            auto& fr = blt.framed();
            auto& ph = blt.physic();
            if (!fr || ph->ready_delete_later())
                continue;

            auto found = gs.players.find(blt.owner());
            auto owner = found != gs.players.end() ? found->second.get() : nullptr;
            auto hit_by = [&](const game_state::hitscan_hit_t& hit) {
                hit.target->on_bullet_hit(ph->impulse(), hit.position, bullet_ref::of(ph.get()));
                ph->delete_later();
            };

            /* The path flown in the last tick (_input_seq) */
            auto [from, to] = blt.take_swept_segment();
            auto flight     = std::exchange(fr->flight, fr->flight + _tick_dt);

            if (fr->own) {
                if (auto hit = own_bullet_hit(gs, from, to, flight, local, blt.group())) {
                    hit->first->on_bullet_hit(ph->impulse(), hit->second, bullet_ref::of(ph.get()));
                    ph->delete_later();
                }
                continue;
            }

            if (local && may_hit(*local, owner, blt.group())) {
                auto test = hit_test_t::of(fr->rewind, _tick_dt);
                if (auto hit = tick_hit(*local, _input_seq, from, to, test)) {
                    apply_tick_hit(gs, *local, *hit, ph->impulse(), bullet_ref::of(ph.get()), test);
                    ph->delete_later();
                    continue;
                }
            }

            auto shown = std::max(flight - hit_test_t::of(fr->rewind, _tick_dt).rewind_lead * _tick_dt - delay, 0.f);
            if (shown <= fr->shown_flight)
                continue;
            auto a = gs.ballistic_forward(fr->start, fr->velocity, fr->gravity, fr->shown_flight).first;
            auto b = gs.ballistic_forward(fr->start, fr->velocity, fr->gravity, shown).first;
            fr->shown_flight = shown;
            if (auto hit = gs.hitscan_if(a, b, 0.f, owner, blt.group(), [local](const player& p) { return &p != local; }))
                hit_by(*hit);
        }
    }

    struct predicted_bullet_t {
        shot_key_t      key;
        std::weak_ptr<physic_point> physic;
        double                      time_s;
    };

    /* Track the own bullets the local weapon spawned this frame (game_state::spawn_bullet) */
    void register_predicted_bullets(game_state& gs) {
        for (auto& be : gs.cur_frame_bullets)
            if (be.predicted && be.owner == _name)
                _predicted.push_back({be.key, be.physic, time_s()});
    }

    /**
     * The authoritative counterpart of a predicted own bullet arrived (same
     * shot key): the prediction takes its id and its trajectory (start and
     * velocity; the elapsed time since the shot is known exactly here, better
     * than the server's lag estimate), or disappears if the bullet is already
     * dead on the server. Returns false if nothing was predicted for the key
     */
    bool confirm_predicted_bullet(game_state& gs, const shot_key_t& key, u32 id, const vec2f& start, const vec2f& vel,
                                  bool gravity, bool alive) {
        auto found = std::ranges::find_if(_predicted, [&](const predicted_bullet_t& p) { return p.key == key; });
        if (found == _predicted.end())
            return false;

        if (auto ph = found->physic.lock(); ph && !ph->ready_delete_later()) {
            if (alive) {
                /* Simulated flight time of the prediction (its travelled distance over its speed) */
                auto elapsed    = ph->get_distance() / std::max(ph->scalar_velocity(), 1.f);
                auto [pos, fwd_vel]    = gs.ballistic_forward(start, vel, gravity, elapsed);
                auto correction        = magnitude(pos - ph->get_position());
                _max_bullet_correction = std::max(_max_bullet_correction, correction);
                if (correction > 1.f)
                    glog().detail("net client: predicted bullet corrected by {} px", correction);
                ph->position(pos);
                ph->velocity(fwd_vel);
                for (auto& blt : gs.blt_mgr.bullets())
                    if (blt.physic() == ph) {
                        blt.net_id(id);
                        blt.restart_sweep(); /* not across the correction */
                    }
            }
            else
                ph->delete_later(); /* it hit something on the server */
        }

        /* Hits predicted for this bullet can now be matched to the server's */
        if (std::erase(_own_hit_keys, key) != 0)
            _own_hit_ids.push_back(id);
        _own_bullet_ids.insert(id);
        trim_own_hit_stats();
        for (auto& [_, kb] : _knockbacks)
            kb.for_each_hit([&](auto& hit) {
                if (hit.ref.key && *hit.ref.key == key)
                    hit.ref.id = id;
            });

        _predicted.erase(found);
        return true;
    }

    /* The own hit stats keep the recent bullets only */
    void trim_own_hit_stats() {
        constexpr size_t keep = 1024;
        while (_own_bullet_ids.size() > keep)
            _own_bullet_ids.erase(_own_bullet_ids.begin());
        if (_own_hit_ids.size() > keep)
            _own_hit_ids.erase(_own_hit_ids.begin(), _own_hit_ids.end() - keep);
        if (_own_hit_keys.size() > keep)
            _own_hit_keys.erase(_own_hit_keys.begin(), _own_hit_keys.end() - keep);
    }

    /* Predictions the server never confirmed (the server did not fire that shot) */
    void expire_predicted_bullets(game_state& gs) {
        auto timeout = std::max(0.5, double(_rtt_s) * 2.0 + 0.25);
        auto now     = time_s();
        bool removed = false;
        while (!_predicted.empty() && now - _predicted.front().time_s > timeout) {
            if (auto ph = _predicted.front().physic.lock()) {
                ph->delete_later();
                removed = true;
            }
            _predicted.pop_front();
        }
        if (removed)
            gs.blt_mgr.remove_dead();
    }

    /**
     * Hidden player that simulates the knockback ghosts of the remote player pl
     * (same movement parameters). Not in the simulation and not a game player:
     * stepping it never touches what is shown (legs, weapon bobbing, ...)
     */
    player& ghost_of(game_state& gs, const player& pl) {
        if (!_ghost) {
            _ghost = player::create(player_name_t("~knockback_ghost"), gs.sim, pl.get_size());
            gs.sim.remove_primitive(_ghost->collision_box());
            _ghost->replaying(true);
            _ghost->set_server_driven(true);
        }
        _ghost->max_speed(pl.get_max_speed());
        _ghost->x_accel(pl.get_x_accel());
        _ghost->x_slowdown(pl.get_x_slowdown());
        _ghost->jump_speed(pl.get_jump_speed());
        _ghost->mass(pl.collision_box()->get_mass());
        return *_ghost;
    }

    /* Every frame: show remote players at the interpolated server time */
    void apply_remote_snapshots(game_state& gs) {
        if (!_clock.valid())
            return;

        auto now = time_s();
        auto dt  = float(std::clamp(now - _last_remote_apply_s, 0.0, 0.1));
        _last_remote_apply_s = now;

        auto t = _clock.render_time(now);
        _shown_time = t;
        for (auto& [name, snaps] : _remote) {
            /* A state received before WELCOME could not tell the local player apart */
            if (_joined && name == _name)
                continue;

            auto found = gs.players.find(name);
            if (found == gs.players.end())
                continue;
            auto snap = snaps.sample(t);
            if (!snap)
                continue;

            auto& pl = *found->second;
            auto& kb = _knockbacks[name];

            /* Where the player is shown now: last frame's state moved on by the local simulation */
            auto shown       = pl.get_physic_state();
            auto& seen       = _remote_teleports[name];
            bool  continuous = seen.has_value();

            /* Teleported: jump there, the predicted knockbacks belong to the old position */
            if (seen != snap->teleports) {
                seen       = snap->teleports;
                kb         = {};
                continuous = false;
                (void)pl.take_predicted_hits();
                pl.reset_render_offset();
            }

            /*
             * Predicted hits: shown as a knockback offset until the snapshots
             * report them. The offset is simulated with the game physics on a
             * hidden ghost player (the shown one is never touched)
             */
            auto& ghost     = ghost_of(gs, pl);
            auto  apply_hit = [&](player::physic_state& st, const vec2f& impulse) {
                ghost.restore_physic_state(st);
                ghost.apply_hit_physics(impulse);
                st = ghost.get_physic_state();
            };
            auto step_ghost = [&](player::physic_state& st, float timestep) {
                ghost.restore_physic_state(st);
                gs.sim.step_isolated(ghost.collision_box(), timestep, [&](float ts) { ghost.physic_update(gs.sim, ts); });
                st = ghost.get_physic_state();
            };

            for (auto& hit : pl.take_predicted_hits()) {
                if (hit.ref.key)
                    _own_hit_keys.push_back(*hit.ref.key);
                else if (_own_bullet_ids.contains(hit.ref.id))
                    _own_hit_ids.push_back(hit.ref.id);
                kb.predict(hit.ref, hit.impulse, shown, apply_hit);
            }
            trim_own_hit_stats();
            for (auto id : snap->hits)
                kb.confirm([id](const bullet_ref& ref) { return ref.id == id; }, snap->ph, apply_hit);

            /* The snapshots report a registered hit about one view delay (lag + interpolation) after it */
            auto view_delay = 0.5f * _rtt_s + float(_clock.interp_delay());
            kb.step(dt, gs.sim.last_timestep(), std::clamp(1.5f * view_delay + 0.2f, 0.3f, 1.f), step_ghost);

            auto ph = snap->ph;
            ph.pos += kb.offset();
            ph.vel += kb.velocity();
            if (auto grounded = kb.grounded())
                ph.lock_y = *grounded;

            /*
             * A jump of the shown position (a confirmed prediction handing over,
             * the server's rewritten history of a retroactive hit) is smoothed.
             * Measured against the previous frame's position moved on by the
             * frame's velocity, not against the locally simulated one (frames
             * shorter than a tick would see the local tick's movement as a
             * jump). The velocity may have changed at any time in the frame
             * (a hit, a landing): that much is motion, not a jump
             */
            constexpr float max_continuous_step = 8.f;
            auto&           prev                = _remote_applied[name];
            if (continuous && prev) {
                auto shown_vel = [](const player::physic_state& st) {
                    return st.lock_y ? vec2f{st.vel.x, 0.f} : st.vel;
                };
                auto v0        = shown_vel(*prev);
                auto v1        = shown_vel(ph);
                auto expected  = prev->pos + (v0 + v1) * (0.5f * dt);
                auto tolerance = max_continuous_step + 0.5f * magnitude(v1 - v0) * dt;
                if (magnitude(expected - ph.pos) > tolerance)
                    pl.add_render_offset(expected - ph.pos);
            }
            prev = ph;

            pl.restore_physic_state(ph);
            pl.place_legs();
            pl.on_left(snap->on_left);

            /*
             * Drive the remote player's pistol locally (muzzle flash, recoil,
             * sounds). The actual bullets arrive via the state broadcast
             */
            if (snap->shooting)
                pl.shot();
            else
                pl.relax();

            /* The throw is shown only: the server drops the gun (and the pistol sync removes it) */
            if (snap->throwing && !pl.throwing())
                pl.drop_gun();
        }
    }

    /* Send the window of the latest input ticks (redundant: lost datagrams cost nothing) */
    void send_input_window() {
        if (!_joined || _sent_inputs.empty() || _input_seq == _last_sent_seq)
            return;

        input msg;
        msg.last_seq       = _input_seq;
        msg.client_time_ms = time_ms();
        msg.rtt_ms         = u16(std::min(_rtt_s * 1000.f, 65535.f));
        msg.interp_ms      = u16(std::min(_clock.interp_delay() * 1000.0, 65535.0));
        /* The ticks the server did not acknowledge yet */
        auto count = std::clamp<size_t>(_input_seq - std::min(_last_ack, _input_seq), 1, _sent_inputs.size());
        msg.bits.assign(_sent_inputs.end() - std::ptrdiff_t(count), _sent_inputs.end());
        _sock.send(encode_msg(msg_type::input, msg));
        _last_sent_seq = _input_seq;
    }

    void update_rtt(const state& st) {
        if (st.echo_client_time_ms == 0)
            return;

        auto sample = float(i64(time_ms()) - i64(st.echo_client_time_ms) - i64(st.echo_hold_ms)) / 1000.f;
        if (sample < 0.f)
            return;
        _rtt_s = _rtt_measured ? _rtt_s + (sample - _rtt_s) * 0.1f : sample;
        _rtt_measured = true;

        auto now = time_s();
        _rtt_samples.push_back({now, sample});
        while (_rtt_samples.size() > 1 && _rtt_samples.front().first < now - server_clock::window_s)
            _rtt_samples.pop_front();
        _rtt_min_s = std::ranges::min_element(_rtt_samples, {}, [](auto& smp) { return smp.second; })->second;
    }

    /**
     * Age of a state when it arrived: half of the fastest recent round trip
     * plus this datagram's extra delay over the fastest recent state
     */
    [[nodiscard]]
    float state_age(const state& st) const {
        auto excess = _clock.excess_delay(time_s(), double(st.server_time_ms) / 1000.0);
        return _rtt_min_s * 0.5f + float(excess);
    }

    /**
     * The server simulated the input tick ack_seq at the state's time and
     * applies one input tick per server tick: the server time of input tick
     * k is offset + k * dt. Smoothed, a queue wait or a merge moves it
     */
    void update_tick_time(const state& st) {
        if (!_joined || st.ack_seq == 0 || st.ack_seq <= _tick_time_ack)
            return;
        _tick_time_ack = st.ack_seq;

        auto sample = double(st.server_time_ms) / 1000.0 - double(st.ack_seq) * double(_tick_dt);
        if (!_tick_time_offset || std::abs(sample - *_tick_time_offset) > 0.25)
            _tick_time_offset = sample;
        else
            *_tick_time_offset += (sample - *_tick_time_offset) * 0.1;
    }

    void handle_welcome(const net_msg& msg) {
        welcome w;
        if (!try_read_msg(msg.payload, w))
            return;

        _joined = true;
        glog().info("net client: joined the game as {}", w.name);
        sig_joined.emit_deferred(w.name);
    }

    void handle_error(const net_msg& msg) {
        error e;
        if (!try_read_msg(msg.payload, e))
            return;

        glog().error("net client: server error: {}", e.message);
        sig_error.emit_deferred(e.message);
    }

    void handle_state(game_state& gs, const net_msg& msg) {
        state st;
        if (!try_read_msg(msg.payload, st))
            return;

        update_rtt(st);

        _clock.on_state(time_s(), double(st.server_time_ms) / 1000.0);
        update_tick_time(st);

        /* States may arrive out of order: only the newest one updates the world state */
        bool newest = st.tick > _last_state_tick;
        if (newest) {
            _last_state_tick = st.tick;
            _own_lag_s       = float(st.own_lag) / shot_info::forward_scale;
            _own_rewind_s    = float(st.own_rewind) / shot_info::forward_scale;
        }

        /* The server sets the game mode: only the newest state changes it */
        if (st.mode && newest) {
            gs.game_mode_networked = true;
            if (auto gm = to_game_mode(*st.mode); gm != gs.cur_game_mode)
                gs.game_mode_set(gm);
        }

        /*
         * Player configs: only the newest state changes a known one (an older
         * one could bring back a stale config); unknown ones from any state
         */
        for (auto& cfg : st.configs) {
            auto known = _configs.find(cfg.id);
            if (known != _configs.end() && (known->second == cfg || !newest))
                continue;
            _configs[cfg.id] = cfg;
            apply_player_config(gs, cfg);
        }

        for (auto& sp : st.players) {
            /* Not known yet: its config arrives within config_resend after the join */
            auto cfg = _configs.find(sp.id);
            if (cfg == _configs.end())
                continue;
            auto& name     = cfg->second.name;
            auto  is_local = _joined && name == _name;

            if (!gs.players.contains(name))
                apply_player_config(gs, cfg->second);
            auto found = gs.players.find(name);
            if (found == gs.players.end())
                continue;
            auto pl = found->second.get();

            /*
             * Every server-known player is authoritative: bullets for it come
             * from the server broadcast, so never spawn them locally (this
             * also covers locally simulated bots that exist on both sides)
             */
            pl->set_server_driven(true);
            if (newest)
                pl->score(sp.score);

            /* A mispredicted switch to the secondary gun (the server kept the gun): take the server's gun back */
            if (is_local && newest && pl->pistol_section() != cfg->second.pistol && !pl->throwing() &&
                st.ack_seq > pl->predicted_switch_seq() + predicted_switch_confirm_ticks)
                pl->setup_pistol_on_release(cfg->second.pistol, gs.cur_game_mode.magazines_count);

            if (is_local)
                reconcile(gs, *pl, to_physic_state(sp), st.ack_seq, sp.hits, sp.teleports,
                          st.own_weapon && cfg->second.pistol == pl->pistol_section()
                              ? std::optional{to_weapon_state(*st.own_weapon)}
                              : std::nullopt);
            else
                _remote[name].push(double(st.server_time_ms) / 1000.0,
                                   remote_player_snapshot{to_physic_state(sp),
                                                          (sp.flags & state_player::flag_on_left) != 0,
                                                          (sp.flags & state_player::flag_shooting) != 0,
                                                          (sp.flags & state_player::flag_throwing) != 0,
                                                          sp.hits,
                                                          sp.teleports});
        }

        auto age = std::min(state_age(st), game_state::max_lag_compensation);
        /* How far the local player's frame is ahead of this state */
        std::optional<float> frame_ahead;
        if (auto local = local_frame_time())
            frame_ahead = std::clamp(float(*local - double(st.server_time_ms) / 1000.0), 0.f,
                                     game_state::max_lag_compensation);
        for (auto& shot : st.shots)
            for (size_t i = 0; i < shot.pellets.size(); ++i)
                spawn_server_bullet(gs, shot, i, st.alive_bullets.contains(shot.first_id + u32(i)), age, frame_ahead);

        if (!newest)
            return;

        /*
         * Drop local bullets the server no longer lists as alive: they hit
         * something on the server but may have missed locally because the
         * local simulation diverged slightly
         */
        gs.blt_mgr.remove_not_in([&](u32 id) { return st.alive_bullets.contains(id); });

        /* Sync server crates: create ghost crates for new ids, drive their
         * position/velocity, remove crates picked up (or gone) on the server */
        std::vector<u32> received_ids;
        for (auto& ci : st.crates) {
            crate* local = nullptr;
            for (auto& c : gs.crates)
                if (c->id() == ci.id) {
                    local = c.get();
                    break;
                }

            if (!local) {
                local = gs.crate_create_net();
                local->id(ci.id);
                glog().info("net client: crate {} synced at ({}, {})", ci.id, ci.pos_x, ci.pos_y);
            }

            local->position({ci.pos_x, ci.pos_y});
            local->velocity({ci.vel_x, ci.vel_y});
            received_ids.push_back(ci.id);
        }

        for (auto it = gs.crates.begin(); it != gs.crates.end();) {
            bool keep = std::find(received_ids.begin(), received_ids.end(), (*it)->id()) != received_ids.end();
            it = keep ? std::next(it) : gs.crates.erase(it);
        }

        /* Sync server dropped guns: create ghost guns for new ids, drive
         * their position/velocity/angle, remove guns gone on the server */
        std::vector<u32> received_gun_ids;
        for (auto& gi : st.guns) {
            dropped_gun* local = nullptr;
            for (auto& g : gs.dropped_guns.guns())
                if (g->id() == gi.id && (g->net_synced() || g->confirmed())) {
                    local = g.get();
                    break;
                }

            /* The authoritative counterpart of the local player's predicted throw */
            auto owner = _configs.find(gi.owner);
            if (!local && _joined && gi.owner != 0 && owner != _configs.end() && owner->second.name == _name)
                local = confirm_predicted_gun(gs, gi.id);

            if (!local) {
                local = gs.dropped_guns.spawn_net(gi.id, gi.section, {gi.pos_x, gi.pos_y},
                                                  gi.on_left != 0, gi.long_shot != 0);
                glog().info("net client: dropped gun {} synced at ({}, {})", gi.id, gi.pos_x, gi.pos_y);
            }

            if (local->predicted())
                correct_predicted_gun(gs, *local, gi);
            else {
                local->position({gi.pos_x, gi.pos_y});
                local->velocity({gi.vel_x, gi.vel_y});
                local->sync_angle(gi.angle);
            }
            received_gun_ids.push_back(gi.id);
        }

        /* Predicted guns the server never confirmed (the server did not drop that gun) */
        auto predicted_timeout = std::max(1.f, _rtt_s * 2.f + 0.5f);
        for (auto it = gs.dropped_guns.guns().begin(); it != gs.dropped_guns.guns().end();) {
            auto& g = *it;
            bool keep = g->predicted() && !g->confirmed()
                            ? g->age() < predicted_timeout
                            : (!g->net_synced() && !g->confirmed()) ||
                                  std::find(received_gun_ids.begin(), received_gun_ids.end(), g->id()) !=
                                      received_gun_ids.end();
            it = keep ? std::next(it) : gs.dropped_guns.guns().erase(it);
        }
    }

    /* The server's gun of the local player's throw arrived: the oldest unconfirmed prediction takes its id */
    dropped_gun* confirm_predicted_gun(game_state& gs, u32 id) {
        dropped_gun* oldest = nullptr;
        for (auto& g : gs.dropped_guns.guns())
            if (g->predicted() && !g->confirmed() && (!oldest || g->age() > oldest->age()))
                oldest = g.get();

        if (oldest) {
            /* Hits predicted for the gun can now be matched to the server's */
            auto predicted_ref = oldest->hit_ref_id();
            oldest->confirm(id);
            auto resolve = [&](auto& hit) {
                if (hit.ref.id == predicted_ref)
                    hit.ref.id = oldest->hit_ref_id();
            };
            for (auto& [_, kb] : _knockbacks)
                kb.for_each_hit(resolve);
            for (auto& [_, pl] : gs.players)
                pl->for_each_predicted_hit(resolve);

            glog().info("net client: predicted dropped gun confirmed as {}", id);
        }
        return oldest;
    }

    /**
     * A predicted gun follows the server's one: the server state is
     * forwarded by the flight time the prediction is ahead of it (the local
     * throw was released that much earlier on the local timeline) along the
     * ballistic path, the remaining difference is smoothed away. A state
     * from before the prediction's last bounce can't be forwarded past it:
     * the server bounces there too (rewound hits), or a later state corrects
     */
    void correct_predicted_gun(game_state& gs, dropped_gun& g, const gun_info& gi) {
        if (auto hit_age = g.last_hit_age(); hit_age && float(gi.age_ms) / 1000.f < *hit_age)
            return;

        auto lead = std::clamp(g.age() - float(gi.age_ms) / 1000.f, 0.f, game_state::max_lag_compensation);
        auto grav = gs.sim.gravity();
        auto vel  = vec2f{gi.vel_x, gi.vel_y};
        auto pos  = vec2f{gi.pos_x, gi.pos_y} + vel * lead + grav * (0.5f * lead * lead);

        auto correction     = magnitude(pos - g.get_position());
        _max_gun_correction = std::max(_max_gun_correction, correction);
        if (correction > 1.f)
            glog().detail("net client: predicted dropped gun corrected by {} px", correction);

        g.correct(pos, vel + grav * lead, gi.angle + g.angle_velocity() * lead);
    }

    /* Create the player of a config or update it (only what differs: a new pistol resets the weapon) */
    static void apply_player_config(game_state& gs, const player_config& cfg) {
        auto found = gs.players.find(cfg.name);
        auto pl    = found != gs.players.end() ? found->second.get() : gs.player_create(cfg.name);
        if (!pl)
            return;

        auto face = "player/face" + std::to_string(u32(cfg.face_id)) + ".png";
        auto body = "player/body" + std::to_string(u32(cfg.body_id)) + ".png";
        if (pl->body_texture_path() != body || pl->body_color() != unpack_color(cfg.body_color))
            pl->set_body(body, unpack_color(cfg.body_color));
        if (pl->face_texture_path() != face)
            pl->set_face(face);
        /* A dropped gun is replaced by the secondary one on the server (an
         * empty pistol section: no gun): replace it locally at the release of
         * a shown throw (remote players are shown in the past) */
        if (pl->pistol_section() != cfg.pistol)
            pl->setup_pistol_on_release(cfg.pistol, gs.cur_game_mode.magazines_count);
        pl->tracer_color(unpack_color(cfg.tracer_color));
        pl->group(cfg.group);
    }

    /* Input ticks the server's switch to the secondary gun may lag behind the predicted one */
    static constexpr u32 predicted_switch_confirm_ticks = 30;

    udp_channel     _sock;
    sf::IpAddress   _server_ip;
    u16             _server_port  = 0;
    player_name_t   _name;
    bool            _joined       = false;
    bool            leave_sent    = false;

    game_state*       _gs = nullptr;
    clock::time_point _start_time = clock::now();
    input_source_t    _input_source;
    std::deque<u8>    _sent_inputs;       // the last input::max_window input ticks
    u32               _input_seq     = 0; // seq of the newest input tick
    u32               _last_sent_seq = 0;
    float             _rtt_s         = 0.f;
    bool              _rtt_measured  = false;
    float             _rtt_min_s     = 0.f;
    std::deque<std::pair<double, float>> _rtt_samples;

    history_t    _history;
    u32          _last_ack        = 0;
    std::map<u16, player_config> _configs; // the players' configs by id
    u32          _last_state_tick = 0;
    u64          _corrections     = 0;
    float        _max_correction  = 0.f;
    u64          _weapon_corrections = 0;
    server_clock _clock;
    std::map<player_name_t, snapshot_buffer<remote_player_snapshot>> _remote;
    using knockback_t = knockback_offset<bullet_ref, player::physic_state>;
    std::shared_ptr<player>                                          _ghost;
    std::map<player_name_t, knockback_t>                             _knockbacks;
    std::map<player_name_t, std::optional<u16>>                      _remote_teleports;
    std::map<player_name_t, std::optional<player::physic_state>>     _remote_applied; // the last state shown
    std::optional<u16>                                               _local_teleports;
    double                                                           _last_remote_apply_s = 0.0;
    std::deque<predicted_bullet_t>                                   _predicted;
    float                                                            _max_bullet_correction = 0.f;
    float                                                            _max_gun_correction    = 0.f;
    float                                                            _tick_dt               = 1.f / 60.f;
    std::optional<double>                                            _tick_time_offset; // server time of input tick 0, s
    u32                                                              _tick_time_ack = 0;
    std::optional<double>                                            _shown_time; // server time the remote players are shown at, s
    std::optional<std::pair<u32, player::physic_state>>              _ack_state;  // the server state at the newest ack

    /* Lag-compensated hits predicted on the local player, by bullet id, until the server registers them */
    struct rewound_hit_t {
        u32 seq;         /* the tick the hit is applied in */
        u32 confirm_seq; /* the first acknowledged tick whose state has it */
    };
    std::map<u32, rewound_hit_t> _rewound_hits;

    std::vector<shot_key_t> _own_hit_keys; // own bullets predicted to hit remote players, not confirmed yet
    std::vector<u32>        _own_hit_ids;  // ... and confirmed
    std::set<u32>           _own_bullet_ids; // confirmed own bullets
    float                   _own_lag_s    = 0.f; // the server's lag compensation of the local player's shots
    float                   _own_rewind_s = 0.f;
};

} // namespace dfdh::net
