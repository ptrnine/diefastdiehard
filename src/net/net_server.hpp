#pragma once

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <SFML/Network/IpAddress.hpp>


#include "base/types.hpp"
#include "base/log.hpp"
#include "net/net_protocol.hpp"
#include "net/net_sync.hpp"
#include "net/net_udp.hpp"
#include "game_state.hpp"

namespace dfdh::net {

inline u32 pack_color(const sf::Color& c) {
    return (u32(c.r) << 24) | (u32(c.g) << 16) | (u32(c.b) << 8) | u32(c.a);
}

inline sf::Color unpack_color(u32 v) {
    return sf::Color(u8((v >> 24) & 0xff), u8((v >> 16) & 0xff), u8((v >> 8) & 0xff), u8(v & 0xff));
}

inline game_mode_info to_game_mode_info(const game_mode& gm) {
    return {gm.section, gm.bullet_gravity, gm.crate_drop_period, gm.crate_limit, gm.magazines_count, u8(gm.score_system), gm.start_lives};
}

/**
 * Multiplayer server.
 *
 * Call update(game_state) once per game frame (from the main thread).
 */
class net_server {
public:
    using clock = game_clock;

    /*
     * Input jitter buffer: after running dry, the queue refills to the
     * client's target number of ticks before it is consumed again (absorbs
     * the arrival jitter, each starving tick is a tick the client did not
     * predict). Every queued tick adds a tick of input latency, and the
     * client's shots are forwarded by the wait. The target adapts to the
     * measured arrival jitter (adaptive_target), this is the target until
     * there are input_jitter_warmup_s of measurements
     */
    static constexpr size_t target_queued_inputs  = physic_simulation::tick_rate / 20;     /* 50 ms */
    static constexpr size_t max_target_inputs     = physic_simulation::tick_rate * 2 / 15; /* ~133 ms */
    static constexpr double input_jitter_warmup_s = 1.0;
    /* Queued input ticks above max(this, target + excess) are merged to bound the input latency */
    static constexpr size_t max_queued_inputs    = max_target_inputs;
    static constexpr size_t max_queued_excess    = physic_simulation::tick_rate / 15; /* ~67 ms */
    /*
     * A queue deeper than the target for this many ticks (a burst after a
     * client hitch, a target that dropped) merges its oldest tick: once per
     * window, until it is back at the target. Only a tick the next one
     * repeats (the same held bits, no edges) is merged: merging a trigger
     * pull would fire the shot a tick later, seeded by another input tick,
     * so it would not match the client's predicted bullets
     */
    static constexpr u32 drain_window_ticks = physic_simulation::tick_rate / 2; /* 0.5 s: drains ~17 ms of queue a second */

    /**
     * Ticks to buffer for an input arrival jitter of jitter_s: a tick is
     * consumed (target - 1) ticks after the earliest arrival, so up to that
     * late it is on time
     */
    [[nodiscard]]
    static size_t adaptive_target(double jitter_s, double tick_s) {
        auto late_ticks = std::max(std::ceil(jitter_s / tick_s - 0.1), 0.0);
        return std::clamp<size_t>(size_t(late_ticks) + 1, 1, max_target_inputs);
    }
    /* Without inputs for this long the client's player gets a neutral input */
    static constexpr auto input_timeout = std::chrono::milliseconds(500);
    /*
     * Player configs and the game mode are sent to a client during this
     * long after it joined or after they changed, and to everyone every
     * config_refresh broadcasts (lost datagrams cost nothing)
     */
    static constexpr auto config_resend  = std::chrono::seconds(1);
    static constexpr u32  config_refresh = physic_simulation::tick_rate / 2; /* 0.5 s of ticks */
    /* A state is broadcast once per simulation tick, or this often without ticks (paused game) */
    static constexpr auto idle_broadcast = std::chrono::milliseconds(100);

    struct client_t {
        sf::IpAddress    address      = sf::IpAddress::Any;
        u16              port         = 0;
        player_name_t    player_name;
        bool             joined       = false;
        game_clock::time_point joined_time;

