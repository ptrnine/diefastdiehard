#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
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

    static remote_player_snapshot interpolate(const remote_player_snapshot& a, const remote_player_snapshot& b, float f) {
        auto res   = f < 1.f ? a : b;
        res.ph.pos = lerp(a.ph.pos, b.ph.pos, f);
        res.ph.vel = lerp(a.ph.vel, b.ph.vel, f);
        return res;
    }
};

inline player::physic_state to_physic_state(const state_player& sp) {
    return {{sp.pos_x, sp.pos_y}, {sp.vel_x, sp.vel_y}, sp.lock_y != 0, sp.jumps_left, sp.accel_lf, sp.accel_rf};
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
        glog().info("net client: DFDH_DEBUG_PING is enabled, simulating ~250ms ping (~125ms one-way)");
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

    void reset_correction_stats() {
        _corrections    = 0;
        _max_correction = 0.f;
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
                sample_input(*_gs, timestep);
            });
            gs.sim.add_tick_end_callback("net_client", [this](const physic_simulation&, float) { record_prediction(*_gs); });
        }

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

        apply_remote_snapshots(gs);
        send_input_window();
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
    using clock = std::chrono::steady_clock;

    using history_t = input_history<player_input_state, player::physic_state, vec2f>;

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
        pl->apply_input(in);

        _sent_inputs.push_back(in.to_bits());
        if (_sent_inputs.size() > input::max_window)
            _sent_inputs.pop_front();
        ++_input_seq;

        /* The impulses applied since the previous tick (recoil) belong to this tick */
        _history.push({_input_seq, in, pl->take_pending_impulse(), timestep});
    }

    /* Simulation tick end: remember the predicted state */
    void record_prediction(game_state& gs) {
        auto entry = _history.back();
        auto pl    = local_player(gs);
        if (entry && pl && entry->seq == _input_seq)
            entry->state = pl->get_physic_state();
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
     * on divergence restore it and re-simulate the unacknowledged ticks
     */
    void reconcile(game_state& gs, player& pl, const player::physic_state& server_st, u32 ack_seq) {
        /* Reordered older state, or the same ack again (already reconciled) */
        if (ack_seq < _last_ack || (ack_seq == _last_ack && ack_seq != 0))
            return;
        _last_ack = ack_seq;

        auto predicted = _history.find(ack_seq);
        if (!predicted || diverged(predicted->state, server_st)) {
            auto old_pos = pl.get_position();

            pl.restore_physic_state(server_st);
            pl.replaying(true);
            for (auto& e : _history) {
                if (e.seq <= ack_seq)
                    continue;
                pl.apply_movement_input(e.input);
                pl.apply_impulse(e.extra);
                gs.sim.step_isolated(pl.collision_box(), e.timestep, [&](float dt) { pl.physic_update(gs.sim, dt); });
                e.state = pl.get_physic_state();
            }
            pl.replaying(false);

            auto correction = old_pos - pl.get_position();
            pl.add_render_offset(correction);
            ++_corrections;
            _max_correction = std::max(_max_correction, magnitude(correction));
            glog().detail("net client: prediction correction at tick {}: {} px", ack_seq, magnitude(correction));
        }

        _history.drop_up_to(ack_seq);
    }

    /* Every frame: show remote players at the interpolated server time */
    void apply_remote_snapshots(game_state& gs) {
        if (!_clock.valid())
            return;

        auto t = _clock.render_time(time_s());
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
            pl.restore_physic_state(snap->ph);
            pl.on_left(snap->on_left);

            /*
             * Drive the remote player's pistol locally (muzzle flash, recoil,
             * sounds). The actual bullets arrive via the state broadcast
             */
            if (snap->shooting)
                pl.shot();
            else
                pl.relax();
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
        msg.bits.assign(_sent_inputs.begin(), _sent_inputs.end());
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

        /* States may arrive out of order: only the newest one updates the world state */
        bool newest = st.tick >= _last_state_tick;
        if (newest)
            _last_state_tick = st.tick;

        for (auto& sp : st.players) {
            auto is_local = _joined && sp.name == _name;

            auto found = gs.players.find(sp.name);
            if (found == gs.players.end()) {
                auto pl = gs.player_create(sp.name);
                if (!pl)
                    continue;
                apply_player_config(*pl, sp);
            }
            else if (newest) {
                apply_player_config_if_changed(*found->second, sp);
            }

            auto pl = gs.players.at(sp.name).get();

            /*
             * Every server-known player is authoritative: bullets for it come
             * from the server broadcast, so never spawn them locally (this
             * also covers locally simulated bots that exist on both sides)
             */
            pl->set_server_driven(true);

            if (is_local)
                reconcile(gs, *pl, to_physic_state(sp), st.ack_seq);
            else
                _remote[sp.name].push(double(st.server_time_ms) / 1000.0,
                                      remote_player_snapshot{to_physic_state(sp), sp.on_left != 0, sp.shooting != 0});
        }

        /* Spawn the server-side bullets in the local simulation */
        for (size_t i = 0; i < st.bullets.size(); ++i) {
            auto& b   = st.bullets[i];
            auto& blt = gs.blt_mgr.shot(gs.sim,
                                        {b.pos_x, b.pos_y},
                                        b.hit_mass,
                                        {b.vel_x, b.vel_y},
                                        unpack_color(b.tracer_color),
                                        b.gravity);

            blt.net_id(b.id);
        }

        if (!newest)
            return;

        /*
         * Drop local bullets the server no longer lists as alive: they hit
         * something on the server but may have missed locally because the
         * local simulation diverged slightly
         */
        gs.blt_mgr.remove_not_in(st.alive_bullet_ids);

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
                if (g->id() == gi.id && g->net_synced()) {
                    local = g.get();
                    break;
                }

            if (!local) {
                local = gs.dropped_guns.spawn_net(gi.id, gi.section, {gi.pos_x, gi.pos_y},
                                                  gi.on_left != 0, gi.long_shot != 0);
                glog().info("net client: dropped gun {} synced at ({}, {})", gi.id, gi.pos_x, gi.pos_y);
            }

            local->position({gi.pos_x, gi.pos_y});
            local->velocity({gi.vel_x, gi.vel_y});
            local->sync_angle(gi.angle);
            received_gun_ids.push_back(gi.id);
        }

        for (auto it = gs.dropped_guns.guns().begin(); it != gs.dropped_guns.guns().end();) {
            auto& g = *it;
            bool keep = !g->net_synced() ||
                        std::find(received_gun_ids.begin(), received_gun_ids.end(), g->id()) != received_gun_ids.end();
            it = keep ? std::next(it) : gs.dropped_guns.guns().erase(it);
        }
    }

    static void apply_player_config(player& pl, const state_player& sp) {
        pl.set_body("player/body" + std::to_string(sp.body_id) + ".png", unpack_color(sp.body_color));
        pl.set_face("player/face" + std::to_string(sp.face_id) + ".png");
        /* An empty pistol section means the server-side player has no gun
         * (e.g. dropped it), so clear it locally as well */
        if (sp.pistol.empty())
            pl.clear_pistol();
        else
            pl.setup_pistol(sp.pistol);
        pl.tracer_color(unpack_color(sp.tracer_color));
    }

    void apply_player_config_if_changed(player& pl, const state_player& sp) {
        if (pl.pistol_section() == sp.pistol &&
            pl.face_texture_path() == ("player/face" + std::to_string(sp.face_id) + ".png") &&
            pl.body_texture_path() == ("player/body" + std::to_string(sp.body_id) + ".png") &&
            pl.body_color() == unpack_color(sp.body_color) &&
            pl.tracer_color() == unpack_color(sp.tracer_color))
            return;

        apply_player_config(pl, sp);
    }

    udp_channel     _sock;
    sf::IpAddress   _server_ip;
    u16             _server_port  = 0;
    player_name_t   _name;
    bool            _joined       = false;
    bool            leave_sent    = false;

    game_state*       _gs = nullptr;
    clock::time_point _start_time = clock::now();
    input_source_t    _input_source;
    std::deque<u16>   _sent_inputs;       // the last input::max_window input ticks
    u32               _input_seq     = 0; // seq of the newest input tick
    u32               _last_sent_seq = 0;
    float             _rtt_s         = 0.f;
    bool              _rtt_measured  = false;

    history_t    _history;
    u32          _last_ack        = 0;
    u32          _last_state_tick = 0;
    u64          _corrections     = 0;
    float        _max_correction  = 0.f;
    server_clock _clock;
    std::map<player_name_t, snapshot_buffer<remote_player_snapshot>> _remote;
};

} // namespace dfdh::net
