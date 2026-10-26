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

/**
 * Multiplayer server.
 *
 * Call update(game_state) once per game frame (from the main thread).
 */
class net_server {
public:
    using clock = std::chrono::steady_clock;

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

    struct client_t {
        sf::IpAddress    address;
        u16              port         = 0;
        player_name_t    player_name;
        bool             joined       = false;

        /* Received, not yet applied input ticks (seq -> bits), applied one per tick in seq order */
        std::map<u32, u16> inputs;
        u32                next_seq      = 0; // first seq not applied yet, 0 = none received yet
        u32                ack_seq       = 0; // last applied seq
        u16                held_bits     = 0; // held part of the last applied input
        u16                pending_edges = 0; // edges of inputs merged away, applied with the next one
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

        /* Broadcast the state once per frame */
        if (!_clients.empty()) {
            net::state st;
            st.tick           = ++_broadcast_seq;
            /* Time of the simulated state (the last tick), not of the broadcast */
            st.server_time_ms = u32(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        gs.sim.current_update_time() - _start_time)
                                        .count());
            for (auto& [name, pl] : gs.players)
                st.players.push_back(make_state_player(*pl));

            /*
             * Broadcast all bullets spawned this frame (network players, the
             * host's local player and bots). The client disables local bullet
             * spawning for every server-known player, so there are no
             * duplicates.
             */
            for (auto& be : gs.cur_frame_bullets)
                st.bullets.push_back(bullet_info{std::string(be.owner),
                                                 be.id,
                                                 be.key.pull_seq,
                                                 u16(be.key.shot_index),
                                                 u8(be.key.pellet),
                                                 be.start.x,
                                                 be.start.y,
                                                 be.position.x,
                                                 be.position.y,
                                                 be.velocity.x,
                                                 be.velocity.y,
                                                 be.hit_mass,
                                                 pack_color(be.tracer_color),
                                                 be.gravity});

            /*
             * The set of all live bullet ids: clients drop their local bullets
             * that no longer exist on the server (e.g. hit a player whose
             * position diverged locally)
             */
            for (auto& b : gs.blt_mgr.bullets())
                if (b.net_id() != 0)
                    st.alive_bullet_ids.push_back(b.net_id());

            for (auto& c : gs.crates) {
                auto pos = c->get_position();
                auto vel = c->get_velocity();
                st.crates.push_back(crate_info{c->id(), pos.x, pos.y, vel.x, vel.y,
                                               c->landed() ? 1 : 0});
            }

            /* Broadcast dropped guns (server-authoritative physics) */
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
                                           static_cast<u8>(g->long_shot())});
            }

            for (auto& [key, client] : _clients) {
                if (!client.joined)
                    continue;

                st.ack_seq             = client.ack_seq;
                st.echo_client_time_ms = client.echo_client_time_ms;
                st.echo_hold_ms        = u16(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          clock::now() - client.echo_recv_time).count());
                _sock.send_to(client.address, client.port, encode_msg(msg_type::state, st));
            }
        }
    }

private:
    static state_player make_state_player(player& pl) {
        state_player sp;
        sp.name         = pl.name();
        sp.pistol       = pl.pistol_section();
        sp.face_id      = player_configurator::texture_path_to_index(pl.face_texture_path());
        sp.body_id      = player_configurator::texture_path_to_index(pl.body_texture_path());
        sp.body_color   = pack_color(pl.body_color());
        sp.tracer_color = pack_color(pl.tracer_color());
        auto pos        = pl.get_position();
        auto vel        = pl.get_velocity();
        sp.pos_x        = pos.x;
        sp.pos_y        = pos.y;
        sp.vel_x        = vel.x;
        sp.vel_y        = vel.y;
        auto ph         = pl.get_physic_state();
        sp.jumps_left   = u8(ph.jumps_left);
        sp.lock_y       = ph.lock_y ? 1 : 0;
        sp.accel_lf     = ph.accel_left_f;
        sp.accel_rf     = ph.accel_right_f;
        sp.on_left      = pl.get_on_left() ? 1 : 0;
        sp.shooting     = pl.shotting() ? 1 : 0;
        sp.group        = pl.get_group();
        sp.teleports    = pl.teleports();
        sp.hits         = pl.recent_hit_ids();
        return sp;
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
                client.pending_edges |= u16(q.begin()->second & in_edge_bits);
                q.erase(q.begin());
            }

            if (q.empty())
                client.buffering = true;
            else if (q.size() >= target_queued_inputs)
                client.buffering = false;

            u16 bits;
            if (!client.buffering) {
                /* Missing seqs before the head are lost for good (they would be in every newer window) */
                auto head            = q.begin();
                bits                 = head->second | client.pending_edges;
                client.ack_seq       = head->first;
                client.next_seq      = head->first + 1;
                client.held_bits     = u16(head->second & ~in_edge_bits);
                client.pending_edges = 0;
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

            auto lag = client.rtt_s * 0.5f + float(q.size()) / 60.f;
            found->second->lag_compensation({lag, lag + client.interp_s});
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

        apply_player_config(*pl, h);
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
                client->inputs.emplace(seq, in.bits[i]);
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

    static void apply_player_config(player& pl, const hello& h) {
        pl.set_body("player/body" + std::to_string(h.body_id) + ".png", unpack_color(h.body_color));
        pl.set_face("player/face" + std::to_string(h.face_id) + ".png");
        if (!h.pistol.empty())
            pl.setup_pistol(h.pistol);
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
};

} // namespace dfdh::net