        /* Received, not yet applied input ticks (seq -> bits), applied one per tick in seq order */
        struct queued_input {
            u8                    bits;
            clock::time_point     received; // first arrival (the wait in the queue is part of the shooter's lag)
            std::optional<double> view_s;   // the server time the client was showing at the tick
        };
        std::map<u32, queued_input> inputs;
        /*
         * Queued seqs merged away (-> their trigger): a newer window must not
         * queue them again. The weapon runs their ticks with their trigger
         */
        std::map<u32, bool>         merged_seqs;
        float                       input_wait_s = 0.f; // time the last applied input tick waited in the queue
        /*
         * Input margin (sent in STATE, the client aligns its ticks with it):
         * how long before the tick that applied it the input tick ack_seq
         * arrived; negative if the queue ran dry waiting for it
         */
        std::optional<float>             input_margin_s;
        std::optional<clock::time_point> starved_since; // the first dry tick waiting for the next seq
        std::optional<float>             late_s;        // how long after that the next seq arrived
        u32                next_seq      = 0; // first seq not applied yet, 0 = none received yet
        u32                ack_seq       = 0; // last applied seq
        u8                 held_bits     = 0; // held part of the last applied input
        u8                 pending_edges = 0; // edges of inputs merged away, applied with the next one
        bool               buffering     = true; // refilling the jitter buffer
        clock::time_point  last_input_time;
        server_clock       input_jitter;               // input arrivals: offset = arrival - client send time
        size_t             target_inputs = target_queued_inputs;
        size_t             min_depth     = std::numeric_limits<size_t>::max(); // queue depth in the drain window
        u32                depth_ticks   = 0;

        /* Newest input's client time, echoed back for the client's RTT measurement */
        u32               echo_seq            = 0;
        u32               echo_client_time_ms = 0;
        clock::time_point echo_recv_time;
        float             rtt_s    = 0.f;

        u32 lobby_session = 0; // lobby_request::session
        u32 lobby_seq     = 0; // last applied lobby_request of the session

        [[nodiscard]]
        static std::string key(const sf::IpAddress& addr) {
            return addr.toString();
        }
    };

    net_server(const net_server&) = delete;
    net_server& operator=(const net_server&) = delete;

    ~net_server() {
        if (_gs) {
            _gs->sim.remove_tick_begin_callback("net_server");
            _gs->sim.remove_tick_end_callback("net_server");
        }
    }

    net_server(u16 port): _port(port) {
        if (!_sock.bind(port))
            glog().error("net server: can't bind port {}", port);
        else {
            glog().info("net server: listening on udp port {}", port);
#if defined(DFDH_DEBUG_PING)
            glog().info("net server: DFDH_DEBUG_PING is enabled, delaying every sent and received datagram by 80..170ms (~500ms ping between two such ends)");
#endif
        }
    }

    [[nodiscard]]
    bool is_ready() const {
        return _sock.is_open();
    }

    u16 port() const {
        return _port;
    }

    size_t client_count() const {
        return _clients.size();
    }

    /** Fixed input jitter buffer of every client, in ticks (1..max_target_inputs); empty: adaptive */
    void input_buffer(std::optional<size_t> ticks) {
        if (ticks)
            ticks = std::clamp<size_t>(*ticks, 1, max_target_inputs);
        _input_buffer = ticks;
    }

    [[nodiscard]]
    std::optional<size_t> input_buffer() const {
        return _input_buffer;
    }

    struct input_stats_t {
        player_name_t player;
        size_t        target;   /* ticks */
        double        jitter_s; /* input arrival jitter */
        float         wait_s;   /* the last applied input tick's wait in the queue */
        std::optional<float> margin_s; /* how early it arrived (client_t::input_margin_s) */
    };

    [[nodiscard]]
    std::vector<input_stats_t> input_stats() const {
        std::vector<input_stats_t> res;
        for (auto& [_, c] : _clients)
            if (c.joined)
                res.push_back({c.player_name, c.target_inputs, c.input_jitter.jitter(), c.input_wait_s, c.input_margin_s});
        return res;
    }

    [[nodiscard]]
    client_t* find_client(const sf::IpAddress& addr) {
        auto found = _clients.find(client_t::key(addr));
        return found == _clients.end() ? nullptr : &found->second;
    }

