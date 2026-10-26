#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include <SFML/Network/IpAddress.hpp>


#include "base/types.hpp"
#include "base/log.hpp"
#include "net/net_protocol.hpp"
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
    return {gm.section, gm.bullet_gravity, gm.crate_drop_period, gm.magazines_count, u8(gm.score_system), gm.start_lives};
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
     * Input jitter buffer: after running dry, the queue refills to this many
     * ticks before it is consumed again (absorbs the arrival jitter, each
     * starving tick is a tick the client did not predict)
     */
    static constexpr size_t target_queued_inputs = 3;
    /* Queued input ticks above this are merged to bound the input latency */
    static constexpr size_t max_queued_inputs = 8;
    /* Without inputs for this long the client's player gets a neutral input */
    static constexpr auto input_timeout = std::chrono::milliseconds(500);
    /*
     * Player configs and the game mode are sent to a client during this
     * long after it joined or after they changed, and to everyone every
     * config_refresh broadcasts (lost datagrams cost nothing)
     */
    static constexpr auto config_resend  = std::chrono::seconds(1);
    static constexpr u32  config_refresh = 30;
    /* A state is broadcast once per simulation tick, or this often without ticks (paused game) */
    static constexpr auto idle_broadcast = std::chrono::milliseconds(100);

    struct client_t {
        sf::IpAddress    address;
        u16              port         = 0;
        player_name_t    player_name;
        bool             joined       = false;
        game_clock::time_point joined_time;

        /* Received, not yet applied input ticks (seq -> bits), applied one per tick in seq order */
        struct queued_input {
            u8                bits;
            clock::time_point received; // first arrival (the wait in the queue is part of the shooter's lag)
        };
        std::map<u32, queued_input> inputs;
        float                       input_wait_s = 0.f; // time the last applied input tick waited in the queue
        u32                next_seq      = 0; // first seq not applied yet, 0 = none received yet
        u32                ack_seq       = 0; // last applied seq
        u8                 held_bits     = 0; // held part of the last applied input
        u8                 pending_edges = 0; // edges of inputs merged away, applied with the next one
        bool               buffering     = true; // refilling the jitter buffer
        clock::time_point  last_input_time;

        /* Newest input's client time, echoed back for the client's RTT measurement */
        u32               echo_seq            = 0;
        u32               echo_client_time_ms = 0;
        clock::time_point echo_recv_time;
        float             rtt_s    = 0.f;
        float             interp_s = 0.f;

        [[nodiscard]]
        static std::string key(const sf::IpAddress& addr) {
            return addr.toString();
        }
    };

    net_server(const net_server&) = delete;
    net_server& operator=(const net_server&) = delete;

    ~net_server() {
        if (_gs)
            _gs->sim.remove_tick_begin_callback("net_server");
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

    [[nodiscard]]
    client_t* find_client(const sf::IpAddress& addr) {
        auto found = _clients.find(client_t::key(addr));
        return found == _clients.end() ? nullptr : &found->second;
    }

    /**
     * The game_state must outlive the server (the server hooks into its
     * simulation ticks)
     */
    void update(game_state& gs) {
        if (!_sock.is_open())
            return;

        if (!_gs) {
            _gs = &gs;
            gs.sim.add_tick_begin_callback("net_server", [this](const physic_simulation&, float) { apply_inputs(*_gs); });
        }

        for (auto& datagram : _sock.recv(std::chrono::milliseconds(2)))
            handle_msg(gs, datagram);

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

        /* Once per simulation tick: the state does not change in between */
        auto now = clock::now();
        if (gs.sim.tick() == _last_broadcast_tick && now - _last_broadcast_time < idle_broadcast)
            return;
        _last_broadcast_tick = gs.sim.tick();
        _last_broadcast_time = now;

        broadcast(gs, now);
    }

private:
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
                pack_color(pl.body_color()),
                pack_color(pl.tracer_color()),
                i16(pl.get_group())};
    }

    static state_player make_state_player(u16 id, player& pl) {
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
        st.tick           = ++_broadcast_seq;
        /* Time of the simulated state (the last tick), not of the broadcast */
        st.server_time_ms = u32(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    gs.sim.current_update_time() - _start_time)
                                    .count());

        /* Players and their configs (remembering when a config changed) */
        std::map<u16, known_config> configs;
        for (auto& [name, pl] : gs.players) {
            auto id  = player_id(name);
            auto cfg = make_player_config(id, *pl);
            auto old = _configs.find(id);
            configs[id] = old != _configs.end() && old->second.cfg == cfg ? old->second : known_config{cfg, now};
            st.players.push_back(make_state_player(id, *pl));
        }
        _configs = std::move(configs);

        /* The game mode (remembering when it changed) */
        if (_game_mode.mode != gs.cur_game_mode)
            _game_mode = known_game_mode{gs.cur_game_mode, now};

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
            st.echo_client_time_ms = client.echo_client_time_ms;
            st.echo_hold_ms        = u16(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      clock::now() - client.echo_recv_time).count());

            auto own      = gs.players.find(client.player_name);
            st.own_weapon = own != gs.players.end() ? make_weapon_sync(*own->second) : std::nullopt;
            /* What the client's shots are compensated by (game_state::spawn_bullet): it predicts their hits with it */
            auto lc       = own != gs.players.end() ? own->second->lag_compensation() : player::lag_compensation_t{};
            st.own_lag    = shot_info::pack_forward(std::min(lc.lag, game_state::max_lag_compensation));
            st.own_rewind = shot_info::pack_forward(std::min(lc.rewind, game_state::max_lag_compensation));

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

            _sock.send_to(client.address, client.port, encode_msg(msg_type::state, st));
        }
    }

    /**
     * Simulation tick: apply the next queued input tick of every client.
     * Inputs are applied exactly once and in seq order, so a late or
     * duplicated datagram can never bring back an old input state
     */
    void apply_inputs(game_state& gs) {
        auto now = clock::now();

        for (auto& [key, client] : _clients) {
            if (!client.joined)
                continue;

            auto found = gs.players.find(client.player_name);
            if (found == gs.players.end() || !found->second)
                continue;

            auto& q = client.inputs;

            /* Too many queued ticks (client clock runs faster, burst after a stall): merge the oldest */
            while (q.size() > max_queued_inputs) {
                client.pending_edges |= u8(q.begin()->second.bits & in_edge_bits);
                q.erase(q.begin());
            }

            if (q.empty())
                client.buffering = true;
            else if (q.size() >= target_queued_inputs)
                client.buffering = false;

            u8  bits;
            if (!client.buffering) {
                /* Missing seqs before the head are lost for good (they would be in every newer window) */
                auto head            = q.begin();
                bits                 = head->second.bits | client.pending_edges;
                client.ack_seq       = head->first;
                client.next_seq      = head->first + 1;
                client.held_bits     = u8(head->second.bits & ~in_edge_bits);
                client.pending_edges = 0;
                /* Measured at the tick's time (catch-up ticks run late) */
                client.input_wait_s =
                    std::max(std::chrono::duration<float>(gs.sim.current_update_time() - head->second.received).count(), 0.f);
                q.erase(head);
            }
            else if (q.empty() && now - client.last_input_time > input_timeout) {
                /* Lost client: release everything */
                bits             = 0;
                client.held_bits = 0;
            }
            else {
                /* Starving: hold the last state, no edges, without consuming a seq */
                bits = client.held_bits;
            }

            found->second->last_input_seq(client.ack_seq);
            found->second->apply_input(player_input_state::from_bits(bits));

            /*
             * The shot is forwarded by the time since the client fired it (one
             * way trip + the measured wait in the input queue). The targets are
             * rewound to where the client saw them: its view of the server lags
             * by the one way trip and its interpolation delay, not by the queue
             * wait
             */
            auto one_way = client.rtt_s * 0.5f;
            found->second->lag_compensation({one_way + client.input_wait_s, one_way + client.interp_s});
        }
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
        case msg_type::input: handle_input(datagram.address, msg); break;
        case msg_type::leave: handle_leave(gs, datagram.address); break;
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
        client.next_seq      = 0;
        client.held_bits     = 0;
        client.pending_edges = 0;
        client.buffering     = true;
        client.echo_seq      = 0;

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

    void handle_input(const sf::IpAddress& addr, const net_msg& msg) {
        auto client = find_client(addr);
        if (!client || !client->joined)
            return;

        input in;
        if (!try_read_msg(msg.payload, in) || in.bits.empty())
            return;

        auto now = clock::now();

        /* Newest input: RTT echo and lag compensation parameters */
        if (in.last_seq > client->echo_seq) {
            client->echo_seq            = in.last_seq;
            client->echo_client_time_ms = in.client_time_ms;
            client->echo_recv_time      = now;
            client->rtt_s               = float(in.rtt_ms) / 1000.f;
            client->interp_s            = float(in.interp_ms) / 1000.f;
            client->last_input_time     = now;
        }

        if (client->next_seq == 0)
            client->next_seq = in.first_seq();

        for (u32 i = 0; i < in.bits.size(); ++i) {
            auto seq = in.first_seq() + i;
            if (seq >= client->next_seq)
                client->inputs.emplace(seq, client_t::queued_input{in.bits[i], now});
        }
    }

    void handle_leave(game_state& gs, const sf::IpAddress& addr) {
        auto client = find_client(addr);
        if (!client)
            return;

        if (gs.players.contains(client->player_name)) {
            gs.players.erase(gs.players.find(client->player_name));
            glog().info("net server: player {} deleted (client left)", client->player_name);
        }

        _clients.erase(client_t::key(addr));
        glog().info("net server: client {} left", addr.toString());
    }

    static void apply_player_config(player& pl, const hello& h, u32 magazines_count) {
        pl.set_body("player/body" + std::to_string(h.body_id) + ".png", unpack_color(h.body_color));
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
};

} // namespace dfdh::net