    /**
     * The game_state must outlive the server (the server hooks into its
     * simulation ticks).
     *
     * The network I/O of the game is at the tick boundaries: every tick
     * begins with reading the inputs (one that arrived during the previous
     * frame is applied in this tick, not in the next frame's), and the state
     * is sent right after the tick. The frame handles everything else (joins,
     * the lobby) and broadcasts while nothing ticks (lobby, paused game)
     */
    void update(game_state& gs) {
        if (!_sock.is_open())
            return;

        if (!_gs) {
            _gs = &gs;
            gs.sim.add_tick_begin_callback("net_server", [this](const physic_simulation&, float) {
                receive_inputs();
                apply_inputs(*_gs);
            });
            /* After the game's tick end callbacks (shots, history): the state of the finished tick */
            gs.sim.add_tick_end_callback(
                "net_server", [this](const physic_simulation&, float) { tick_broadcast(*_gs, clock::now()); }, 1000);
        }

        for (auto& datagram : _deferred)
            handle_msg(gs, datagram);
        _deferred.clear();
        for (auto& datagram : _sock.recv(std::chrono::milliseconds(2)))
            handle_msg(gs, datagram);

        /* Nothing ticks (lobby, paused game): the state is sent anyway, this often */
        auto now = clock::now();
        if (gs.sim.tick() != _last_broadcast_tick || now - _last_broadcast_time >= idle_broadcast)
            tick_broadcast(gs, now);
    }

private:
    /*
     * Tick begin: read the socket without waiting. Inputs are queued now,
     * other messages wait for the frame (they create and delete players)
     */
    void receive_inputs() {
        for (auto& datagram : _sock.recv(std::chrono::milliseconds(0))) {
            net_msg msg;
            try {
                msg = decode_msg({datagram.data.data(), datagram.data.size()});
            }
            catch (const std::exception&) {
                _deferred.push_back(std::move(datagram)); /* the frame logs it */
                continue;
            }
            if (msg.type == msg_type::input)
                handle_input(datagram.address, datagram.arrival, msg);
            else
                _deferred.push_back(std::move(datagram));
        }
    }

    /* The state after a tick (or of the idle frame): once per tick, it does not change in between */
    void tick_broadcast(game_state& gs, clock::time_point now) {
        /* Bullets spawned since the last broadcast (ids grow: each one is taken once) */
        for (auto& be : gs.cur_frame_bullets)
            if (be.id > _last_pending_bullet_id) {
                _pending_bullets.push_back(be);
                _last_pending_bullet_id = be.id;
            }

        if (_clients.empty()) {
            _pending_bullets.clear();
            return;
        }

        _last_broadcast_tick = gs.sim.tick();
        _last_broadcast_time = now;
        broadcast(gs, now);
    }

    /* Ids of the players on the wire (never reused) */
    u16 player_id(const player_name_t& name) {
        auto [it, inserted] = _player_ids.try_emplace(name, 0);
        if (inserted)
            it->second = ++_next_player_id;
        return it->second;
    }

    static player_config make_player_config(u16 id, player& pl) {
        return {id,
                pl.name(),
                pl.pistol_section(),
                u16(player_configurator::texture_path_to_index(pl.face_texture_path())),
                u16(player_configurator::texture_path_to_index(pl.body_texture_path())),
                u16(player_configurator::texture_path_to_index(pl.head_texture_path())),
                i16(player_configurator::hat_texture_path_to_id(pl.hat_texture_path())),
                pack_color(pl.body_color()),
                pack_color(pl.tracer_color()),
                i16(pl.get_group())};
    }

    static state_player make_state_player(u16 id, player& pl, bool ready) {
        /*
         * The raw velocity: on a platform its vertical part is hidden from the
         * physics but still drives the weapon bobbing, and the client's
         * prediction keeps it the same way (one gravity step at a tick end)
         */
        auto ph  = pl.get_physic_state();
        auto vel = ph.vel;
        u8   flags = 0;
        if (ph.lock_y)
            flags |= state_player::flag_lock_y;
        if (pl.get_on_left())
            flags |= state_player::flag_on_left;
        if (pl.shotting())
            flags |= state_player::flag_shooting;
        if (pl.throwing())
            flags |= state_player::flag_throwing;
        if (pl.long_shot_enabled())
            flags |= state_player::flag_long_shot;
        if (ready)
            flags |= state_player::flag_ready;

        return {id,
                ph.pos.x,
                ph.pos.y,
                vel.x,
                vel.y,
                flags,
                u8(ph.jumps_left),
                pack_unit(ph.accel_left_f),
                pack_unit(ph.accel_right_f),
                pl.teleports(),
                i16(std::clamp(pl.score(), i32(INT16_MIN), i32(INT16_MAX))),
                pl.recent_hit_ids()};
    }

    static std::optional<weapon_sync> make_weapon_sync(player& pl) {
        auto& gun = pl.get_gun();
        if (!gun)
            return {};
        auto ws = gun.get_sync_state();
        return weapon_sync{u16(ws.ammo),
                           u16(std::min<u32>(ws.reserve, weapon_sync::unlimited_reserve)),
                           ws.anim,
                           ws.anim_time,
                           ws.since_shot,
                           u8((ws.on_reload ? weapon_sync::flag_reloading : 0) | (ws.on_shot ? weapon_sync::flag_trigger : 0)),
                           u16(ws.shot_index),
                           ws.seed_base};
    }

    /* Group the pending bullets by shot: the pellets of a shot share everything but the velocity */
    void add_shots(state& st) {
        for (auto& be : _pending_bullets) {
            auto owner   = player_id(be.owner);
            auto forward = shot_info::pack_forward(be.forward);
            auto rewind  = shot_info::pack_forward(be.rewind);
            auto flags   = u8(be.gravity ? shot_info::flag_gravity : 0);

            auto* last = st.shots.empty() ? nullptr : &st.shots.back();
            bool  same = last && last->owner == owner && last->pull_seq == be.key.pull_seq &&
                        last->shot_index == u16(be.key.shot_index) &&
                        last->first_id + u32(last->pellets.size()) == be.id &&
                        last->first_pellet + last->pellets.size() == be.key.pellet && exactly_equal(last->start_x, be.start.x) &&
                        exactly_equal(last->start_y, be.start.y) && last->forward == forward && last->rewind == rewind &&
                        exactly_equal(last->hit_mass, be.hit_mass) &&
                        last->flags == flags;
            if (!same)
                st.shots.push_back(shot_info{owner,
                                             be.id,
                                             be.key.pull_seq,
                                             u16(be.key.shot_index),
                                             u8(be.key.pellet),
                                             be.start.x,
                                             be.start.y,
                                             forward,
                                             rewind,
                                             be.hit_mass,
                                             flags,
                                             {}});
            st.shots.back().pellets.push_back({be.muzzle_velocity.x, be.muzzle_velocity.y});
        }
        _pending_bullets.clear();
    }

    void broadcast(game_state& gs, clock::time_point now) {
        net::state st;
        st.tick        = ++_broadcast_seq;
        /* Time of the simulated state (the last tick), not of the broadcast */
        st.server_time = u32(std::llround(std::chrono::duration<double>(gs.sim.current_update_time() - _start_time).count() *
                                          net::state::server_time_scale));

        /* Players and their configs (remembering when a config changed) */
        std::map<u16, known_config> configs;
        for (auto& [name, pl] : gs.players) {
            auto id  = player_id(name);
            auto cfg = make_player_config(id, *pl);
            auto old = _configs.find(id);
            configs[id] = old != _configs.end() && old->second.cfg == cfg ? old->second : known_config{cfg, now};
            st.players.push_back(make_state_player(id, *pl, !pl->net_controlled() || gs.lobby.is_ready(name)));
        }
        _configs = std::move(configs);

        /* The game mode (remembering when it changed) */
        if (_game_mode.mode != gs.cur_game_mode)
            _game_mode = known_game_mode{gs.cur_game_mode, now};

        /* The lobby: the choices, or what is played (remembering when it changed) */
        lobby_info lobby{u8(gs.lobby.active ? 1 : 0),
                         gs.lobby.active || !gs.cur_level ? gs.lobby.level : gs.cur_level->section_name(),
                         gs.lobby.active ? gs.lobby.mode : gs.cur_game_mode.section};
        if (_lobby.info != lobby)
            _lobby = known_lobby{lobby, now};

        /*
         * Every bullet spawned since the last broadcast (network players, the
         * host's local player and bots). The client disables local bullet
         * spawning for every server-known player, so there are no duplicates
         */
        add_shots(st);

        /*
         * The set of all live bullet ids: clients drop their local bullets
         * that no longer exist on the server (e.g. hit a player whose
         * position diverged locally)
         */
        std::vector<u32> alive;
        for (auto& b : gs.blt_mgr.bullets())
            if (b.net_id() != 0)
                alive.push_back(b.net_id());
        st.alive_bullets = alive_set::from_ids(alive);

        for (auto& c : gs.crates) {
            auto pos = c->get_position();
            auto vel = c->get_velocity();
            st.crates.push_back(crate_info{c->id(), pos.x, pos.y, vel.x, vel.y, c->landed() ? u8(1) : u8(0)});
        }

        /* Dropped guns (server-authoritative physics) */
        for (auto& g : gs.dropped_guns.guns()) {
            auto pos = g->get_position();
            auto vel = g->get_velocity();
            st.guns.push_back(gun_info{g->id(),
                                       g->section(),
                                       pos.x,
                                       pos.y,
                                       vel.x,
                                       vel.y,
                                       g->angle(),
                                       static_cast<u8>(g->on_left()),
                                       static_cast<u8>(g->long_shot()),
                                       g->owner().empty() ? u16(0) : player_id(g->owner()),
                                       u16(std::min(g->age() * 1000.f, 65535.f))});
        }

        bool refresh = st.tick % config_refresh == 0;
        for (auto& [key, client] : _clients) {
            if (!client.joined)
                continue;

            st.ack_seq             = client.ack_seq;
            st.input_margin        = client.input_margin_s ? state::pack_input_margin(*client.input_margin_s)
                                                           : state::no_input_margin;
            st.echo_client_time_ms = client.echo_client_time_ms;
            st.echo_hold_ms        = u16(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      clock::now() - client.echo_recv_time).count());

            auto own      = gs.players.find(client.player_name);
            st.own_weapon = own != gs.players.end() ? make_weapon_sync(*own->second) : std::nullopt;

            /* The configs the client may not know */
            bool new_client = now - client.joined_time < config_resend;
            st.configs.clear();
            for (auto& [_, kc] : _configs)
                if (refresh || new_client || now - kc.changed < config_resend)
                    st.configs.push_back(kc.cfg);

            /* The game mode, if the client may not know it */
            st.mode.reset();
            if (refresh || new_client || now - _game_mode.changed < config_resend)
                st.mode = to_game_mode_info(_game_mode.mode);

            /* The lobby, the same way */
            st.lobby.reset();
            if (refresh || new_client || now - _lobby.changed < config_resend)
                st.lobby = _lobby.info;

            _sock.send_to(client.address, client.port, encode_msg(msg_type::state, st));
        }
    }

    /**
     * Simulation tick: apply the next queued input tick of every client.
     * Inputs are applied exactly once and in seq order, so a late or
     * duplicated datagram can never bring back an old input state
     */
    void apply_inputs(game_state& gs) {
        auto now = clock::now(); /* the inputs were read now: one arriving later is too late for this tick */
        _tick_s  = std::chrono::duration<double>(gs.sim.tick_period()).count();

        for (auto& [key, client] : _clients) {
            if (!client.joined)
                continue;

            auto found = gs.players.find(client.player_name);
            if (found == gs.players.end() || !found->second)
                continue;

            auto& q = client.inputs;

            if (_input_buffer)
                client.target_inputs = *_input_buffer;
            else if (client.input_jitter.span() >= input_jitter_warmup_s)
                client.target_inputs = adaptive_target(client.input_jitter.jitter(), _tick_s);
            else
                client.target_inputs = target_queued_inputs;

            auto merge = [&](auto it) {
                client.merged_seqs.emplace(it->first, (it->second.bits & in_bit_shot) != 0);
                q.erase(it);
            };

            /*
             * The tick repeats the previous one (the same input, no edges):
             * merging it away loses a tick of time only, every press and
             * release stays on its tick. by_next: the next tick is the same
             * input too
             */
            auto repeated = [&](auto it, bool by_next) {
                auto bits = it->second.bits;
                auto next = std::next(it);
                bool contiguous = next != q.end() && next->first == it->first + 1;
                if ((bits & in_edge_bits) != 0 || (by_next && !(contiguous && next->second.bits == bits)))
                    return false;
                if (it == q.begin())
                    return client.ack_seq != 0 && it->first == client.ack_seq + 1 && client.pending_edges == 0 &&
                           bits == client.held_bits;
                auto prev = std::prev(it);
                return prev->first + 1 == it->first && prev->second.bits == bits;
            };

            /*
             * Too many queued ticks (client clock runs faster, burst after a
             * stall): merge the oldest repeated one. None repeated: merge the
             * oldest, its edges carry over to the next one. The weapon runs
             * the merged ticks with their own trigger (merged_seqs), so the
             * shots and reloads stay on the client's predicted ticks
             */
            while (q.size() > std::max(max_queued_inputs, client.target_inputs + max_queued_excess)) {
                auto it = q.end();
                for (bool by_next : {true, false})
                    for (auto i = q.begin(); it == q.end() && i != q.end(); ++i)
                        if (repeated(i, by_next))
                            it = i;
                if (it == q.end()) {
                    it = q.begin();
                    client.pending_edges |= u8(it->second.bits & in_edge_bits);
                }
                merge(it);
            }

            if (q.empty())
                client.buffering = true;
            else if (q.size() >= client.target_inputs)
                client.buffering = false;

            /*
             * Deeper than the target for a whole window: drain a repeated tick
             * (the client corrects its prediction); none yet: try next tick
             */
            if (client.buffering) {
                client.min_depth   = std::numeric_limits<size_t>::max();
                client.depth_ticks = 0;
            }
            else {
                client.min_depth = std::min(client.min_depth, q.size());
                if (++client.depth_ticks >= drain_window_ticks) {
                    if (client.min_depth <= client.target_inputs || repeated(q.begin(), true)) {
                        if (client.min_depth > client.target_inputs)
                            merge(q.begin());
                        client.min_depth   = std::numeric_limits<size_t>::max();
                        client.depth_ticks = 0;
                    }
                }
            }

            u8  bits;
            std::optional<double> view_s;
            std::map<u32, bool>   merged_triggers;
            if (!client.buffering) {
                /* Missing seqs before the head are lost for good (they would be in every newer window) */
                auto head            = q.begin();
                view_s               = head->second.view_s;
                bits                 = head->second.bits | client.pending_edges;
                client.ack_seq       = head->first;
                client.next_seq      = head->first + 1;
                client.held_bits     = u8(head->second.bits & ~in_edge_bits);
                client.pending_edges = 0;
                auto merged_end = client.merged_seqs.lower_bound(client.next_seq);
                merged_triggers = std::map<u32, bool>(client.merged_seqs.begin(), merged_end);
                client.merged_seqs.erase(client.merged_seqs.begin(), merged_end);
                /* Measured at the tick's time (catch-up ticks run late) */
                client.input_wait_s =
                    std::max(std::chrono::duration<float>(gs.sim.current_update_time() - head->second.received).count(), 0.f);
                client.input_margin_s =
                    client.late_s ? -*client.late_s : std::chrono::duration<float>(now - head->second.received).count();
                client.late_s.reset();
                client.starved_since.reset();
                q.erase(head);
            }
            else if (q.empty() && now - client.last_input_time > input_timeout) {
                /* Lost client: release everything */
                bits             = 0;
                client.held_bits = 0;
                client.starved_since.reset();
            }
            else {
                /* Starving: hold the last state, no edges, without consuming a seq */
                bits = client.held_bits;
                if (q.empty() && client.ack_seq != 0 && !client.starved_since && !client.late_s)
                    client.starved_since = now;
            }

            found->second->last_input_seq(client.ack_seq);
            found->second->merged_triggers(std::move(merged_triggers));
            found->second->apply_input(player_input_state::from_bits(bits));

            /* A starving tick keeps the compensation of the last applied one */
            if (!client.buffering)
                found->second->lag_compensation(lag_compensation_of(client, gs, view_s));
        }
    }

    /**
     * Lag compensation of a client's tick that showed the server time view_s.
     * A shot is forwarded by the time since the client fired it (lag: one way
     * trip + the measured wait in the input queue, an estimate). At flight f
     * the client saw it together with the players at view + f, so they are
     * tested rewind = (tick time - view) - lag ago: exact, whatever the lag
     * estimate. The lag is whole ticks: the server sweeps the bullet's path
     * tick by tick, ending at the flights the client sweeps its prediction
     * at (net_client::own_bullet_hit: view + n ticks). Without a view
     * (nothing shown yet) the view is estimated: the client's view of the
     * server lags by the one way trip and about the default interpolation
     */
    [[nodiscard]]
    player::lag_compensation_t lag_compensation_of(const client_t& client, const game_state& gs,
                                                   std::optional<double> view_s) const {
        auto one_way = client.rtt_s * 0.5f;
        auto lag     = one_way + client.input_wait_s;
        if (!view_s)
            return {lag, one_way + float(server_clock::default_interval_s + server_clock::interp_margin_s)};

        auto tick_s = std::chrono::duration<double>(gs.sim.current_update_time() - _start_time).count();
        auto behind = float(std::clamp(tick_s - *view_s, 0.0, double(game_state::max_lag_compensation)));
        auto tick   = float(_tick_s);
        lag         = std::floor(std::min(lag, behind) / tick + 1e-3f) * tick;
        return {lag, std::max(behind - lag, 0.f)};
    }

    void handle_msg(game_state& gs, const received_datagram& datagram) {
        net_msg msg;
        try {
            msg = decode_msg({datagram.data.data(), datagram.data.size()});
        }
        catch (const std::exception& e) {
            glog().warn("net server: bad datagram from {}:{}: {}",
                    datagram.address.toString(), datagram.port, e.what());
            return;
        }

        switch (msg.type) {
        case msg_type::hello: handle_hello(gs, datagram.address, datagram.port, msg); break;
        case msg_type::input: handle_input(datagram.address, datagram.arrival, msg); break;
        case msg_type::leave: handle_leave(gs, datagram.address); break;
        case msg_type::lobby: handle_lobby(gs, datagram.address, msg); break;
        default: break;
        }
    }

    void handle_hello(game_state& gs, const sf::IpAddress& addr, u16 port, const net_msg& msg) {
        hello h;
        if (!try_read_msg(msg.payload, h)) {
            glog().warn("net server: bad HELLO from {}:{}", addr.toString(), port);
            return;
        }

        if (h.version != protocol_version) {
            glog().error("net server: client {}:{} uses protocol version {}, the server uses {}",
                         addr.toString(), port, h.version, protocol_version);
            auto data = encode_msg(msg_type::error, error{"protocol version mismatch"});
            _sock.send_to(addr, port, data);
            return;
        }

        if (h.name.empty()) {
            glog().error("net server: HELLO with empty player name from {}:{}", addr.toString(), port);
            auto data = encode_msg(msg_type::error, error{"empty player name"});
            _sock.send_to(addr, port, data);
            return;
        }

        /* One client record per address */
        auto& client = _clients[client_t::key(addr)];

        /* Reject duplicate player names from other clients */
        if (client.player_name != h.name) {
            auto found = gs.players.find(h.name);
            if (found != gs.players.end()) {
                glog().error("net server: player {} already exists (hello from {}:{} as '{}')",
                        h.name, addr.toString(), port, h.name);
                auto data = encode_msg(msg_type::error, error{"player already exists"});
                _sock.send_to(addr, port, data);
                return;
            }
        }

        auto was_new = !client.joined;
        auto old_name = client.player_name;
        auto renamed  = !was_new && client.player_name != h.name;
        client.address     = addr;
        client.port        = port;
        client.player_name = h.name;
        client.joined      = true;
        client.joined_time = clock::now();
        client.inputs.clear();
        client.merged_seqs.clear();
        client.next_seq      = 0;
        client.held_bits     = 0;
        client.pending_edges = 0;
        client.buffering     = true;
        client.echo_seq      = 0;
        client.input_jitter.reset();
        client.input_margin_s.reset();
        client.starved_since.reset();
        client.late_s.reset();

        /* Create or reconfigure the player */
        player* pl = nullptr;
        auto found = gs.players.find(h.name);
        if (found != gs.players.end())
            pl = found->second.get();
        else
            pl = gs.player_create(h.name);

        if (!pl) {
            glog().error("net server: can't create player {}", h.name);
            client.joined = false;
            return;
        }

        if (renamed && !old_name.empty()) {
            auto old = gs.players.find(old_name);
            gs.lobby.ready.erase(old_name);
            if (old != gs.players.end()) {
                gs.players.erase(old);
                glog().info("net server: player {} deleted (renamed to {})", old_name, h.name);
            }
        }

        apply_player_config(*pl, h, gs.cur_game_mode.magazines_count);
        pl->set_net_controlled(true);
        gs.player_spawn(pl);

        if (was_new)
            glog().info("net server: client {}:{} joined as player {}", addr.toString(), port, h.name);

        auto data = encode_msg(msg_type::welcome, welcome{h.name});
        _sock.send_to(addr, port, data);
    }

    void handle_input(const sf::IpAddress& addr, clock::time_point arrival, const net_msg& msg) {
        auto client = find_client(addr);
        if (!client || !client->joined)
            return;

        input in;
        if (!try_read_msg(msg.payload, in) || in.bits.empty())
            return;

        auto now = arrival;

        /*
         * Arrival jitter (input_jitter): restarts after a pause (the client
         * sends nothing while it doesn't tick). Against the client's send
         * time, not seq * tick: the client slews its tick rate to align its
         * inputs (net_client::align_ticks), that is not jitter
         */
        if (now - client->last_input_time > input_timeout)
            client->input_jitter.reset();
        client->input_jitter.on_state(std::chrono::duration<double>(now.time_since_epoch()).count(),
                                      double(in.client_time_ms) / 1000.0);

        /* Newest input: RTT echo and lag compensation parameters */
        if (in.last_seq > client->echo_seq) {
            client->echo_seq            = in.last_seq;
            client->echo_client_time_ms = in.client_time_ms;
            client->echo_recv_time      = now;
            client->rtt_s               = float(in.rtt_ms) / 1000.f;
            client->last_input_time     = now;
        }

        if (client->next_seq == 0)
            client->next_seq = in.first_seq();

        /* The queue ran dry waiting for it: it was this late */
        if (client->starved_since && in.last_seq >= client->next_seq) {
            client->late_s        = std::chrono::duration<float>(now - *client->starved_since).count();
            client->starved_since.reset();
        }

        for (u32 i = 0; i < in.bits.size(); ++i) {
            auto seq = in.first_seq() + i;
            if (seq >= client->next_seq && !client->merged_seqs.contains(seq))
                client->inputs.emplace(seq, client_t::queued_input{in.bits[i], now, in.view_s(i)});
        }
    }

    void handle_lobby(game_state& gs, const sf::IpAddress& addr, const net_msg& msg) {
        auto client = find_client(addr);
        if (!client || !client->joined || !gs.lobby.active)
            return;

        lobby_request req;
        if (!try_read_msg(msg.payload, req))
            return;
        if (req.session != client->lobby_session) {
            client->lobby_session = req.session;
            client->lobby_seq     = 0;
        }
        if (req.seq <= client->lobby_seq)
            return;
        client->lobby_seq = req.seq;

        gs.lobby_player_group(client->player_name, std::clamp(int(req.group), -1, lobby_state::max_group));
        gs.lobby.ready[client->player_name] = req.ready != 0;
    }

    void handle_leave(game_state& gs, const sf::IpAddress& addr) {
        auto client = find_client(addr);
        if (!client)
            return;

        gs.lobby.ready.erase(client->player_name);

        if (gs.players.contains(client->player_name)) {
            gs.players.erase(gs.players.find(client->player_name));
            glog().info("net server: player {} deleted (client left)", client->player_name);
        }

        _clients.erase(client_t::key(addr));
        glog().info("net server: client {} left", addr.toString());
    }

    static void apply_player_config(player& pl, const hello& h, u32 magazines_count) {
        pl.set_body("player/body" + std::to_string(h.body_id) + ".png", unpack_color(h.body_color));
        pl.set_head("player/head" + std::to_string(h.head_id) + ".png");
        pl.set_hat(player_configurator::hat_texture_path(h.hat_id));
        pl.set_face("player/face" + std::to_string(h.face_id) + ".png");
        /* Only a secondary gun can be configured */
        if (weapon::is_secondary_section(h.pistol))
            pl.setup_pistol(h.pistol, magazines_count);
        else if (!h.pistol.empty())
            pl.setup_pistol(std::string(weapon::default_secondary), magazines_count);
        pl.tracer_color(unpack_color(h.tracer_color));
    }

public:
    /* For tests: the channel (e.g. to simulate latency) */
    udp_channel& channel() {
        return _sock;
    }

private:
    udp_channel                  _sock;
    u16                          _port;
    game_state*                  _gs             = nullptr;
    u32                          _broadcast_seq  = 0;
    clock::time_point            _start_time     = clock::now();
    std::map<std::string, client_t> _clients;
    std::vector<received_datagram>  _deferred; // read at a tick begin, handled by the frame (not inputs)
    std::optional<size_t>        _input_buffer; // fixed input jitter buffer target (ticks), adaptive if empty
    double                       _tick_s = physic_simulation::tick_s;

    u64                                     _last_broadcast_tick = 0;
    clock::time_point                       _last_broadcast_time;
    std::vector<game_state::bullet_event_t> _pending_bullets;
    u32                                     _last_pending_bullet_id = 0;

    struct known_config {
        player_config     cfg;
        clock::time_point changed; // since when the config is like this
    };
    std::map<player_name_t, u16> _player_ids;
    u16                          _next_player_id = 0;
    std::map<u16, known_config>  _configs;

    struct known_game_mode {
        game_mode         mode;
        clock::time_point changed; // since when the game mode is like this
    };
    known_game_mode _game_mode;

    struct known_lobby {
        lobby_info        info;
        clock::time_point changed;
    };
    known_lobby _lobby;
};

} // namespace dfdh::net
